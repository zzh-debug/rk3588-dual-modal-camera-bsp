// SPDX-License-Identifier: GPL-2.0

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define MLX90640_I2C_ADDRESS          0x33
#define MLX90640_PIXEL_START          0x0400
#define MLX90640_PIXEL_WORDS          768
#define MLX90640_AUX_START            0x0700
#define MLX90640_AUX_WORDS            64
#define MLX90640_STATUS               0x8000
#define MLX90640_CONTROL              0x800d
#define MLX90640_STATUS_DATA_READY    0x0008
#define MLX90640_STATUS_CLEAR         0x0030
#define MLX90640_STATUS_SUBPAGE       0x0001
#define MLX90640_CONTROL_REFRESH_MASK 0x0380
#define MLX90640_CONTROL_CHESS        0x1000
#define MLX90640_CHUNK_WORDS          32
#define MLX90640_I2C_RETRIES          3
#define MLX90640_POLL_US              1000
#define MLX90640_TIMEOUT_MS           2000
#define MLX90640_CONFIG_WARMUP_FRAMES 2

static volatile sig_atomic_t stop_requested;

static void handle_signal(int signal_number)
{
	(void)signal_number;
	stop_requested = 1;
}

static uint64_t monotonic_ns(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
		perror("clock_gettime");
		return 0;
	}

	return (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
}

static int transfer_retry(int fd, struct i2c_msg *messages,
			  unsigned int count)
{
	struct i2c_rdwr_ioctl_data transaction = {
		.msgs = messages,
		.nmsgs = count,
	};
	int saved_errno = EIO;
	unsigned int attempt;

	for (attempt = 0; attempt < MLX90640_I2C_RETRIES; attempt++) {
		int ret = ioctl(fd, I2C_RDWR, &transaction);

		if (ret == (int)count)
			return 0;
		if (ret >= 0)
			saved_errno = EIO;
		else
			saved_errno = errno;
		if (saved_errno == EINTR)
			continue;
		usleep(2000);
	}

	errno = saved_errno;
	return -1;
}

static int read_words(int fd, uint16_t start, uint16_t *values,
		      size_t count)
{
	uint8_t address[2];
	uint8_t raw[MLX90640_CHUNK_WORDS * 2];
	size_t offset = 0;

	while (offset < count) {
		struct i2c_msg messages[2];
		size_t words = count - offset;
		size_t i;
		uint16_t reg = start + offset;

		if (words > MLX90640_CHUNK_WORDS)
			words = MLX90640_CHUNK_WORDS;

		address[0] = reg >> 8;
		address[1] = reg & 0xff;
		messages[0].addr = MLX90640_I2C_ADDRESS;
		messages[0].flags = 0;
		messages[0].len = sizeof(address);
		messages[0].buf = address;
		messages[1].addr = MLX90640_I2C_ADDRESS;
		messages[1].flags = I2C_M_RD;
		messages[1].len = words * 2;
		messages[1].buf = raw;

		if (transfer_retry(fd, messages, 2) < 0) {
			fprintf(stderr,
				"read 0x%04x..0x%04x from 0x%02x failed: %s\n",
				(unsigned int)reg,
				(unsigned int)(reg + words - 1),
				MLX90640_I2C_ADDRESS, strerror(errno));
			return -1;
		}

		for (i = 0; i < words; i++)
			values[offset + i] =
				((uint16_t)raw[i * 2] << 8) | raw[i * 2 + 1];
		offset += words;
	}

	return 0;
}

static int write_word(int fd, uint16_t reg, uint16_t value)
{
	uint8_t raw[4] = {
		reg >> 8,
		reg & 0xff,
		value >> 8,
		value & 0xff,
	};
	struct i2c_msg message = {
		.addr = MLX90640_I2C_ADDRESS,
		.flags = 0,
		.len = sizeof(raw),
		.buf = raw,
	};

	if (transfer_retry(fd, &message, 1) < 0) {
		fprintf(stderr, "write 0x%04x=0x%04x to 0x%02x failed: %s\n",
			(unsigned int)reg, (unsigned int)value,
			MLX90640_I2C_ADDRESS, strerror(errno));
		return -1;
	}

	return 0;
}

