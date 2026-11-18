// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * cuseecho_ioctl: calls the ioctls of a cuseecho device and checks what they return
 *
 * Usage: cuseecho_ioctl /dev/NAME
 *
 * The buffers the ioctls' argument points at reach the server only through its retries, so a wrong retry
 * shows here as a failed ioctl or as wrong bytes. Leaves the content "ioctl" for the caller to read back.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "cuseecho.h"

static int failures;

#define CHECK(cond)                                                                                         \
	do {                                                                                                \
		if (!(cond)) {                                                                              \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);                          \
			failures++;                                                                         \
		}                                                                                           \
	} while (0)

/** As CHECK, for a condition on what an ioctl returned, so errno is that ioctl's */
#define CHECK_IOCTL(cond)                                                                                   \
	do {                                                                                                \
		errno = 0;                                                                                  \
		if (!(cond)) {                                                                              \
			fprintf(stderr, "%s:%d: %s (errno %d)\n", __FILE__, __LINE__, #cond, errno);        \
			failures++;                                                                         \
		}                                                                                           \
	} while (0)

static int
xfer(int fd, unsigned long cmd, void *buf, uint32_t nbytes, uint32_t *result)
{
	struct cuseecho_xfer x;
	int rc;

	memset(&x, 0, sizeof(x));
	x.addr = (uint64_t)(uintptr_t)buf;
	x.nbytes = nbytes;
	x.result = 0xdeadbeef;
	rc = ioctl(fd, cmd, &x);
	// The server writes the struct back only for GET, and must leave the fields it did not set
	if (rc == 0 && (x.addr != (uint64_t)(uintptr_t)buf || x.nbytes != nbytes)) {
		return -2;
	}
	*result = x.result;
	return rc;
}

int
main(int argc, char **argv)
{
	uint8_t pattern[3000];
	uint8_t full[4096];
	uint8_t got[4096];
	uint32_t result;
	size_t i;
	int fd;

	if (argc != 2) {
		fprintf(stderr, "usage: cuseecho_ioctl /dev/NAME\n");
		return 64;
	}
	fd = open(argv[1], O_RDWR);
	if (fd < 0) {
		perror(argv[1]);
		return 1;
	}
	for (i = 0; i < sizeof(pattern); i++) {
		pattern[i] = (uint8_t)(i * 7 + 1);
	}

	CHECK_IOCTL(xfer(fd, CUSEECHO_IOC_SET, pattern, sizeof(pattern), &result) == 0);
	CHECK(result == 0xdeadbeef);
	CHECK_IOCTL(ioctl(fd, CUSEECHO_IOC_NBYTES) == (int)sizeof(pattern));

	// A buffer larger than the content gets all of it, and nothing after it
	memset(got, 0x5a, sizeof(got));
	CHECK_IOCTL(xfer(fd, CUSEECHO_IOC_GET, got, sizeof(got), &result) == 0);
	CHECK(result == sizeof(pattern));
	CHECK(memcmp(got, pattern, sizeof(pattern)) == 0);
	for (i = sizeof(pattern); i < sizeof(got); i++) {
		if (got[i] != 0x5a) {
			break;
		}
	}
	CHECK(i == sizeof(got));

	// A smaller one gets its length
	memset(got, 0x5a, sizeof(got));
	CHECK_IOCTL(xfer(fd, CUSEECHO_IOC_GET, got, 10, &result) == 0);
	CHECK(result == 10);
	CHECK(memcmp(got, pattern, 10) == 0 && got[10] == 0x5a);

	// The most the device holds, 4096 bytes after the 16 of the struct, so the data fetched spans two pages
	for (i = 0; i < sizeof(full); i++) {
		full[i] = (uint8_t)(i * 13 + 5);
	}
	CHECK_IOCTL(xfer(fd, CUSEECHO_IOC_SET, full, sizeof(full), &result) == 0);
	CHECK_IOCTL(ioctl(fd, CUSEECHO_IOC_NBYTES) == (int)sizeof(full));
	memset(got, 0x5a, sizeof(got));
	CHECK_IOCTL(xfer(fd, CUSEECHO_IOC_GET, got, sizeof(got), &result) == 0);
	CHECK(result == sizeof(full));
	CHECK(memcmp(got, full, sizeof(full)) == 0);

	// No buffer at all still takes the round for the struct
	CHECK_IOCTL(xfer(fd, CUSEECHO_IOC_GET, NULL, 0, &result) == 0);
	CHECK(result == 0);
	CHECK_IOCTL(xfer(fd, CUSEECHO_IOC_SET, NULL, 0, &result) == 0);
	CHECK_IOCTL(ioctl(fd, CUSEECHO_IOC_NBYTES) == 0);

	// The server's errors come back as the ioctl's
	CHECK_IOCTL(xfer(fd, CUSEECHO_IOC_SET, got, sizeof(got) + 1, &result) == -1 && errno == EINVAL);
	CHECK_IOCTL(ioctl(fd, _IO('E', 99)) == -1 && errno == ENOTTY);

	memcpy(got, "ioctl", 5);
	CHECK_IOCTL(xfer(fd, CUSEECHO_IOC_SET, got, 5, &result) == 0);
	close(fd);
	if (failures) {
		fprintf(stderr, "cuseecho_ioctl: %d failed\n", failures);
		return 1;
	}
	printf("cuseecho_ioctl: ok\n");
	return 0;
}
