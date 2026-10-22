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
 * switches on its opcode, and replies, or hands the request to ::ufuse_dispatch with a table of callbacks.
 * There are no threads or allocations, so a session is one more file descriptor in whatever loop the caller
 * already runs.
 *
 * Every function returns 0, or a count, on success and a negative errno on failure. It needs Linux and the
 * POSIX and BSD declarations of its system headers: compile C as gnu11, or define _DEFAULT_SOURCE before
 * any include; C++ compilers provide them.
 *
 * @file ufuse.h
 * @version 0.2.0
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

#if FUSE_KERNEL_MINOR_VERSION < 39
#error "ufuse needs <linux/fuse.h> of protocol 7.39 (Linux 6.6) or later"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define UFUSE_VERSION "0.2.0"

/** The oldest protocol minor version a session accepts: Linux 6.6. From it on, each request struct has one size. */
#define UFUSE_MIN_MINOR 39

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
	uint64_t flags;     ///< The FUSE_* capabilities granted in INIT, flags2 in the upper half; read-only after
};

/**
 * A request as read from the session: its header, and the argument after it
 *
 * A borrowed view: 'in' and 'arg' point into the caller's buffer and are valid until the next read into it,
 * or, handed to a callback by ::ufuse_dispatch, until the callback returns. To reply after that, take a
 * ::ufuse_defer with ::ufuse_req_defer.
 */