static int write_control_verified(int fd, uint16_t value)
{
	uint16_t readback;

	if (write_word(fd, MLX90640_CONTROL, value) < 0)
		return -1;
	if (read_words(fd, MLX90640_CONTROL, &readback, 1) < 0)
		return -1;
	if (readback != value) {
		fprintf(stderr,
			"control readback mismatch: wrote 0x%04x, read 0x%04x\n",
			(unsigned int)value, (unsigned int)readback);
		errno = EIO;
		return -1;
	}

	return 0;
}

static int wait_data_ready(int fd, uint16_t *status, uint64_t *ready_ns)
{
	uint64_t start_ns = monotonic_ns();

	if (!start_ns)
		return -1;

	while (!stop_requested) {
		uint64_t now_ns;

		if (read_words(fd, MLX90640_STATUS, status, 1) < 0)
			return -1;
		if (*status & MLX90640_STATUS_DATA_READY) {
			*ready_ns = monotonic_ns();
			return *ready_ns ? 0 : -1;
		}

		now_ns = monotonic_ns();
		if (!now_ns)
			return -1;
		if (now_ns - start_ns >
		    (uint64_t)MLX90640_TIMEOUT_MS * 1000000ULL) {
			fprintf(stderr, "timed out waiting for MLX90640 data-ready\n");
			errno = ETIMEDOUT;
			return -1;
		}
		usleep(MLX90640_POLL_US);
	}

	errno = EINTR;
	return -1;
}

static int validate_pixels(const uint16_t *pixels, unsigned int subpage)
{
	unsigned int line;

	for (line = 0; line < 24; line++) {
		if (pixels[line * 32] == 0x7fff && line % 2 == subpage)
			return -1;
	}

	return 0;
}

static int validate_aux(const uint16_t *aux)
{
	unsigned int i;

	if (aux[0] == 0x7fff)
		return -1;
	for (i = 8; i < 19; i++)
		if (aux[i] == 0x7fff)
			return -1;
	for (i = 20; i < 23; i++)
		if (aux[i] == 0x7fff)
			return -1;
	for (i = 24; i < 33; i++)
		if (aux[i] == 0x7fff)
			return -1;
	for (i = 40; i < 51; i++)
		if (aux[i] == 0x7fff)
			return -1;
	for (i = 52; i < 55; i++)
		if (aux[i] == 0x7fff)
			return -1;
	for (i = 56; i < 64; i++)
		if (aux[i] == 0x7fff)
			return -1;

	return 0;
}

static uint64_t hash_words(const uint16_t *values, size_t count)
{
	uint64_t hash = 1469598103934665603ULL;
	size_t i;

	for (i = 0; i < count; i++) {
		hash ^= values[i] >> 8;
		hash *= 1099511628211ULL;
		hash ^= values[i] & 0xff;
		hash *= 1099511628211ULL;
	}

	return hash;
}

static double refresh_hz_from_code(unsigned int code)
{
	static const double rates[] = { 0.5, 1, 2, 4, 8, 16, 32, 64 };

	return rates[code & 7];
}

static int parse_refresh(const char *text)
{
	if (!strcmp(text, "keep"))
		return -1;
	if (!strcmp(text, "8"))
		return 4;
	if (!strcmp(text, "16"))
		return 5;

	return -2;
}

static int parse_frame_count(const char *text)
{
	char *end;
	long count;

	errno = 0;
	count = strtol(text, &end, 10);
	if (errno || *end || count < 2 || count > 1000)
		return -1;

	return count;
}

