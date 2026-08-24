// SPDX-License-Identifier: GPL-2.0

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <linux/videodev2.h>

#include "../include/uapi/linux/zzh_mlx90640_meta.h"

#define MAX_VIDEO_NODES 256
#define BUFFER_COUNT 4
#define POLL_TIMEOUT_MS 3000

struct mapped_buffer {
	void *address;
	size_t length;
};

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
static uint16_t from_le16(__le16 value) { return value; }
static uint32_t from_le32(__le32 value) { return value; }
static uint64_t from_le64(__le64 value) { return value; }
#else
static uint16_t from_le16(__le16 value) { return __builtin_bswap16(value); }
static uint32_t from_le32(__le32 value) { return __builtin_bswap32(value); }
static uint64_t from_le64(__le64 value) { return __builtin_bswap64(value); }
#endif

static int ioctl_retry(int fd, unsigned long request, void *argument)
{
	int ret;

	do {
		ret = ioctl(fd, request, argument);
	} while (ret < 0 && errno == EINTR);

	return ret;
}

static uint64_t monotonic_ns(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return 0;
	return (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
}

static uint64_t payload_hash(const void *data, size_t bytes)
{
	const uint8_t *raw = data;
	uint64_t hash = 1469598103934665603ULL;
	size_t i;

	for (i = 0; i < bytes; i++) {
		hash ^= raw[i];
		hash *= 1099511628211ULL;
	}

	return hash;
}

static int effective_caps(const struct v4l2_capability *capability)
{
	if (capability->capabilities & V4L2_CAP_DEVICE_CAPS)
		return capability->device_caps;
	return capability->capabilities;
}

static int find_device(char *path, size_t path_size)
{
	unsigned int index;

	for (index = 0; index < MAX_VIDEO_NODES; index++) {
		struct v4l2_capability capability = { 0 };
		char candidate[32];
		int fd;

		snprintf(candidate, sizeof(candidate), "/dev/video%u", index);
		fd = open(candidate, O_RDWR | O_NONBLOCK);
		if (fd < 0)
			continue;
		if (ioctl_retry(fd, VIDIOC_QUERYCAP, &capability) == 0 &&
		    strcmp((char *)capability.driver, "zzh_mlx90640") == 0) {
			close(fd);
			snprintf(path, path_size, "%s", candidate);
			return 0;
		}
		close(fd);
	}

	return -1;
}

static int validate_meta(const struct zzh_mlx90640_meta_v1 *meta,
			 const struct v4l2_buffer *buffer,
			 unsigned int expected_sequence,
			 double *subpage_gap_ms,
			 uint64_t *hash)
{
	uint32_t flags = from_le32(meta->flags);
	uint16_t first_id = from_le16(meta->subpage[0].subpage_id);
	uint16_t second_id = from_le16(meta->subpage[1].subpage_id);
	uint64_t first_ready = from_le64(meta->subpage[0].ready_timestamp_ns);
	uint64_t second_ready = from_le64(meta->subpage[1].ready_timestamp_ns);
	uint64_t first_done = from_le64(meta->subpage[0].read_done_timestamp_ns);
	uint64_t second_done = from_le64(meta->subpage[1].read_done_timestamp_ns);
	uint64_t v4l2_timestamp;
	uint64_t timestamp_delta;

	if (from_le32(meta->magic) != ZZH_MLX90640_META_MAGIC ||
	    from_le16(meta->version) != ZZH_MLX90640_META_VERSION ||
	    from_le16(meta->header_size) != ZZH_MLX90640_META_V1_HEADER_SIZE ||
	    from_le32(meta->buffer_size) != ZZH_MLX90640_META_V1_SIZE ||
	    from_le32(meta->pair_sequence) != expected_sequence ||
	    buffer->sequence != expected_sequence)
		return -1;
	if ((flags & (ZZH_MLX90640_META_FLAG_CHESS |
		      ZZH_MLX90640_META_FLAG_CONSECUTIVE)) !=
	    (ZZH_MLX90640_META_FLAG_CHESS |
	     ZZH_MLX90640_META_FLAG_CONSECUTIVE))
		return -1;
	if (first_id > 1 || second_id > 1 || first_id == second_id)
		return -1;
	if (!first_ready || second_ready <= first_ready ||
	    first_done < first_ready || second_done < second_ready)
		return -1;

	*subpage_gap_ms = (second_ready - first_ready) / 1000000.0;
	if (*subpage_gap_ms < 30.0 || *subpage_gap_ms > 100.0)
		return -1;
	if (!(buffer->flags & V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC))
		return -1;
	v4l2_timestamp = (uint64_t)buffer->timestamp.tv_sec * 1000000000ULL +
		(uint64_t)buffer->timestamp.tv_usec * 1000ULL;
	timestamp_delta = v4l2_timestamp > second_ready ?
		v4l2_timestamp - second_ready : second_ready - v4l2_timestamp;
	if (timestamp_delta > 1000000ULL)
		return -1;
	*hash = payload_hash(meta, ZZH_MLX90640_META_V1_SIZE);

	return 0;
}

static int run_cycle(const char *path, unsigned int pairs,
		     unsigned int cycle, unsigned int hold_ms)
{
	struct mapped_buffer mapped[BUFFER_COUNT] = { 0 };
	struct v4l2_capability capability = { 0 };
	struct v4l2_requestbuffers request = { 0 };
	struct v4l2_format format = { 0 };
	enum v4l2_buf_type type = V4L2_BUF_TYPE_META_CAPTURE;
	unsigned int queued = 0;
	unsigned int captured = 0;
	bool streaming = false;
	int result = -1;
	int fd;

	fd = open(path, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
		return -1;
	}
	if (ioctl_retry(fd, VIDIOC_QUERYCAP, &capability) < 0) {
		perror("VIDIOC_QUERYCAP");
		goto out;
	}
	if ((effective_caps(&capability) &
	     (V4L2_CAP_META_CAPTURE | V4L2_CAP_STREAMING)) !=
	    (V4L2_CAP_META_CAPTURE | V4L2_CAP_STREAMING)) {
		fprintf(stderr, "%s lacks Meta Capture/Streaming\n", path);
		goto out;
	}

	format.type = V4L2_BUF_TYPE_META_CAPTURE;
	if (ioctl_retry(fd, VIDIOC_G_FMT, &format) < 0) {
		perror("VIDIOC_G_FMT");
		goto out;
	}
	if (format.fmt.meta.dataformat != V4L2_META_FMT_ZZH_MLX90640 ||
	    format.fmt.meta.buffersize != ZZH_MLX90640_META_V1_SIZE) {
		fprintf(stderr, "unexpected Meta format 0x%08x size %u\n",
			format.fmt.meta.dataformat, format.fmt.meta.buffersize);
		goto out;
	}

	request.count = BUFFER_COUNT;
	request.type = V4L2_BUF_TYPE_META_CAPTURE;
	request.memory = V4L2_MEMORY_MMAP;
	if (ioctl_retry(fd, VIDIOC_REQBUFS, &request) < 0) {
		perror("VIDIOC_REQBUFS");
		goto out;
	}
	if (request.count < 2 || request.count > BUFFER_COUNT) {
		fprintf(stderr, "unexpected buffer count %u\n", request.count);
		goto out;
	}

	for (queued = 0; queued < request.count; queued++) {
		struct v4l2_buffer buffer = { 0 };

		buffer.type = V4L2_BUF_TYPE_META_CAPTURE;
		buffer.memory = V4L2_MEMORY_MMAP;
		buffer.index = queued;
		if (ioctl_retry(fd, VIDIOC_QUERYBUF, &buffer) < 0) {
			perror("VIDIOC_QUERYBUF");
			goto out;
		}
		mapped[queued].length = buffer.length;
		mapped[queued].address = mmap(NULL, buffer.length,
					      PROT_READ | PROT_WRITE,
					      MAP_SHARED, fd, buffer.m.offset);
		if (mapped[queued].address == MAP_FAILED) {
			mapped[queued].address = NULL;
			perror("mmap");
			goto out;
		}
		if (ioctl_retry(fd, VIDIOC_QBUF, &buffer) < 0) {
			perror("VIDIOC_QBUF");
			goto out;
		}
	}

	if (ioctl_retry(fd, VIDIOC_STREAMON, &type) < 0) {
		perror("VIDIOC_STREAMON");
		goto out;
	}
	streaming = true;

	while (captured < pairs) {
		struct zzh_mlx90640_meta_v1 *meta;
		struct v4l2_buffer buffer = { 0 };
		struct pollfd poll_fd = {
			.fd = fd,
			.events = POLLIN | POLLPRI,
		};
		double gap_ms;
		uint64_t hash;
		int poll_ret;

		poll_ret = poll(&poll_fd, 1, POLL_TIMEOUT_MS);
		if (poll_ret <= 0) {
			fprintf(stderr, "poll failed/timeout: ret=%d errno=%s\n",
				poll_ret, strerror(errno));
			goto out;
		}
		if (poll_fd.revents & POLLERR) {
			fprintf(stderr, "poll reported POLLERR\n");
			goto out;
		}

		buffer.type = V4L2_BUF_TYPE_META_CAPTURE;
		buffer.memory = V4L2_MEMORY_MMAP;
		if (ioctl_retry(fd, VIDIOC_DQBUF, &buffer) < 0) {
			if (errno == EAGAIN)
				continue;
			perror("VIDIOC_DQBUF");
			goto out;
		}
		if (buffer.index >= request.count ||
		    buffer.bytesused != ZZH_MLX90640_META_V1_SIZE) {
			fprintf(stderr, "bad DQBUF index/bytesused: %u/%u\n",
				buffer.index, buffer.bytesused);
			goto out;
		}
		meta = mapped[buffer.index].address;
		if (validate_meta(meta, &buffer, captured, &gap_ms, &hash) < 0) {
			fprintf(stderr, "Meta validation failed at pair %u\n", captured);
			goto out;
		}
		printf("CYCLE=%u PAIR=%u BUFFER=%u SUBPAGES=%u/%u ",
		       cycle, captured, buffer.index,
		       from_le16(meta->subpage[0].subpage_id),
		       from_le16(meta->subpage[1].subpage_id));
		printf("GAP_MS=%.3f HASH=%016llx\n", gap_ms,
		       (unsigned long long)hash);
		if (hold_ms)
			usleep(hold_ms * 1000U);
		if (ioctl_retry(fd, VIDIOC_QBUF, &buffer) < 0) {
			perror("VIDIOC_QBUF recycle");
			goto out;
		}
		captured++;
	}

	if (ioctl_retry(fd, VIDIOC_STREAMOFF, &type) < 0) {
		perror("VIDIOC_STREAMOFF");
		goto out;
	}
	streaming = false;
	result = 0;

out:
	if (streaming)
		ioctl_retry(fd, VIDIOC_STREAMOFF, &type);
	for (queued = 0; queued < request.count && queued < BUFFER_COUNT;
	     queued++)
		if (mapped[queued].address)
			munmap(mapped[queued].address, mapped[queued].length);
	if (request.count) {
		request.count = 0;
		if (ioctl_retry(fd, VIDIOC_REQBUFS, &request) < 0 && result == 0) {
			perror("VIDIOC_REQBUFS release");
			result = -1;
		}
	}
	close(fd);
	return result;
}

static int parse_count(const char *text, unsigned int minimum,
		       unsigned int maximum, unsigned int *value)
{
	char *end;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno || *end || parsed < minimum || parsed > maximum)
		return -1;
	*value = parsed;
	return 0;
}

