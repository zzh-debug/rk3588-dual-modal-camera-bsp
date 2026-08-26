// SPDX-License-Identifier: GPL-2.0

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/media-bus-format.h>
#include <linux/videodev2.h>
#include <linux/rk-camera-module.h>

static int ioctl_retry(int fd, unsigned long request, void *argument)
{
	int ret;

	do {
		ret = ioctl(fd, request, argument);
	} while (ret < 0 && errno == EINTR);

	return ret;
}

static int require_equal(const char *field, const char *actual,
			 const char *expected)
{
	if (strcmp(actual, expected) == 0)
		return 0;

	fprintf(stderr, "%s mismatch: expected '%s', got '%s'\n",
		field, expected, actual);
	return -1;
}

int main(int argc, char **argv)
{
	const char *device;
	struct rkmodule_channel_info channel = { 0 };
	struct rkmodule_hdr_cfg hdr = { 0 };
	struct rkmodule_inf info = { 0 };
	int fd;
	int result = EXIT_FAILURE;

	if (argc != 2) {
		fprintf(stderr, "usage: %s /dev/v4l-subdevX\n", argv[0]);
		return EXIT_FAILURE;
	}
	device = argv[1];

	fd = open(device, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", device, strerror(errno));
		return EXIT_FAILURE;
	}

	if (ioctl_retry(fd, RKMODULE_GET_MODULE_INFO, &info) < 0) {
		fprintf(stderr, "RKMODULE_GET_MODULE_INFO failed: %s\n",
			strerror(errno));
		goto out;
	}
	if (require_equal("sensor", info.base.sensor, "imx415") ||
	    require_equal("module", info.base.module, "CMK-OT2022-PX1") ||
	    require_equal("lens", info.base.lens,
			  "IR0147-50IRC-8M-F20"))
		goto out;

	if (ioctl_retry(fd, RKMODULE_GET_HDR_CFG, &hdr) < 0) {
		fprintf(stderr, "RKMODULE_GET_HDR_CFG failed: %s\n",
			strerror(errno));
		goto out;
	}
	if (hdr.hdr_mode != NO_HDR || hdr.esp.mode != HDR_NORMAL_VC) {
		fprintf(stderr, "unexpected HDR contract: hdr=%u esp=%u\n",
			hdr.hdr_mode, hdr.esp.mode);
		goto out;
	}
	if (ioctl_retry(fd, RKMODULE_SET_HDR_CFG, &hdr) < 0) {
		fprintf(stderr, "RKMODULE_SET_HDR_CFG(NO_HDR) failed: %s\n",
			strerror(errno));
		goto out;
	}

	channel.index = 0;
	if (ioctl_retry(fd, RKMODULE_GET_CHANNEL_INFO, &channel) < 0) {
		fprintf(stderr, "RKMODULE_GET_CHANNEL_INFO failed: %s\n",
			strerror(errno));
		goto out;
	}
	if (channel.vc != 0 || channel.width != 3864 ||
	    channel.height != 2192 ||
	    channel.bus_fmt != MEDIA_BUS_FMT_SGBRG10_1X10) {
		fprintf(stderr,
			"unexpected channel: vc=%u size=%ux%u bus_fmt=0x%x\n",
			channel.vc, channel.width, channel.height,
			channel.bus_fmt);
		goto out;
	}

	printf("DEVICE=%s\n", device);
	printf("SENSOR=%s\n", info.base.sensor);
	printf("MODULE=%s\n", info.base.module);
	printf("LENS=%s\n", info.base.lens);
	printf("HDR_MODE=%u ESP_MODE=%u\n", hdr.hdr_mode, hdr.esp.mode);
	printf("CHANNEL0=vc%u %ux%u bus_fmt=0x%x\n",
	       channel.vc, channel.width, channel.height, channel.bus_fmt);
	printf("IMX415_AIQ_MODULE_CONTRACT=PASS\n");
	result = EXIT_SUCCESS;

out:
	close(fd);
	return result;
}