int main(int argc, char **argv)
{
	uint16_t pixels[MLX90640_PIXEL_WORDS];
	uint16_t aux[MLX90640_AUX_WORDS];
	uint16_t original_control;
	uint16_t active_control;
	uint16_t desired_control;
	uint64_t previous_ready_ns = 0;
	double expected_period_ms;
	double max_gap_ms = 0;
	unsigned int previous_subpage = 0;
	unsigned int alternation_errors = 0;
	unsigned int gap_errors = 0;
	unsigned int validation_errors = 0;
	unsigned int pair_count = 0;
	bool have_previous = false;
	bool control_modified = false;
	unsigned long functions;
	int requested_refresh;
	int frame_count = 20;
	int captured = 0;
	int warmup;
	int result = EXIT_FAILURE;
	int fd;

	if (argc < 3 || argc > 4) {
		fprintf(stderr,
			"usage: %s /dev/i2c-N <keep|8|16> [frame-count]\n",
			argv[0]);
		return EXIT_FAILURE;
	}

	requested_refresh = parse_refresh(argv[2]);
	if (requested_refresh == -2) {
		fprintf(stderr, "refresh must be keep, 8, or 16 Hz\n");
		return EXIT_FAILURE;
	}
	if (argc == 4) {
		frame_count = parse_frame_count(argv[3]);
		if (frame_count < 0) {
			fprintf(stderr, "frame-count must be in the range 2..1000\n");
			return EXIT_FAILURE;
		}
	}

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	fd = open(argv[1], O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", argv[1], strerror(errno));
		return EXIT_FAILURE;
	}
	if (ioctl(fd, I2C_FUNCS, &functions) < 0) {
		fprintf(stderr, "I2C_FUNCS failed: %s\n", strerror(errno));
		goto out_close;
	}
	if (!(functions & I2C_FUNC_I2C)) {
		fprintf(stderr, "%s lacks plain I2C transfer support\n", argv[1]);
		goto out_close;
	}
	if (read_words(fd, MLX90640_CONTROL, &original_control, 1) < 0)
		goto out_close;

	desired_control = original_control;
	if (requested_refresh >= 0) {
		desired_control &= ~MLX90640_CONTROL_REFRESH_MASK;
		desired_control |= requested_refresh << 7;
		desired_control |= MLX90640_CONTROL_CHESS;
	}
	if (desired_control != original_control) {
		if (write_control_verified(fd, desired_control) < 0)
			goto out_close;
		control_modified = true;
	}
	if (read_words(fd, MLX90640_CONTROL, &active_control, 1) < 0)
		goto out_restore;
	if (!(active_control & MLX90640_CONTROL_CHESS)) {
		fprintf(stderr, "sensor is not in Chess mode (control=0x%04x)\n",
			(unsigned int)active_control);
		goto out_restore;
	}

	expected_period_ms = 1000.0 /
		refresh_hz_from_code((active_control &
				      MLX90640_CONTROL_REFRESH_MASK) >> 7);

	printf("I2C_DEVICE=%s\n", argv[1]);
	printf("I2C_ADDRESS=0x%02x\n", MLX90640_I2C_ADDRESS);
	printf("ORIGINAL_CONTROL=0x%04x\n", (unsigned int)original_control);
	printf("ACTIVE_CONTROL=0x%04x\n", (unsigned int)active_control);
	printf("MODE=CHESS\n");
	printf("REFRESH_HZ=%.1f\n",
	       refresh_hz_from_code((active_control &
				     MLX90640_CONTROL_REFRESH_MASK) >> 7));
	printf("EXPECTED_SUBPAGE_PERIOD_MS=%.3f\n", expected_period_ms);

	/* Discard a stale ready indication before starting the measured run. */
	if (write_word(fd, MLX90640_STATUS, MLX90640_STATUS_CLEAR) < 0)
		goto out_restore;

	/*
	 * A refresh-rate or mode update can leave the first two subpages with
	 * transition data. Drain them before starting the measured sequence.
	 */
	for (warmup = 0;
	     control_modified && warmup < MLX90640_CONFIG_WARMUP_FRAMES;
	     warmup++) {
		uint16_t status;
		uint16_t frame_control;
		uint64_t ready_ns;
		uint64_t read_start_ns;
		uint64_t read_end_ns;
		unsigned int subpage;
		int valid;

		if (wait_data_ready(fd, &status, &ready_ns) < 0)
			goto out_restore;
		subpage = status & MLX90640_STATUS_SUBPAGE;
		if (write_word(fd, MLX90640_STATUS,
			       MLX90640_STATUS_CLEAR) < 0)
			goto out_restore;
		read_start_ns = monotonic_ns();
		if (!read_start_ns ||
		    read_words(fd, MLX90640_PIXEL_START, pixels,
			       MLX90640_PIXEL_WORDS) < 0 ||
		    read_words(fd, MLX90640_AUX_START, aux,
			       MLX90640_AUX_WORDS) < 0 ||
		    read_words(fd, MLX90640_CONTROL, &frame_control, 1) < 0)
			goto out_restore;
		read_end_ns = monotonic_ns();
		if (!read_end_ns)
			goto out_restore;
		if (frame_control != active_control) {
			fprintf(stderr,
				"control changed during warmup: 0x%04x -> 0x%04x\n",
				(unsigned int)active_control,
				(unsigned int)frame_control);
			goto out_restore;
		}
		valid = validate_pixels(pixels, subpage) == 0 &&
			validate_aux(aux) == 0;
		printf("WARMUP_FRAME=%d SUBPAGE=%u STATUS=0x%04x READY_NS=%"
		       PRIu64 " READ_MS=%.3f VALID=%d\n",
		       warmup, subpage, (unsigned int)status, ready_ns,
		       (read_end_ns - read_start_ns) / 1000000.0, valid);
	}

	for (captured = 0; captured < frame_count && !stop_requested;
	     captured++) {
		uint16_t status;
		uint16_t frame_control;
		uint64_t ready_ns;
		uint64_t read_start_ns;
		uint64_t read_end_ns;
		uint64_t frame_hash;
		double gap_ms = 0;
		double read_ms;
		unsigned int subpage;
		int pixel_valid;
		int aux_valid;

		if (wait_data_ready(fd, &status, &ready_ns) < 0)
			goto out_restore;
		subpage = status & MLX90640_STATUS_SUBPAGE;
		if (write_word(fd, MLX90640_STATUS,
			       MLX90640_STATUS_CLEAR) < 0)
			goto out_restore;

		read_start_ns = monotonic_ns();
		if (!read_start_ns ||
		    read_words(fd, MLX90640_PIXEL_START, pixels,
			       MLX90640_PIXEL_WORDS) < 0 ||
		    read_words(fd, MLX90640_AUX_START, aux,
			       MLX90640_AUX_WORDS) < 0 ||
		    read_words(fd, MLX90640_CONTROL, &frame_control, 1) < 0)
			goto out_restore;
		read_end_ns = monotonic_ns();
		if (!read_end_ns)
			goto out_restore;

		pixel_valid = validate_pixels(pixels, subpage) == 0;
		aux_valid = validate_aux(aux) == 0;
		if (!pixel_valid || !aux_valid)
			validation_errors++;
		if (frame_control != active_control) {
			fprintf(stderr,
				"control changed during capture: 0x%04x -> 0x%04x\n",
				(unsigned int)active_control,
				(unsigned int)frame_control);
			goto out_restore;
		}

		if (have_previous) {
			gap_ms = (ready_ns - previous_ready_ns) / 1000000.0;
			if (gap_ms > max_gap_ms)
				max_gap_ms = gap_ms;
			if (subpage == previous_subpage)
				alternation_errors++;
			if (gap_ms > expected_period_ms * 1.75)
				gap_errors++;
			if ((captured & 1) && subpage != previous_subpage)
				pair_count++;
		}
		previous_ready_ns = ready_ns;
		previous_subpage = subpage;
		have_previous = true;

		read_ms = (read_end_ns - read_start_ns) / 1000000.0;
		frame_hash = hash_words(pixels, MLX90640_PIXEL_WORDS);
		frame_hash ^= hash_words(aux, MLX90640_AUX_WORDS);
		printf("FRAME=%d SUBPAGE=%u STATUS=0x%04x READY_NS=%" PRIu64
		       " GAP_MS=%.3f READ_MS=%.3f VALID=%d HASH=%016" PRIx64
		       "\n",
		       captured, subpage, (unsigned int)status, ready_ns,
		       gap_ms, read_ms, pixel_valid && aux_valid, frame_hash);
	}

	if (stop_requested) {
		fprintf(stderr, "capture interrupted after %d frames\n", captured);
		result = 130;
		goto out_restore;
	}

	printf("FRAMES=%d\n", captured);
	printf("PAIRS=%u\n", pair_count);
	printf("ALTERNATION_ERRORS=%u\n", alternation_errors);
	printf("GAP_ERRORS=%u\n", gap_errors);
	printf("VALIDATION_ERRORS=%u\n", validation_errors);
	printf("MAX_GAP_MS=%.3f\n", max_gap_ms);
	result = alternation_errors || gap_errors || validation_errors ?
		EXIT_FAILURE : EXIT_SUCCESS;

out_restore:
	if (control_modified) {
		if (write_control_verified(fd, original_control) < 0) {
			fprintf(stderr,
				"failed to restore original control 0x%04x\n",
				(unsigned int)original_control);
			result = EXIT_FAILURE;
		} else {
			printf("RESTORED_CONTROL=0x%04x\n",
			       (unsigned int)original_control);
		}
	}
out_close:
	close(fd);
	return result;
}