int main(int argc, char **argv)
{
	char device[32] = { 0 };
	unsigned int pairs = 20;
	unsigned int cycles = 3;
	unsigned int hold_ms = 0;
	unsigned int cycle;
	uint64_t start_ns;

	if (argc > 5) {
		fprintf(stderr,
			"usage: %s [pairs] [cycles] [auto|/dev/videoN] [hold-ms]\n",
			argv[0]);
		return EXIT_FAILURE;
	}
	if (argc >= 2 && parse_count(argv[1], 1, 1000000, &pairs) < 0) {
		fprintf(stderr, "pairs must be in the range 1..1000000\n");
		return EXIT_FAILURE;
	}
	if (argc >= 3 && parse_count(argv[2], 1, 1000, &cycles) < 0) {
		fprintf(stderr, "cycles must be in the range 1..1000\n");
		return EXIT_FAILURE;
	}
	if (argc >= 4 && strcmp(argv[3], "auto") != 0) {
		snprintf(device, sizeof(device), "%s", argv[3]);
	} else if (find_device(device, sizeof(device)) < 0) {
		fprintf(stderr, "zzh_mlx90640 Meta node not found\n");
		return EXIT_FAILURE;
	}
	if (argc == 5 && parse_count(argv[4], 0, 5000, &hold_ms) < 0) {
		fprintf(stderr, "hold-ms must be in the range 0..5000\n");
		return EXIT_FAILURE;
	}

	if (sizeof(struct zzh_mlx90640_meta_v1) !=
	    ZZH_MLX90640_META_V1_SIZE) {
		fprintf(stderr, "userspace ABI size mismatch: %zu\n",
			sizeof(struct zzh_mlx90640_meta_v1));
		return EXIT_FAILURE;
	}

	printf("DEVICE=%s\n", device);
	printf("META_SIZE=%u\n", ZZH_MLX90640_META_V1_SIZE);
	printf("PAIRS_PER_CYCLE=%u\n", pairs);
	printf("CYCLES=%u\n", cycles);
	printf("HOLD_MS=%u\n", hold_ms);
	start_ns = monotonic_ns();
	for (cycle = 0; cycle < cycles; cycle++)
		if (run_cycle(device, pairs, cycle, hold_ms) < 0)
			return EXIT_FAILURE;
	printf("TOTAL_PAIRS=%u\n", pairs * cycles);
	printf("ELAPSED_SECONDS=%.3f\n",
	       (monotonic_ns() - start_ns) / 1000000000.0);
	printf("STAGE5_RUNTIME_CHECK=PASS\n");
	return EXIT_SUCCESS;
}
