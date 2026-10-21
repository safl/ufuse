// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * ufuse: the FUSE and CUSE wire protocol, header-only
 * ===================================================
 *
 * A filesystem or character device in user space talks to the kernel by reading requests from, and
 * writing replies to, a file descriptor: /dev/fuse for a filesystem, /dev/cuse for a character device.
 * The messages are the structs of the kernel's <linux/fuse.h>. This header is that conversation and
 * nothing more: opening and mounting a session, the INIT handshakes, reading a request, writing a reply,
 * and packing directory entries. Which requests to answer, and how, is the caller's: it reads a request,
 * switches on its opcode, and replies. There are no callbacks, threads or allocations, so a session is one
 * more file descriptor in whatever loop the caller already runs.
 *
 * Every function returns 0, or a count, on success and a negative errno on failure. It needs Linux and the
 * POSIX and BSD declarations of its system headers: compile C as gnu11, or define _DEFAULT_SOURCE before
 * any include; C++ compilers provide them.
 *
 * @file ufuse.h
 * @version 0.1.0
 */
#ifndef UFUSE_H
#define UFUSE_H

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <linux/fuse.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UFUSE_VERSION "0.1.0"

/** The oldest protocol minor version a session accepts: Linux 2.6.29, which every supported kernel exceeds */
#define UFUSE_MIN_MINOR 12

/** Default largest write the kernel may send, and the request buffer that holds one with its headers */
#define UFUSE_MAX_WRITE (128 * 1024)
#define UFUSE_BUF_NBYTES (UFUSE_MAX_WRITE + 4096)

/**
 * One session: a filesystem mounted from /dev/fuse, or a character device served from /dev/cuse
 *
 * Zero it, then set 'fd' to -1, or call ::ufuse_session_init, before opening it.
 */
struct ufuse_session {
	int fd;             ///< The /dev/fuse or /dev/cuse descriptor; poll it for POLLIN
	uint32_t minor;     ///< The protocol minor version agreed in INIT
	uint32_t max_write; ///< The largest write the kernel may send; the request buffer must hold it
	uint32_t max_read;  ///< The largest read the kernel may ask for (CUSE)
};

/**
 * A request as read from the session: its header, and the argument after it
 *
 * Both point into the caller's buffer and are valid until the next read into it.
 */
struct ufuse_req {
	const struct fuse_in_header *in; ///< opcode, unique, nodeid, uid, gid, pid
	const void *arg;                 ///< The opcode's argument struct, possibly followed by names or data
	size_t arg_nbytes;               ///< Bytes from 'arg' to the end of the request
};

static inline void
ufuse_session_init(struct ufuse_session *s)
{
	memset(s, 0, sizeof(*s));
	s->fd = -1;
	s->max_write = UFUSE_MAX_WRITE;
	s->max_read = UFUSE_MAX_WRITE;
}

/**
 * Open /dev/fuse and mount a filesystem served from it at 'mountpoint'
 *
 * Mounting is a mount(2) call carrying the descriptor, so it needs CAP_SYS_ADMIN; there is no fusermount
 * fallback. A mount left by a process that died answers "Transport endpoint is not connected"; call
 * ::ufuse_unmount on the mountpoint first to clear it.
 *
 * @param s Session, from ::ufuse_session_init
 * @param mountpoint An existing directory
 * @param fsname What the mount table shows as the source, e.g. the pool's name
 * @param subtype Shown as the type "fuse.<subtype>"; NULL for plain "fuse"
 * @param mountflags MS_* flags; MS_NOSUID and MS_NODEV are always added, MS_RDONLY makes it read-only
 * @param options Further comma-separated options, e.g. "allow_other,default_permissions"; may be NULL
 *
 * @return 0 on success, negative errno on error, with the session closed again
 */
static inline int
ufuse_mount(struct ufuse_session *s, const char *mountpoint, const char *fsname, const char *subtype,
            unsigned long mountflags, const char *options)
{
	char type[64];
	char opts[512];
	struct stat st;
	int n;

	if (stat(mountpoint, &st) != 0) {
		return -errno;
	}
	s->fd = open("/dev/fuse", O_RDWR | O_CLOEXEC);
	if (s->fd < 0) {
		return -errno;
	}
	if (subtype) {
		snprintf(type, sizeof(type), "fuse.%s", subtype);
	} else {
		snprintf(type, sizeof(type), "fuse");
	}
	n = snprintf(opts, sizeof(opts), "fd=%d,rootmode=%o,user_id=%u,group_id=%u%s%s", s->fd,
	             (unsigned int)(st.st_mode & S_IFMT), (unsigned int)getuid(), (unsigned int)getgid(),
	             options && *options ? "," : "", options ? options : "");
	if (n < 0 || (size_t)n >= sizeof(opts)) {
		close(s->fd);
		s->fd = -1;
		return -EINVAL;
	}
	if (mount(fsname, mountpoint, type, mountflags | MS_NOSUID | MS_NODEV, opts) != 0) {
		int err = -errno;

		close(s->fd);
		s->fd = -1;
		return err;
	}
	return 0;
}

