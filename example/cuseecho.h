// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * The ioctls of a cuseecho device, for the server and for a program calling them
 *
 * The device is unrestricted, so the kernel copies no argument for the server: every ioctl arrives without
 * data. The argument of CUSEECHO_IOC_SET and CUSEECHO_IOC_GET points at a struct that itself points at a
 * buffer, as the argument of an NVMe passthrough ioctl does, and the server asks for the struct, and then
 * for the buffer, with a retry.
 */
#ifndef CUSEECHO_H
#define CUSEECHO_H

#include <stdint.h>
#include <sys/ioctl.h>

struct cuseecho_xfer {
	uint64_t addr;   ///< The caller's buffer
	uint32_t nbytes; ///< Its length
	uint32_t result; ///< Set by CUSEECHO_IOC_GET: the bytes copied to the buffer
};

/** Returns the length of the content */
#define CUSEECHO_IOC_NBYTES _IO('E', 0)
/** Replaces the content with the buffer's 'nbytes' */
#define CUSEECHO_IOC_SET _IOW('E', 1, struct cuseecho_xfer)
/** Copies the content, up to 'nbytes' of it, to the buffer, and sets 'result' */
#define CUSEECHO_IOC_GET _IOWR('E', 2, struct cuseecho_xfer)

#endif