struct ufuse_req {
	const struct fuse_in_header *in; ///< opcode, unique, nodeid, uid, gid, pid
	const void *arg;                 ///< The opcode's argument struct, possibly followed by names or data
	size_t arg_nbytes;               ///< Bytes from 'arg' to the end of the request
	struct ufuse_session *s;         ///< The session the request came from, and its reply goes to
	void *ctx;                       ///< The caller's, as given to ::ufuse_dispatch; NULL from ::ufuse_read
	uint64_t unique;                 ///< Copied from the header, as are the fields below
	uint64_t nodeid;
	uint32_t opcode;
	uint32_t uid;
	uint32_t gid;
	uint32_t pid;
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
 * Describe the request of 'nbytes' in 'buf', read from session 's', in 'req'
 *
 * @return 0 on success, -EPROTO when 'buf' is not one whole request
 */
static inline int
ufuse_req_parse(struct ufuse_session *s, const void *buf, size_t nbytes, struct ufuse_req *req)
{
	memset(req, 0, sizeof(*req));
	if (nbytes < sizeof(struct fuse_in_header)) {
		return -EPROTO;
	}
	req->in = (const struct fuse_in_header *)buf;
	if (req->in->len != nbytes) {
		return -EPROTO;
	}
	req->arg = (const uint8_t *)buf + sizeof(struct fuse_in_header);
	req->arg_nbytes = nbytes - sizeof(struct fuse_in_header);
	req->s = s;
	req->unique = req->in->unique;
	req->nodeid = req->in->nodeid;
	req->opcode = req->in->opcode;
	req->uid = req->in->uid;
	req->gid = req->in->gid;
	req->pid = req->in->pid;
	return 0;
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
	int err;

	memset(req, 0, sizeof(*req));
	n = read(s->fd, buf, nbytes);
	if (n < 0) {
		return -errno;
	}
	err = ufuse_req_parse(s, buf, (size_t)n, req);
	return err ? err : n;
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
 * the reply grants those the kernel also offered. FUSE_SETXATTR_EXT is always asked for, and
 * FUSE_SECURITY_CTX and FUSE_CREATE_SUPP_GROUP never, so every request struct has its one size.
 *
 * @return 0 on success; -EPROTO when the request is not a FUSE_INIT at least as long as this header's, or the
 * kernel speaks another major version or a minor older than UFUSE_MIN_MINOR; -ENOTSUP when it does not offer
 * FUSE_SETXATTR_EXT. Both are answered with EPROTO first, which fails the mount.
 */
static inline int
ufuse_init_reply(struct ufuse_session *s, const struct ufuse_req *req, uint64_t want)
{
	struct fuse_init_in in;
	struct fuse_init_out out;
	uint64_t offered;

	if (req->in->opcode != FUSE_INIT || req->arg_nbytes < 2 * sizeof(uint32_t)) {
		return -EPROTO;
	}
	// INIT comes before a version is agreed, in the kernel's own layout: an older kernel's shorter struct is
	// refused for its version, and what a newer kernel's longer one adds is ignored
	memset(&in, 0, sizeof(in));
	memcpy(&in, req->arg, req->arg_nbytes < sizeof(in) ? req->arg_nbytes : sizeof(in));
	if (in.major != FUSE_KERNEL_VERSION || in.minor < UFUSE_MIN_MINOR || req->arg_nbytes < sizeof(in)) {
		ufuse_reply_err(s, req->in->unique, EPROTO);
		return -EPROTO;
	}

	offered = in.flags;
	if (in.flags & FUSE_INIT_EXT) {
		offered |= (uint64_t)in.flags2 << 32;
	}
	if (!(offered & FUSE_SETXATTR_EXT)) {
		ufuse_reply_err(s, req->in->unique, EPROTO);
		return -ENOTSUP;
	}
	want |= FUSE_SETXATTR_EXT | FUSE_INIT_EXT | FUSE_MAX_PAGES;
	want &= ~(FUSE_SECURITY_CTX | FUSE_CREATE_SUPP_GROUP);
	want &= offered;

	s->minor = in.minor < FUSE_KERNEL_MINOR_VERSION ? in.minor : FUSE_KERNEL_MINOR_VERSION;
	s->flags = want;
	memset(&out, 0, sizeof(out));
	out.major = FUSE_KERNEL_VERSION;
	out.minor = s->minor;
	out.max_readahead = in.max_readahead;
	out.flags = (uint32_t)want;
	out.flags2 = (uint32_t)(want >> 32);
	out.max_background = 16;
	out.congestion_threshold = 12;
	out.max_write = s->max_write;
	out.time_gran = 1;
	out.max_pages = (uint16_t)(s->max_write / 4096);
	return ufuse_reply(s, req->in->unique, &out, sizeof(out));
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

	if (req->in->opcode != CUSE_INIT || req->arg_nbytes < 2 * sizeof(uint32_t)) {
		return -EPROTO;
	}
	memset(&in, 0, sizeof(in));
	memcpy(&in, req->arg, req->arg_nbytes < sizeof(in) ? req->arg_nbytes : sizeof(in));
	if (in.major != FUSE_KERNEL_VERSION || in.minor < UFUSE_MIN_MINOR || req->arg_nbytes < sizeof(in)) {
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

/**
 * Append one entry to a READDIRPLUS reply being built in 'buf'
 *
 * As ::ufuse_dirent_add, with the entry's LOOKUP reply in 'entry', from which its inode number and type are
 * taken. The kernel counts the entry as a lookup, to be forgotten later, unless entry->nodeid is 0.
 *
 * @return The bytes the entry took, or 0 when it does not fit and the reply is full
 */
static inline size_t
ufuse_direntplus_add(void *buf, size_t used, size_t size, const char *name, const struct fuse_entry_out *entry,
                     uint64_t off)
{
	size_t namelen = strlen(name);
	size_t entlen = FUSE_DIRENT_ALIGN(FUSE_NAME_OFFSET_DIRENTPLUS + namelen);
	struct fuse_direntplus *d;

	if (used + entlen > size) {
		return 0;
	}
	d = (struct fuse_direntplus *)((uint8_t *)buf + used);
	d->entry_out = *entry;
	d->dirent.ino = entry->attr.ino;
	d->dirent.off = off;
	d->dirent.namelen = (uint32_t)namelen;
	d->dirent.type = (entry->attr.mode & S_IFMT) >> 12;
	memcpy(d->dirent.name, name, namelen);
	memset(d->dirent.name + namelen, 0, entlen - FUSE_NAME_OFFSET_DIRENTPLUS - namelen);
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

/** Reply to 'req' with 'error' (0 or a negative errno) and the payload in 'iov'; see ::ufuse_reply_iov */
static inline int
ufuse_req_reply_iov(const struct ufuse_req *req, int32_t error, const struct iovec *iov, int count)
{
	return ufuse_reply_iov(req->s, req->unique, error, iov, count);
}

/** Reply to 'req' with one payload; see ::ufuse_reply */
static inline int
ufuse_req_reply(const struct ufuse_req *req, const void *arg, size_t nbytes)
{
	return ufuse_reply(req->s, req->unique, arg, nbytes);
}

/** Reply to 'req' with an error, a positive errno; see ::ufuse_reply_err */
static inline int
ufuse_req_reply_err(const struct ufuse_req *req, int err)
{
	return ufuse_reply_err(req->s, req->unique, err);
}

/**
 * What a reply needs and nothing that points into the request buffer, from ::ufuse_req_defer
 *
 * Copy and keep it by value; answer it once, from any thread, while the session is open.
 */
struct ufuse_defer {
	struct ufuse_session *s;
	uint64_t unique;
	void *ctx; ///< The request's ctx, for the caller's convenience
};

/** Keep 'req' for replying after its callback returns or its buffer is reused */
static inline struct ufuse_defer
ufuse_req_defer(const struct ufuse_req *req)
{
	struct ufuse_defer d;

	d.s = req->s;
	d.unique = req->unique;
	d.ctx = req->ctx;
	return d;
}

/** Reply to a deferred request with 'error' (0 or a negative errno) and the payload in 'iov' */
static inline int
ufuse_defer_reply_iov(const struct ufuse_defer *d, int32_t error, const struct iovec *iov, int count)
{
	return ufuse_reply_iov(d->s, d->unique, error, iov, count);
}

/** Reply to a deferred request with one payload */
static inline int
ufuse_defer_reply(const struct ufuse_defer *d, const void *arg, size_t nbytes)
{
	return ufuse_reply(d->s, d->unique, arg, nbytes);
}

/** Reply to a deferred request with an error, a positive errno */
static inline int
ufuse_defer_reply_err(const struct ufuse_defer *d, int err)
{
	return ufuse_reply_err(d->s, d->unique, err);
}

/**
 * Tell the kernel to drop what it caches of inode 'ino': its attributes, and with 'off' >= 0 its pages from 'off'
 * for 'len' bytes, to the end when 'len' is 0. A negative 'off' drops the attributes only, which takes no lock a
 * pending request can hold, so a single-threaded server may send it between requests.
 *
 * @return 0 on success, also when the kernel holds nothing of 'ino'; a negative errno otherwise
 */
static inline int
ufuse_notify_inval_inode(struct ufuse_session *s, uint64_t ino, int64_t off, int64_t len)
{
	struct fuse_notify_inval_inode_out out;
	struct iovec iov;

	memset(&out, 0, sizeof(out));
	out.ino = ino;
	out.off = off;
	out.len = len;
	iov.iov_base = &out;
	iov.iov_len = sizeof(out);
	return ufuse_reply_iov(s, 0, FUSE_NOTIFY_INVAL_INODE, &iov, 1);
}

/**
 * Tell the kernel to forget the name 'name' in directory 'parent', so the next use looks it up again
 *
 * The kernel takes the directory's lock to do it and waits for it, so it must not be sent while a request on that
 * directory awaits its reply from the same thread: a single-threaded server deadlocks. Send it from a thread of
 * its own, or rely on short entry timeouts instead.
 *
 * @return 0 on success, also when the kernel holds no such name; a negative errno otherwise
 */
static inline int
ufuse_notify_inval_entry(struct ufuse_session *s, uint64_t parent, const char *name)
{
	struct fuse_notify_inval_entry_out out;
	struct iovec iov[2];
	size_t namelen = strlen(name);

	memset(&out, 0, sizeof(out));
	out.parent = parent;
	out.namelen = (uint32_t)namelen;
	iov[0].iov_base = &out;
	iov[0].iov_len = sizeof(out);
	iov[1].iov_base = (void *)name;
	iov[1].iov_len = namelen + 1;
	return ufuse_reply_iov(s, 0, FUSE_NOTIFY_INVAL_ENTRY, iov, 2);
}

#define UFUSE_OP_NOREPLY 0x1 ///< The kernel expects no reply
#define UFUSE_OP_MUTATES 0x2 ///< Refused with EROFS under UFUSE_READONLY
#define UFUSE_OP_DATA 0x4    ///< Data follows the struct and names, its length given in the struct

/** What ::ufuse_dispatch knows of an opcode before decoding it */
struct ufuse_opdesc {
	uint32_t opcode;
	const char *name;
	uint32_t in_nbytes; ///< The argument struct's one size since protocol 7.39; 0 for none
	uint32_t nnames;    ///< NUL-terminated names following the struct
	uint32_t flags;     ///< UFUSE_OP_*
};

/** The description of 'opcode', or NULL for one this header does not decode */
static inline const struct ufuse_opdesc *
ufuse_opdesc(uint32_t opcode)
{
#define UFUSE_OPDESC(op, name, in_nbytes, nnames, flags)                                                    \
	case op: {                                                                                          \
		static const struct ufuse_opdesc d = {op, name, in_nbytes, nnames, flags};                  \
		return &d;                                                                                  \
	}
#define UFUSE_M UFUSE_OP_MUTATES
	switch (opcode) {
	UFUSE_OPDESC(FUSE_LOOKUP, "LOOKUP", 0, 1, 0)
	UFUSE_OPDESC(FUSE_FORGET, "FORGET", sizeof(struct fuse_forget_in), 0, UFUSE_OP_NOREPLY)
	UFUSE_OPDESC(FUSE_GETATTR, "GETATTR", sizeof(struct fuse_getattr_in), 0, 0)
	UFUSE_OPDESC(FUSE_SETATTR, "SETATTR", sizeof(struct fuse_setattr_in), 0, UFUSE_M)
	UFUSE_OPDESC(FUSE_READLINK, "READLINK", 0, 0, 0)
	UFUSE_OPDESC(FUSE_SYMLINK, "SYMLINK", 0, 2, UFUSE_M)
	UFUSE_OPDESC(FUSE_MKNOD, "MKNOD", sizeof(struct fuse_mknod_in), 1, UFUSE_M)
	UFUSE_OPDESC(FUSE_MKDIR, "MKDIR", sizeof(struct fuse_mkdir_in), 1, UFUSE_M)
	UFUSE_OPDESC(FUSE_UNLINK, "UNLINK", 0, 1, UFUSE_M)
	UFUSE_OPDESC(FUSE_RMDIR, "RMDIR", 0, 1, UFUSE_M)
	UFUSE_OPDESC(FUSE_RENAME, "RENAME", sizeof(struct fuse_rename_in), 2, UFUSE_M)
	UFUSE_OPDESC(FUSE_LINK, "LINK", sizeof(struct fuse_link_in), 1, UFUSE_M)
	UFUSE_OPDESC(FUSE_OPEN, "OPEN", sizeof(struct fuse_open_in), 0, 0)
	UFUSE_OPDESC(FUSE_READ, "READ", sizeof(struct fuse_read_in), 0, 0)
	UFUSE_OPDESC(FUSE_WRITE, "WRITE", sizeof(struct fuse_write_in), 0, UFUSE_M | UFUSE_OP_DATA)
	UFUSE_OPDESC(FUSE_STATFS, "STATFS", 0, 0, 0)
	UFUSE_OPDESC(FUSE_RELEASE, "RELEASE", sizeof(struct fuse_release_in), 0, 0)
	UFUSE_OPDESC(FUSE_FSYNC, "FSYNC", sizeof(struct fuse_fsync_in), 0, 0)
	UFUSE_OPDESC(FUSE_SETXATTR, "SETXATTR", sizeof(struct fuse_setxattr_in), 1, UFUSE_M | UFUSE_OP_DATA)
	UFUSE_OPDESC(FUSE_GETXATTR, "GETXATTR", sizeof(struct fuse_getxattr_in), 1, 0)
	UFUSE_OPDESC(FUSE_LISTXATTR, "LISTXATTR", sizeof(struct fuse_getxattr_in), 0, 0)
	UFUSE_OPDESC(FUSE_REMOVEXATTR, "REMOVEXATTR", 0, 1, UFUSE_M)
	UFUSE_OPDESC(FUSE_FLUSH, "FLUSH", sizeof(struct fuse_flush_in), 0, 0)
	UFUSE_OPDESC(FUSE_INIT, "INIT", sizeof(struct fuse_init_in), 0, 0)
	UFUSE_OPDESC(FUSE_OPENDIR, "OPENDIR", sizeof(struct fuse_open_in), 0, 0)
	UFUSE_OPDESC(FUSE_READDIR, "READDIR", sizeof(struct fuse_read_in), 0, 0)
	UFUSE_OPDESC(FUSE_RELEASEDIR, "RELEASEDIR", sizeof(struct fuse_release_in), 0, 0)
	UFUSE_OPDESC(FUSE_FSYNCDIR, "FSYNCDIR", sizeof(struct fuse_fsync_in), 0, 0)
	UFUSE_OPDESC(FUSE_GETLK, "GETLK", sizeof(struct fuse_lk_in), 0, 0)
	UFUSE_OPDESC(FUSE_SETLK, "SETLK", sizeof(struct fuse_lk_in), 0, 0)
	UFUSE_OPDESC(FUSE_SETLKW, "SETLKW", sizeof(struct fuse_lk_in), 0, 0)
	UFUSE_OPDESC(FUSE_ACCESS, "ACCESS", sizeof(struct fuse_access_in), 0, 0)
	UFUSE_OPDESC(FUSE_CREATE, "CREATE", sizeof(struct fuse_create_in), 1, UFUSE_M)
	UFUSE_OPDESC(FUSE_INTERRUPT, "INTERRUPT", sizeof(struct fuse_interrupt_in), 0, UFUSE_OP_NOREPLY)
	UFUSE_OPDESC(FUSE_BMAP, "BMAP", sizeof(struct fuse_bmap_in), 0, 0)
	UFUSE_OPDESC(FUSE_DESTROY, "DESTROY", 0, 0, 0)
	UFUSE_OPDESC(FUSE_IOCTL, "IOCTL", sizeof(struct fuse_ioctl_in), 0, UFUSE_OP_DATA)
	UFUSE_OPDESC(FUSE_POLL, "POLL", sizeof(struct fuse_poll_in), 0, 0)
	UFUSE_OPDESC(FUSE_BATCH_FORGET, "BATCH_FORGET", sizeof(struct fuse_batch_forget_in), 0,
	             UFUSE_OP_NOREPLY | UFUSE_OP_DATA)
	UFUSE_OPDESC(FUSE_FALLOCATE, "FALLOCATE", sizeof(struct fuse_fallocate_in), 0, UFUSE_M)
	UFUSE_OPDESC(FUSE_READDIRPLUS, "READDIRPLUS", sizeof(struct fuse_read_in), 0, 0)
	UFUSE_OPDESC(FUSE_RENAME2, "RENAME2", sizeof(struct fuse_rename2_in), 2, UFUSE_M)
	UFUSE_OPDESC(FUSE_LSEEK, "LSEEK", sizeof(struct fuse_lseek_in), 0, 0)
	UFUSE_OPDESC(FUSE_COPY_FILE_RANGE, "COPY_FILE_RANGE", sizeof(struct fuse_copy_file_range_in), 0, UFUSE_M)
	UFUSE_OPDESC(FUSE_SYNCFS, "SYNCFS", sizeof(struct fuse_syncfs_in), 0, 0)
	UFUSE_OPDESC(FUSE_TMPFILE, "TMPFILE", sizeof(struct fuse_create_in), 1, UFUSE_M)
	UFUSE_OPDESC(FUSE_STATX, "STATX", sizeof(struct fuse_statx_in), 0, 0)
#if FUSE_KERNEL_MINOR_VERSION >= 45
	// Named for traces; not dispatched, as it is newer than the protocol floor
	UFUSE_OPDESC(FUSE_COPY_FILE_RANGE_64, "COPY_FILE_RANGE_64", sizeof(struct fuse_copy_file_range_in), 0,
	             UFUSE_M)
#endif
	UFUSE_OPDESC(CUSE_INIT, "CUSE_INIT", sizeof(struct cuse_init_in), 0, 0)
	default:
		return NULL;
	}
#undef UFUSE_M
#undef UFUSE_OPDESC
}

/** A name for an opcode, for traces; "UNKNOWN" for one this header does not name */
static inline const char *
ufuse_opcode_name(uint32_t opcode)
{
	const struct ufuse_opdesc *d = ufuse_opdesc(opcode);

	return d ? d->name : "UNKNOWN";
}

/** For ufuse_ops.flags: answer every mutating request, and every open for writing, with EROFS */
#define UFUSE_READONLY 0x1

/**
 * Callbacks for ::ufuse_dispatch, one per opcode; a NULL callback is answered with ENOSYS
 *
 * Every callback takes the request, the header's node id, the kernel's argument struct when the opcode has
 * one, and then what follows it: names as NUL-terminated strings, or data and its length. The node id is the
 * inode a request is about, or the parent directory for one with names. The request and the arguments point
 * into the request buffer and are valid until the callback returns. A callback replies exactly once, unless
 * noted otherwise: before returning with ::ufuse_req_reply and its siblings, or later, from any thread,
 * through the ::ufuse_defer it took with ::ufuse_req_defer. Dispatch never replies to a request it handed to
 * a callback.
 *
 * Zero it before setting callbacks, so the ones a filesystem does not have are NULL.
 */
struct ufuse_ops {
	uint32_t flags; ///< UFUSE_READONLY
	uint64_t want;  ///< FUSE_* capabilities to ask for in INIT, as for ::ufuse_init_reply

	/// Called before INIT is answered, to change 'want' or 's->max_write'; replies nothing
	void (*init)(const struct ufuse_req *req, uint64_t nodeid, const struct fuse_init_in *in, uint64_t *want);
	/// Called before DESTROY is answered; replies nothing
	void (*destroy)(const struct ufuse_req *req, uint64_t nodeid);
	/// The kernel gave up on request in->unique; that request still gets its reply. No reply.
	void (*interrupt)(const struct ufuse_req *req, uint64_t nodeid, const struct fuse_interrupt_in *in);
	/// Drop in->nlookup references to 'ino'; called per entry for BATCH_FORGET. No reply.
	void (*forget)(const struct ufuse_req *req, uint64_t ino, const struct fuse_forget_in *in);

	void (*lookup)(const struct ufuse_req *req, uint64_t parent, const char *name);
	void (*getattr)(const struct ufuse_req *req, uint64_t ino, const struct fuse_getattr_in *in);
	void (*setattr)(const struct ufuse_req *req, uint64_t ino, const struct fuse_setattr_in *in);
	void (*readlink)(const struct ufuse_req *req, uint64_t ino);
	void (*symlink)(const struct ufuse_req *req, uint64_t parent, const char *name, const char *target);
	void (*mknod)(const struct ufuse_req *req, uint64_t parent, const struct fuse_mknod_in *in, const char *name);
	void (*mkdir)(const struct ufuse_req *req, uint64_t parent, const struct fuse_mkdir_in *in, const char *name);
	void (*unlink)(const struct ufuse_req *req, uint64_t parent, const char *name);
	void (*rmdir)(const struct ufuse_req *req, uint64_t parent, const char *name);
	void (*rename)(const struct ufuse_req *req, uint64_t parent, const struct fuse_rename_in *in, const char *name,
	               const char *newname);
	/// 'newparent' gets a link to in->oldnodeid named 'newname'
	void (*link)(const struct ufuse_req *req, uint64_t newparent, const struct fuse_link_in *in,
	             const char *newname);
	void (*open)(const struct ufuse_req *req, uint64_t ino, const struct fuse_open_in *in);
	void (*read)(const struct ufuse_req *req, uint64_t ino, const struct fuse_read_in *in);
	void (*write)(const struct ufuse_req *req, uint64_t ino, const struct fuse_write_in *in, const void *data,
	              size_t nbytes);
	void (*statfs)(const struct ufuse_req *req, uint64_t ino);
	void (*release)(const struct ufuse_req *req, uint64_t ino, const struct fuse_release_in *in);
	void (*fsync)(const struct ufuse_req *req, uint64_t ino, const struct fuse_fsync_in *in);
	void (*setxattr)(const struct ufuse_req *req, uint64_t ino, const struct fuse_setxattr_in *in, const char *name,
	                 const void *value, size_t nbytes);
	void (*getxattr)(const struct ufuse_req *req, uint64_t ino, const struct fuse_getxattr_in *in,
	                 const char *name);
	void (*listxattr)(const struct ufuse_req *req, uint64_t ino, const struct fuse_getxattr_in *in);
	void (*removexattr)(const struct ufuse_req *req, uint64_t ino, const char *name);
	void (*flush)(const struct ufuse_req *req, uint64_t ino, const struct fuse_flush_in *in);
	void (*opendir)(const struct ufuse_req *req, uint64_t ino, const struct fuse_open_in *in);
	void (*readdir)(const struct ufuse_req *req, uint64_t ino, const struct fuse_read_in *in);
	void (*releasedir)(const struct ufuse_req *req, uint64_t ino, const struct fuse_release_in *in);
	void (*fsyncdir)(const struct ufuse_req *req, uint64_t ino, const struct fuse_fsync_in *in);
	void (*getlk)(const struct ufuse_req *req, uint64_t ino, const struct fuse_lk_in *in);
	void (*setlk)(const struct ufuse_req *req, uint64_t ino, const struct fuse_lk_in *in);
	void (*setlkw)(const struct ufuse_req *req, uint64_t ino, const struct fuse_lk_in *in);
	void (*access)(const struct ufuse_req *req, uint64_t ino, const struct fuse_access_in *in);
	void (*create)(const struct ufuse_req *req, uint64_t parent, const struct fuse_create_in *in, const char *name);
	void (*bmap)(const struct ufuse_req *req, uint64_t ino, const struct fuse_bmap_in *in);
	void (*ioctl)(const struct ufuse_req *req, uint64_t ino, const struct fuse_ioctl_in *in, const void *data,
	              size_t nbytes);
	void (*poll)(const struct ufuse_req *req, uint64_t ino, const struct fuse_poll_in *in);
	void (*fallocate)(const struct ufuse_req *req, uint64_t ino, const struct fuse_fallocate_in *in);
	/// Entries are packed with ::ufuse_direntplus_add
	void (*readdirplus)(const struct ufuse_req *req, uint64_t ino, const struct fuse_read_in *in);
	void (*rename2)(const struct ufuse_req *req, uint64_t parent, const struct fuse_rename2_in *in,
	                const char *name, const char *newname);
	void (*lseek)(const struct ufuse_req *req, uint64_t ino, const struct fuse_lseek_in *in);
	void (*copy_file_range)(const struct ufuse_req *req, uint64_t ino, const struct fuse_copy_file_range_in *in);
	void (*syncfs)(const struct ufuse_req *req, uint64_t ino, const struct fuse_syncfs_in *in);
	void (*tmpfile)(const struct ufuse_req *req, uint64_t parent, const struct fuse_create_in *in,
	                const char *name);
	void (*statx)(const struct ufuse_req *req, uint64_t ino, const struct fuse_statx_in *in);
};

/**
 * Decode the request of 'nbytes' in 'buf', as read by ::ufuse_read, and call its callback in 'ops'
 *
 * INIT, DESTROY, INTERRUPT, FORGET and BATCH_FORGET are handled here, calling their callbacks as hooks. A
 * request whose length is not exactly what its opcode carries is answered with EIO; one for an opcode without
 * a callback, or that this header does not decode, with ENOSYS. CUSE_INIT is answered with
 * ::ufuse_cuse_init_reply before dispatching.
 *
 * @param ctx The caller's, passed to every callback as req->ctx
 *
 * @return 0 when the request went to a callback or was answered, -EPROTO for a malformed request, or what
 * ::ufuse_init_reply or writing a reply dispatch made itself returned
 */
static inline int
ufuse_dispatch(struct ufuse_session *s, const struct ufuse_ops *ops, void *ctx, const void *buf, size_t nbytes)
{
	const struct ufuse_opdesc *d;
	const char *names[2] = {NULL, NULL};
	const uint8_t *data;
	const uint8_t *end;
	struct ufuse_req req;
	size_t data_nbytes;
	uint32_t i;

	if (ufuse_req_parse(s, buf, nbytes, &req)) {
		return -EPROTO;
	}
	req.ctx = ctx;
	if (req.opcode == FUSE_INIT) {
		const struct fuse_init_in *in = (const struct fuse_init_in *)req.arg;
		uint64_t want = ops->want;

		// A kernel too old for the struct is refused by the reply, without calling the hook
		if (ops->init && req.arg_nbytes >= sizeof(*in) && in->major == FUSE_KERNEL_VERSION &&
		    in->minor >= UFUSE_MIN_MINOR) {
			ops->init(&req, req.nodeid, in, &want);
		}
		return ufuse_init_reply(s, &req, want);
	}
	d = ufuse_opdesc(req.opcode);
	if (!d) {
		return ufuse_req_reply_err(&req, ENOSYS);
	}
	// After INIT the kernel sends the layout of the agreed minor, s->minor, whose structs all have the size
	// this header gives them, so any other length is malformed
	if (req.arg_nbytes < d->in_nbytes) {
		goto malformed;
	}
	data = (const uint8_t *)req.arg + d->in_nbytes;
	end = (const uint8_t *)req.arg + req.arg_nbytes;
	for (i = 0; i < d->nnames; i++) {
		const uint8_t *nul = (const uint8_t *)memchr(data, 0, (size_t)(end - data));

		if (!nul) {
			goto malformed;
		}
		names[i] = (const char *)data;
		data = nul + 1;
	}
	data_nbytes = (size_t)(end - data);
	if (!(d->flags & UFUSE_OP_DATA) && data_nbytes) {
		goto malformed;
	}

	if (ops->flags & UFUSE_READONLY) {
		int mutates = d->flags & UFUSE_OP_MUTATES;

		if (req.opcode == FUSE_OPEN) {
			uint32_t flags = ((const struct fuse_open_in *)req.arg)->flags;

			mutates = (flags & O_ACCMODE) != O_RDONLY || (flags & O_TRUNC);
		}
		if (mutates) {
			return ufuse_req_reply_err(&req, EROFS);
		}
	}

// Calls 'cb' with the request, its node id and the arguments that follow, or answers ENOSYS without it
#define UFUSE_CALL(cb, ...)                                                                                 \
	do {                                                                                                \
		if (!ops->cb) {                                                                             \
			return ufuse_req_reply_err(&req, ENOSYS);                                           \
		}                                                                                           \
		ops->cb(&req, req.nodeid, __VA_ARGS__);                                                     \
		return 0;                                                                                   \
	} while (0)
#define UFUSE_CALL0(cb)                                                                                     \
	do {                                                                                                \
		if (!ops->cb) {                                                                             \
			return ufuse_req_reply_err(&req, ENOSYS);                                           \
		}                                                                                           \
		ops->cb(&req, req.nodeid);                                                                  \
		return 0;                                                                                   \
	} while (0)
#define UFUSE_IN(type) ((const struct type *)req.arg)

	switch (req.opcode) {
	case FUSE_DESTROY:
		if (ops->destroy) {
			ops->destroy(&req, req.nodeid);
		}
		return ufuse_req_reply(&req, NULL, 0);
	case FUSE_INTERRUPT:
		if (ops->interrupt) {
			ops->interrupt(&req, req.nodeid, UFUSE_IN(fuse_interrupt_in));
		}
		return 0;
	case FUSE_FORGET:
		if (ops->forget) {
			ops->forget(&req, req.nodeid, UFUSE_IN(fuse_forget_in));
		}
		return 0;
	case FUSE_BATCH_FORGET: {
		uint32_t count = UFUSE_IN(fuse_batch_forget_in)->count;

		if (data_nbytes != (size_t)count * sizeof(struct fuse_forget_one)) {
			goto malformed;
		}
		for (i = 0; ops->forget && i < count; i++) {
			struct fuse_forget_one one;
			struct fuse_forget_in in;

			memcpy(&one, data + i * sizeof(one), sizeof(one));
			memset(&in, 0, sizeof(in));
			in.nlookup = one.nlookup;
			ops->forget(&req, one.nodeid, &in);
		}
		return 0;
	}
	case FUSE_LOOKUP:
		UFUSE_CALL(lookup, names[0]);
	case FUSE_GETATTR:
		UFUSE_CALL(getattr, UFUSE_IN(fuse_getattr_in));
	case FUSE_SETATTR:
		UFUSE_CALL(setattr, UFUSE_IN(fuse_setattr_in));
	case FUSE_READLINK:
		UFUSE_CALL0(readlink);
	case FUSE_SYMLINK:
		UFUSE_CALL(symlink, names[0], names[1]);
	case FUSE_MKNOD:
		UFUSE_CALL(mknod, UFUSE_IN(fuse_mknod_in), names[0]);
	case FUSE_MKDIR:
		UFUSE_CALL(mkdir, UFUSE_IN(fuse_mkdir_in), names[0]);
	case FUSE_UNLINK:
		UFUSE_CALL(unlink, names[0]);
	case FUSE_RMDIR:
		UFUSE_CALL(rmdir, names[0]);
	case FUSE_RENAME:
		UFUSE_CALL(rename, UFUSE_IN(fuse_rename_in), names[0], names[1]);
	case FUSE_LINK:
		UFUSE_CALL(link, UFUSE_IN(fuse_link_in), names[0]);
	case FUSE_OPEN:
		UFUSE_CALL(open, UFUSE_IN(fuse_open_in));
	case FUSE_READ:
		UFUSE_CALL(read, UFUSE_IN(fuse_read_in));
	case FUSE_WRITE:
		if (data_nbytes != UFUSE_IN(fuse_write_in)->size) {
			goto malformed;
		}
		UFUSE_CALL(write, UFUSE_IN(fuse_write_in), data, data_nbytes);
	case FUSE_STATFS:
		UFUSE_CALL0(statfs);
	case FUSE_RELEASE:
		UFUSE_CALL(release, UFUSE_IN(fuse_release_in));
	case FUSE_FSYNC:
		UFUSE_CALL(fsync, UFUSE_IN(fuse_fsync_in));
	case FUSE_SETXATTR:
		if (data_nbytes != UFUSE_IN(fuse_setxattr_in)->size) {
			goto malformed;
		}
		UFUSE_CALL(setxattr, UFUSE_IN(fuse_setxattr_in), names[0], data, data_nbytes);
	case FUSE_GETXATTR:
		UFUSE_CALL(getxattr, UFUSE_IN(fuse_getxattr_in), names[0]);
	case FUSE_LISTXATTR:
		UFUSE_CALL(listxattr, UFUSE_IN(fuse_getxattr_in));
	case FUSE_REMOVEXATTR:
		UFUSE_CALL(removexattr, names[0]);
	case FUSE_FLUSH:
		UFUSE_CALL(flush, UFUSE_IN(fuse_flush_in));
	case FUSE_OPENDIR:
		UFUSE_CALL(opendir, UFUSE_IN(fuse_open_in));
	case FUSE_READDIR:
		UFUSE_CALL(readdir, UFUSE_IN(fuse_read_in));
	case FUSE_RELEASEDIR:
		UFUSE_CALL(releasedir, UFUSE_IN(fuse_release_in));
	case FUSE_FSYNCDIR:
		UFUSE_CALL(fsyncdir, UFUSE_IN(fuse_fsync_in));
	case FUSE_GETLK:
		UFUSE_CALL(getlk, UFUSE_IN(fuse_lk_in));
	case FUSE_SETLK:
		UFUSE_CALL(setlk, UFUSE_IN(fuse_lk_in));
	case FUSE_SETLKW:
		UFUSE_CALL(setlkw, UFUSE_IN(fuse_lk_in));
	case FUSE_ACCESS:
		UFUSE_CALL(access, UFUSE_IN(fuse_access_in));
	case FUSE_CREATE:
		UFUSE_CALL(create, UFUSE_IN(fuse_create_in), names[0]);
	case FUSE_BMAP:
		UFUSE_CALL(bmap, UFUSE_IN(fuse_bmap_in));
	case FUSE_IOCTL:
		if (data_nbytes != UFUSE_IN(fuse_ioctl_in)->in_size) {
			goto malformed;
		}
		UFUSE_CALL(ioctl, UFUSE_IN(fuse_ioctl_in), data, data_nbytes);
	case FUSE_POLL:
		UFUSE_CALL(poll, UFUSE_IN(fuse_poll_in));
	case FUSE_FALLOCATE:
		UFUSE_CALL(fallocate, UFUSE_IN(fuse_fallocate_in));
	case FUSE_READDIRPLUS:
		UFUSE_CALL(readdirplus, UFUSE_IN(fuse_read_in));
	case FUSE_RENAME2:
		UFUSE_CALL(rename2, UFUSE_IN(fuse_rename2_in), names[0], names[1]);
	case FUSE_LSEEK:
		UFUSE_CALL(lseek, UFUSE_IN(fuse_lseek_in));
	case FUSE_COPY_FILE_RANGE:
		UFUSE_CALL(copy_file_range, UFUSE_IN(fuse_copy_file_range_in));
	case FUSE_SYNCFS:
		UFUSE_CALL(syncfs, UFUSE_IN(fuse_syncfs_in));
	case FUSE_TMPFILE:
		UFUSE_CALL(tmpfile, UFUSE_IN(fuse_create_in), names[0]);
	case FUSE_STATX:
		UFUSE_CALL(statx, UFUSE_IN(fuse_statx_in));
	default:
		return ufuse_req_reply_err(&req, ENOSYS);
	}
#undef UFUSE_CALL
#undef UFUSE_CALL0
#undef UFUSE_IN

malformed:
	if (!(d->flags & UFUSE_OP_NOREPLY)) {
		ufuse_req_reply_err(&req, EIO);
	}
	return -EPROTO;
}

#ifdef __cplusplus
}
#endif

#endif