/**
 * Detach the filesystem at 'mountpoint', if any, and close the session
 *
 * Safe on a session that was never opened, and on a mountpoint with nothing mounted, so it also clears a
 * stale mount before ::ufuse_mount.
 */
static inline int
ufuse_unmount(struct ufuse_session *s, const char *mountpoint)
{
	int err = 0;

	if (mountpoint && umount2(mountpoint, MNT_DETACH) != 0 && errno != EINVAL && errno != ENOENT) {
		err = -errno;
	}
	if (s->fd >= 0) {
		close(s->fd);
		s->fd = -1;
	}
	return err;
}

/**
 * Open /dev/cuse for a character device session; ::ufuse_cuse_init_reply names the device
 *
 * Needs the cuse module loaded and access to /dev/cuse, root by default.
 */
static inline int
ufuse_cuse_open(struct ufuse_session *s)
{
	s->fd = open("/dev/cuse", O_RDWR | O_CLOEXEC);
	return s->fd < 0 ? -errno : 0;
}

/**
 * Close a CUSE session; the kernel removes its device
 */
static inline void
ufuse_cuse_close(struct ufuse_session *s)
{
	if (s->fd >= 0) {
		close(s->fd);
		s->fd = -1;
	}
}

/**
 * Read one request into 'buf'
 *
 * @param buf At least UFUSE_BUF_NBYTES, or s->max_write plus 4096 when that was changed before INIT
 *
 * @return The request's length on success. -EINTR and -EAGAIN mean nothing was read; -ENOENT means the
 * kernel withdrew the request it was about to send; all three ask for another read. -ENODEV means the
 * filesystem was unmounted or the device removed, and the session is over.
 */
static inline ssize_t
ufuse_read(struct ufuse_session *s, void *buf, size_t nbytes, struct ufuse_req *req)
{
	ssize_t n;

	memset(req, 0, sizeof(*req));
	n = read(s->fd, buf, nbytes);
	if (n < 0) {
		return -errno;
	}
	if ((size_t)n < sizeof(struct fuse_in_header)) {
		return -EPROTO;
	}
	req->in = (const struct fuse_in_header *)buf;
	if (req->in->len != (uint32_t)n) {
		return -EPROTO;
	}
	req->arg = (const uint8_t *)buf + sizeof(struct fuse_in_header);
	req->arg_nbytes = (size_t)n - sizeof(struct fuse_in_header);
	return n;
}

/**
 * Reply to request 'unique' with 'error' (0 or a negative errno) and the payload in 'iov'
 *
 * @return 0 on success. A reply to a request the kernel has since withdrawn is dropped by it with ENOENT,
 * which is not an error here.
 */
static inline int
ufuse_reply_iov(struct ufuse_session *s, uint64_t unique, int32_t error, const struct iovec *iov, int count)
{
	struct fuse_out_header out;
	struct iovec vec[16];
	size_t len = sizeof(out);
	int i;

	if (count < 0 || count > 15) {
		return -EINVAL;
	}
	vec[0].iov_base = &out;
	vec[0].iov_len = sizeof(out);
	for (i = 0; i < count; i++) {
		vec[i + 1] = iov[i];
		len += iov[i].iov_len;
	}
	out.len = (uint32_t)len;
	out.error = error;
	out.unique = unique;
	if (writev(s->fd, vec, count + 1) < 0) {
		return errno == ENOENT ? 0 : -errno;
	}
	return 0;
}

/** Reply with one payload, e.g. a struct fuse_attr_out, or the bytes a READ asked for */
static inline int
ufuse_reply(struct ufuse_session *s, uint64_t unique, const void *arg, size_t nbytes)
{
	struct iovec iov;

	iov.iov_base = (void *)arg;
	iov.iov_len = nbytes;
	return ufuse_reply_iov(s, unique, 0, &iov, nbytes ? 1 : 0);
}

/** Reply with an error, a positive errno such as ENOENT or EROFS */
static inline int
ufuse_reply_err(struct ufuse_session *s, uint64_t unique, int err)
{
	return ufuse_reply_iov(s, unique, -err, NULL, 0);
}

