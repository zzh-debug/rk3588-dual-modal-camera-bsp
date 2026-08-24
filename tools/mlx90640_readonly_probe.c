// SPDX-License-Identifier: GPL-2.0

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define MLX90640_I2C_ADDRESS	0x33
#define MLX90640_STATUS		0x8000
#define MLX90640_CONTROL		0x800d
#define MLX90640_EEPROM_START	0x2400
#define MLX90640_EEPROM_WORDS	832
#define MLX90640_ID_OFFSET	7
#define MLX90640_ID_WORDS	3
#define MLX90640_CHUNK_WORDS	32

/* MLX90640 VCC/GND are external board wiring; this tool only performs I2C reads. */

static double monotonic_seconds(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
		perror("clock_gettime");
		exit(EXIT_FAILURE);
	}

	return now.tv_sec + now.tv_nsec / 1000000000.0;
}

static int read_words(int fd, uint16_t start, uint16_t *values, size_t count)
{
	uint8_t address[2];
	uint8_t raw[MLX90640_CHUNK_WORDS * 2];
	size_t offset = 0;

	while (offset < count) {
		struct i2c_rdwr_ioctl_data transaction;
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
		transaction.msgs = messages;
		transaction.nmsgs = 2;

		if (ioctl(fd, I2C_RDWR, &transaction) < 0) {
			fprintf(stderr,
				"read 0x%04x..0x%04x from 0x%02x failed: %s\n",
				(unsigned int)reg,
				(unsigned int)(reg + words - 1),
				MLX90640_I2C_ADDRESS, strerror(errno));
			return -1;
		}

		for (i = 0; i < words; i++)
			values[offset + i] = (raw[i * 2] << 8) | raw[i * 2 + 1];
		offset += words;
	}

	return 0;
}

static int write_eeprom_be(const char *path, const uint16_t *values)
{
	uint8_t raw[MLX90640_EEPROM_WORDS * 2];
	size_t i;
	ssize_t written;
	int fd;

	for (i = 0; i < MLX90640_EEPROM_WORDS; i++) {
		raw[i * 2] = values[i] >> 8;
		raw[i * 2 + 1] = values[i] & 0xff;
	}

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
		return -1;
	}
	written = write(fd, raw, sizeof(raw));
	if (written != (ssize_t)sizeof(raw)) {
		fprintf(stderr, "write %s failed: %s\n", path,
			written < 0 ? strerror(errno) : "short write");
		close(fd);
		return -1;
	}
	if (close(fd) < 0) {
		fprintf(stderr, "close %s failed: %s\n", path, strerror(errno));
		return -1;
	}

	return 0;
}

int main(int argc, char **argv)
{
	uint16_t first[MLX90640_EEPROM_WORDS];
	uint16_t second[MLX90640_EEPROM_WORDS];
	uint16_t status;
	uint16_t control;
	unsigned long functions;
	double first_start;
	double first_end;
	double second_end;
	int fd;

	if (argc != 2 && argc != 3) {
		fprintf(stderr, "usage: %s /dev/i2c-N [eeprom-be.bin]\n", argv[0]);
		return EXIT_FAILURE;
	}

	fd = open(argv[1], O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", argv[1], strerror(errno));
		return EXIT_FAILURE;
	}
	if (ioctl(fd, I2C_FUNCS, &functions) < 0) {
		fprintf(stderr, "I2C_FUNCS failed: %s\n", strerror(errno));
		close(fd);
		return EXIT_FAILURE;
	}
	if (!(functions & I2C_FUNC_I2C)) {
		fprintf(stderr, "%s lacks plain I2C transfer support\n", argv[1]);
		close(fd);
		return EXIT_FAILURE;
	}

	if (read_words(fd, MLX90640_STATUS, &status, 1) < 0) {
		close(fd);
		return 2;
	}
	if (read_words(fd, MLX90640_CONTROL, &control, 1) < 0) {
		close(fd);
		return 2;
	}

	first_start = monotonic_seconds();
	if (read_words(fd, MLX90640_EEPROM_START, first,
		       MLX90640_EEPROM_WORDS) < 0) {
		close(fd);
		return 2;
	}
	first_end = monotonic_seconds();
	if (read_words(fd, MLX90640_EEPROM_START, second,
		       MLX90640_EEPROM_WORDS) < 0) {
		close(fd);
		return 2;
	}
	second_end = monotonic_seconds();
	close(fd);

	printf("I2C_DEVICE=%s\n", argv[1]);
	printf("I2C_ADDRESS=0x%02x\n", MLX90640_I2C_ADDRESS);
	printf("STATUS=0x%04x\n", (unsigned int)status);
	printf("CONTROL=0x%04x\n", (unsigned int)control);
	printf("ID=0x%04x-0x%04x-0x%04x\n",
	       (unsigned int)first[MLX90640_ID_OFFSET],
	       (unsigned int)first[MLX90640_ID_OFFSET + 1],
	       (unsigned int)first[MLX90640_ID_OFFSET + 2]);
	printf("EEPROM_WORDS=%u\n", MLX90640_EEPROM_WORDS);
	printf("EEPROM_CHUNK_WORDS=%u\n", MLX90640_CHUNK_WORDS);
	printf("EEPROM_FIRST_SECONDS=%.6f\n", first_end - first_start);
	printf("EEPROM_SECOND_SECONDS=%.6f\n", second_end - first_end);
	printf("EEPROM_MATCH=%d\n",
	       memcmp(first, second, sizeof(first)) == 0);

	if (memcmp(first, second, sizeof(first)) != 0) {
		fprintf(stderr, "two EEPROM reads differ\n");
		return 3;
	}
	if (argc == 3) {
		if (write_eeprom_be(argv[2], first) < 0)
			return 4;
		printf("EEPROM_FILE=%s\n", argv[2]);
	}

	return EXIT_SUCCESS;
}