/**
 * Answer FUSE_INIT, agreeing on the protocol version and the capabilities both sides have
 *
 * @param want FUSE_* capability flags the filesystem would use, e.g. FUSE_ASYNC_READ | FUSE_BIG_WRITES;
 * the reply grants those the kernel also offered. Flags above bit 31 are carried in flags2, when the
 * kernel has FUSE_INIT_EXT.
 *
 * @return 0 on success; -EPROTO when the request is not FUSE_INIT or the kernel speaks another major
 * version or a minor older than UFUSE_MIN_MINOR, which is answered with EPROTO first.
 */
static inline int
ufuse_init_reply(struct ufuse_session *s, const struct ufuse_req *req, uint64_t want)
{
	struct fuse_init_in in;
	struct fuse_init_out out;
	uint64_t offered;
	size_t out_nbytes;

	if (req->in->opcode != FUSE_INIT) {
		return -EPROTO;
	}
	// A kernel older than this header sends a shorter struct, e.g. without flags2 before 7.36
	memset(&in, 0, sizeof(in));
	memcpy(&in, req->arg, req->arg_nbytes < sizeof(in) ? req->arg_nbytes : sizeof(in));
	if (in.major != FUSE_KERNEL_VERSION || in.minor < UFUSE_MIN_MINOR) {
		ufuse_reply_err(s, req->in->unique, EPROTO);
		return -EPROTO;
	}

	offered = in.flags;
#ifdef FUSE_INIT_EXT
	if (in.flags & FUSE_INIT_EXT) {
		offered |= (uint64_t)in.flags2 << 32;
	}
#endif
	want &= offered;
#ifdef FUSE_INIT_EXT
	if (offered & FUSE_INIT_EXT) {
		want |= FUSE_INIT_EXT;
	}
#endif
	if (offered & FUSE_MAX_PAGES) {
		want |= FUSE_MAX_PAGES;
	}

	s->minor = in.minor < FUSE_KERNEL_MINOR_VERSION ? in.minor : FUSE_KERNEL_MINOR_VERSION;
	memset(&out, 0, sizeof(out));
	out.major = FUSE_KERNEL_VERSION;
	out.minor = s->minor;
	out.max_readahead = in.max_readahead;
	out.flags = (uint32_t)want;
#ifdef FUSE_INIT_EXT
	out.flags2 = (uint32_t)(want >> 32);
#endif
	out.max_background = 16;
	out.congestion_threshold = 12;
	out.max_write = s->max_write;
	out.time_gran = 1;
	out.max_pages = (uint16_t)(s->max_write / 4096);

	// Before 7.23 the reply ended after max_write; later fields are read only by kernels that know them
	out_nbytes = s->minor < 23 ? FUSE_COMPAT_22_INIT_OUT_SIZE : sizeof(out);
	return ufuse_reply(s, req->in->unique, &out, out_nbytes);
}

/**
 * Answer CUSE_INIT, creating /dev/<devname>
 *
 * @param devname The device's name under /dev; the caller picks one no other device has
 * @param dev_major,dev_minor 0 and 0 let the kernel choose the numbers
 * @param flags CUSE_UNRESTRICTED_IOCTL to receive ioctls whose arguments the kernel cannot size
 */
static inline int
ufuse_cuse_init_reply(struct ufuse_session *s, const struct ufuse_req *req, const char *devname, uint32_t dev_major,
                      uint32_t dev_minor, uint32_t flags)
{
	struct cuse_init_in in;
	struct cuse_init_out out;
	char info[128];
	struct iovec iov[2];
	int n;

	if (req->in->opcode != CUSE_INIT) {
		return -EPROTO;
	}
	memset(&in, 0, sizeof(in));
	memcpy(&in, req->arg, req->arg_nbytes < sizeof(in) ? req->arg_nbytes : sizeof(in));
	if (in.major != FUSE_KERNEL_VERSION || in.minor < UFUSE_MIN_MINOR) {
		ufuse_reply_err(s, req->in->unique, EPROTO);
		return -EPROTO;
	}
	// The device's properties follow the struct as NUL-terminated "KEY=value" strings
	n = snprintf(info, sizeof(info), "DEVNAME=%s", devname);
	if (n < 0 || (size_t)n >= sizeof(info)) {
		ufuse_reply_err(s, req->in->unique, ENAMETOOLONG);
		return -ENAMETOOLONG;
	}

	s->minor = in.minor < FUSE_KERNEL_MINOR_VERSION ? in.minor : FUSE_KERNEL_MINOR_VERSION;
	memset(&out, 0, sizeof(out));
	out.major = FUSE_KERNEL_VERSION;
	out.minor = s->minor;
	out.flags = flags & in.flags;
	out.max_read = s->max_read;
	out.max_write = s->max_write;
	out.dev_major = dev_major;
	out.dev_minor = dev_minor;

	iov[0].iov_base = &out;
	iov[0].iov_len = sizeof(out);
	iov[1].iov_base = info;
	iov[1].iov_len = (size_t)n + 1;
	return ufuse_reply_iov(s, req->in->unique, 0, iov, 2);
}

/**
 * Append one entry to a READDIR reply being built in 'buf'
 *
 * @param used Bytes of 'buf' already filled
 * @param size The reply's limit, the 'size' of the request's struct fuse_read_in
 * @param off The offset a later READDIR passes to continue after this entry
 * @param type The entry's type, as in a struct dirent: DT_REG, DT_DIR, ...
 *
 * @return The bytes the entry took, or 0 when it does not fit and the reply is full
 */
static inline size_t
ufuse_dirent_add(void *buf, size_t used, size_t size, const char *name, uint64_t ino, uint64_t off,
                 uint32_t type)
{
	size_t namelen = strlen(name);
	size_t entlen = FUSE_DIRENT_ALIGN(FUSE_NAME_OFFSET + namelen);
	struct fuse_dirent *d;

	if (used + entlen > size) {
		return 0;
	}
	d = (struct fuse_dirent *)((uint8_t *)buf + used);
	d->ino = ino;
	d->off = off;
	d->namelen = (uint32_t)namelen;
	d->type = type;
	memcpy(d->name, name, namelen);
	memset(d->name + namelen, 0, entlen - FUSE_NAME_OFFSET - namelen);
	return entlen;
}

/** Fill the attributes of an inode with no times, owned by the caller's process */
static inline void
ufuse_attr_fill(struct fuse_attr *attr, uint64_t ino, uint32_t mode, uint32_t nlink, uint64_t size)
{
	memset(attr, 0, sizeof(*attr));
	attr->ino = ino;
	attr->mode = mode;
	attr->nlink = nlink;
	attr->size = size;
	attr->blocks = (size + 511) / 512;
	attr->blksize = 4096;
	attr->uid = (uint32_t)getuid();
	attr->gid = (uint32_t)getgid();
}

/** A name for an opcode, for traces; "UNKNOWN" for one this header does not name */
static inline const char *
ufuse_opcode_name(uint32_t opcode)
{
	switch (opcode) {
	case FUSE_LOOKUP:
		return "LOOKUP";
	case FUSE_FORGET:
		return "FORGET";
	case FUSE_GETATTR:
		return "GETATTR";
	case FUSE_SETATTR:
		return "SETATTR";
	case FUSE_READLINK:
		return "READLINK";
	case FUSE_MKNOD:
		return "MKNOD";
	case FUSE_MKDIR:
		return "MKDIR";
	case FUSE_UNLINK:
		return "UNLINK";
	case FUSE_RMDIR:
		return "RMDIR";
	case FUSE_RENAME:
		return "RENAME";
	case FUSE_OPEN:
		return "OPEN";
	case FUSE_READ:
		return "READ";
	case FUSE_WRITE:
		return "WRITE";
	case FUSE_STATFS:
		return "STATFS";
	case FUSE_RELEASE:
		return "RELEASE";
	case FUSE_FSYNC:
		return "FSYNC";
	case FUSE_GETXATTR:
		return "GETXATTR";
	case FUSE_LISTXATTR:
		return "LISTXATTR";
	case FUSE_FLUSH:
		return "FLUSH";
	case FUSE_INIT:
		return "INIT";
	case FUSE_OPENDIR:
		return "OPENDIR";
	case FUSE_READDIR:
		return "READDIR";
	case FUSE_RELEASEDIR:
		return "RELEASEDIR";
	case FUSE_ACCESS:
		return "ACCESS";
	case FUSE_CREATE:
		return "CREATE";
	case FUSE_INTERRUPT:
		return "INTERRUPT";
	case FUSE_DESTROY:
		return "DESTROY";
	case FUSE_IOCTL:
		return "IOCTL";
	case FUSE_POLL:
		return "POLL";
	case FUSE_BATCH_FORGET:
		return "BATCH_FORGET";
	case FUSE_READDIRPLUS:
		return "READDIRPLUS";
	case FUSE_LSEEK:
		return "LSEEK";
	case CUSE_INIT:
		return "CUSE_INIT";
	default:
		return "UNKNOWN";
	}
}

#ifdef __cplusplus
}
#endif

#endif
