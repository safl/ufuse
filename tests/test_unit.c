// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

// What ufuse does without a kernel: session defaults, directory entry packing, attributes, opcode names, and
// dispatch, with a socketpair standing in for /dev/fuse. Written in the subset of C and C++ both accept;
// test_unit_cxx.cpp compiles it as C++.
#include <dirent.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>

#include <ufuse.h>

static int failures;

#define CHECK(cond)                                                                                         \
	do {                                                                                                \
		if (!(cond)) {                                                                              \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);                          \
			failures++;                                                                         \
		}                                                                                           \
	} while (0)

static void
test_session(void)
{
	struct ufuse_session s;

	ufuse_session_init(&s);
	CHECK(s.fd == -1);
	CHECK(s.max_write == UFUSE_MAX_WRITE);
	CHECK(UFUSE_BUF_NBYTES >= UFUSE_MAX_WRITE + sizeof(struct fuse_in_header) + sizeof(struct fuse_write_in));
	CHECK(UFUSE_BUF_NBYTES >= FUSE_MIN_READ_BUFFER);
	// Closing a session that was never opened, and unmounting nothing, are both harmless
	CHECK(ufuse_unmount(&s, NULL) == 0);
	ufuse_cuse_close(&s);
}

static void
test_dirent(void)
{
	uint8_t buf[256];
	const struct fuse_dirent *d;
	size_t used = 0;
	size_t took;

	memset(buf, 0xff, sizeof(buf));
	took = ufuse_dirent_add(buf, used, sizeof(buf), "hello", 2, 1, DT_REG);
	// 24 bytes of fixed fields, the name, padded to 8
	CHECK(took == 32);
	d = (const struct fuse_dirent *)buf;
	CHECK(d->ino == 2 && d->off == 1 && d->namelen == 5 && d->type == DT_REG);
	CHECK(memcmp(d->name, "hello\0\0\0", 8) == 0);
	used += took;

	took = ufuse_dirent_add(buf, used, sizeof(buf), "abcdefgh", 3, 2, DT_DIR);
	CHECK(took == 32);
	used += took;
	CHECK(used % 8 == 0);

	// An entry that does not fit is refused, and the reply is left as it was
	took = ufuse_dirent_add(buf, used, used + 31, "x", 4, 3, DT_REG);
	CHECK(took == 0);
	took = ufuse_dirent_add(buf, used, used + 32, "x", 4, 3, DT_REG);
	CHECK(took == 32);
}

static void
test_attr(void)
{
	struct fuse_attr a;

	ufuse_attr_fill(&a, 7, S_IFREG | 0444, 1, 1000);
	CHECK(a.ino == 7 && a.mode == (S_IFREG | 0444) && a.nlink == 1 && a.size == 1000);
	CHECK(a.blocks == 2 && a.blksize == 4096);
	CHECK(a.uid == (uint32_t)getuid());
}

static void
test_names(void)
{
	CHECK(strcmp(ufuse_opcode_name(FUSE_INIT), "INIT") == 0);
	CHECK(strcmp(ufuse_opcode_name(CUSE_INIT), "CUSE_INIT") == 0);
	CHECK(strcmp(ufuse_opcode_name(FUSE_READDIR), "READDIR") == 0);
	CHECK(strcmp(ufuse_opcode_name(FUSE_RENAME2), "RENAME2") == 0);
	CHECK(strcmp(ufuse_opcode_name(FUSE_STATX), "STATX") == 0);
	CHECK(strcmp(ufuse_opcode_name(0xdead), "UNKNOWN") == 0);
}

static void
test_direntplus(void)
{
	uint8_t buf[512];
	const struct fuse_direntplus *d;
	struct fuse_entry_out entry;
	size_t took;

	memset(&entry, 0, sizeof(entry));
	entry.nodeid = 9;
	ufuse_attr_fill(&entry.attr, 9, S_IFDIR | 0555, 2, 0);
	memset(buf, 0xff, sizeof(buf));
	took = ufuse_direntplus_add(buf, 0, sizeof(buf), "dir", &entry, 1);
	// The 128-byte entry, 24 bytes of dirent fields, the name, padded to 8
	CHECK(took == 160);
	d = (const struct fuse_direntplus *)buf;
	CHECK(d->entry_out.nodeid == 9 && d->entry_out.attr.mode == (S_IFDIR | 0555));
	CHECK(d->dirent.ino == 9 && d->dirent.off == 1 && d->dirent.namelen == 3 && d->dirent.type == DT_DIR);
	CHECK(memcmp(d->dirent.name, "dir\0\0\0\0\0", 8) == 0);
	CHECK(ufuse_direntplus_add(buf, took, 2 * took - 1, "dir", &entry, 2) == 0);
	CHECK(ufuse_direntplus_add(buf, took, 2 * took, "dir", &entry, 2) == took);
}

// The dispatch tests' kernel side: requests are built in 'buf', replies read back from 'kernel'
struct fixture {
	struct ufuse_session s;
	int kernel;
	uint64_t buf[UFUSE_BUF_NBYTES / 8];
	size_t used;
};

// What the last callback received
static struct {
	int calls;
	uint32_t opcode;
	uint64_t nodeid;
	uint8_t in[128];
	size_t in_nbytes;
	char name[64];
	char name2[64];
	uint8_t data[64];
	size_t data_nbytes;
} rec;

static struct ufuse_defer kept;

static void
fixture_open(struct fixture *f)
{
	int sv[2];

	ufuse_session_init(&f->s);
	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);
	f->s.fd = sv[0];
	f->kernel = sv[1];
	memset(&rec, 0, sizeof(rec));
}

static void
fixture_close(struct fixture *f)
{
	close(f->kernel);
	ufuse_unmount(&f->s, NULL);
}

/** Start a request with the given header and argument struct */
static void
msg_start(struct fixture *f, uint32_t opcode, uint64_t unique, uint64_t nodeid, const void *arg, size_t nbytes)
{
	struct fuse_in_header *in = (struct fuse_in_header *)f->buf;

	memset(f->buf, 0, sizeof(f->buf));
	in->opcode = opcode;
	in->unique = unique;
	in->nodeid = nodeid;
	in->uid = 1000;
	f->used = sizeof(*in);
	if (nbytes) {
		memcpy((uint8_t *)f->buf + f->used, arg, nbytes);
	}
	f->used += nbytes;
	in->len = (uint32_t)f->used;
}

/** Append bytes, e.g. a name with its NUL, to the request */
static void
msg_add(struct fixture *f, const void *data, size_t nbytes)
{
	memcpy((uint8_t *)f->buf + f->used, data, nbytes);
	f->used += nbytes;
	((struct fuse_in_header *)f->buf)->len = (uint32_t)f->used;
}

static int
msg_dispatch(struct fixture *f, const struct ufuse_ops *ops)
{
	return ufuse_dispatch(&f->s, ops, f, f->buf, f->used);
}

/** Read one reply; returns its length, or 0 when there is none */
static size_t
reply_read(struct fixture *f, struct fuse_out_header *out, void *payload, size_t nbytes)
{
	uint8_t msg[4096];
	ssize_t n = recv(f->kernel, msg, sizeof(msg), MSG_DONTWAIT);
	size_t got;

	if (n < (ssize_t)sizeof(*out)) {
		return 0;
	}
	memcpy(out, msg, sizeof(*out));
	CHECK(out->len == (uint32_t)n);
	got = (size_t)n - sizeof(*out);
	if (payload) {
		memcpy(payload, msg + sizeof(*out), got < nbytes ? got : nbytes);
	}
	return (size_t)n;
}

/** Note what a callback received, and answer it unless its opcode takes no reply */
static void
record(const struct ufuse_req *req, uint64_t nodeid, const void *in, size_t in_nbytes, const char *name,
       const char *name2, const void *data, size_t data_nbytes)
{
	rec.calls++;
	rec.opcode = req->opcode;
	rec.nodeid = nodeid;
	rec.in_nbytes = in_nbytes;
	if (in_nbytes) {
		memcpy(rec.in, in, in_nbytes < sizeof(rec.in) ? in_nbytes : sizeof(rec.in));
	}
	snprintf(rec.name, sizeof(rec.name), "%s", name ? name : "");
	snprintf(rec.name2, sizeof(rec.name2), "%s", name2 ? name2 : "");
	rec.data_nbytes = data_nbytes;
	if (data_nbytes) {
		memcpy(rec.data, data, data_nbytes < sizeof(rec.data) ? data_nbytes : sizeof(rec.data));
	}
	if (!(ufuse_opdesc(req->opcode)->flags & UFUSE_OP_NOREPLY)) {
		ufuse_req_reply(req, NULL, 0);
	}
}

// One recording callback per signature the callback rule gives
#define CB(op, type)                                                                                        \
	static void cb_##op(const struct ufuse_req *req, uint64_t nodeid, const struct type *in)            \
	{                                                                                                   \
		record(req, nodeid, in, sizeof(*in), NULL, NULL, NULL, 0);                                  \
	}
#define CB_NAME(op, type)                                                                                   \
	static void cb_##op(const struct ufuse_req *req, uint64_t nodeid, const struct type *in, const char *name) \
	{                                                                                                   \
		record(req, nodeid, in, sizeof(*in), name, NULL, NULL, 0);                                  \
	}
#define CB_NAMES(op, type)                                                                                  \
	static void cb_##op(const struct ufuse_req *req, uint64_t nodeid, const struct type *in, const char *name, \
	                    const char *name2)                                                              \
	{                                                                                                   \
		record(req, nodeid, in, sizeof(*in), name, name2, NULL, 0);                                 \
	}
#define CB_DATA(op, type)                                                                                   \
	static void cb_##op(const struct ufuse_req *req, uint64_t nodeid, const struct type *in, const void *data, \
	                    size_t nbytes)                                                                  \
	{                                                                                                   \
		record(req, nodeid, in, sizeof(*in), NULL, NULL, data, nbytes);                             \
	}
CB(forget, fuse_forget_in)
CB(interrupt, fuse_interrupt_in)
CB(getattr, fuse_getattr_in)
CB(setattr, fuse_setattr_in)
CB(open, fuse_open_in)
CB(read, fuse_read_in)
CB(release, fuse_release_in)
CB(fsync, fuse_fsync_in)
CB(listxattr, fuse_getxattr_in)
CB(flush, fuse_flush_in)
CB(opendir, fuse_open_in)
CB(readdir, fuse_read_in)
CB(releasedir, fuse_release_in)
CB(fsyncdir, fuse_fsync_in)
CB(getlk, fuse_lk_in)
CB(setlk, fuse_lk_in)
CB(setlkw, fuse_lk_in)
CB(access, fuse_access_in)
CB(bmap, fuse_bmap_in)
CB(poll, fuse_poll_in)
CB(fallocate, fuse_fallocate_in)
CB(readdirplus, fuse_read_in)
CB(lseek, fuse_lseek_in)
CB(copy_file_range, fuse_copy_file_range_in)
CB(syncfs, fuse_syncfs_in)
CB(statx, fuse_statx_in)
CB_NAME(mknod, fuse_mknod_in)
CB_NAME(mkdir, fuse_mkdir_in)
CB_NAME(link, fuse_link_in)
CB_NAME(getxattr, fuse_getxattr_in)
CB_NAME(create, fuse_create_in)
CB_NAME(tmpfile, fuse_create_in)
CB_NAMES(rename, fuse_rename_in)
CB_NAMES(rename2, fuse_rename2_in)
CB_DATA(write, fuse_write_in)
CB_DATA(ioctl, fuse_ioctl_in)

static void
cb_lookup(const struct ufuse_req *req, uint64_t nodeid, const char *name)
{
	record(req, nodeid, NULL, 0, name, NULL, NULL, 0);
}

static void
cb_unlink(const struct ufuse_req *req, uint64_t nodeid, const char *name)
{
	record(req, nodeid, NULL, 0, name, NULL, NULL, 0);
}

static void
cb_rmdir(const struct ufuse_req *req, uint64_t nodeid, const char *name)
{
	record(req, nodeid, NULL, 0, name, NULL, NULL, 0);
}

static void
cb_removexattr(const struct ufuse_req *req, uint64_t nodeid, const char *name)
{
	record(req, nodeid, NULL, 0, name, NULL, NULL, 0);
}

static void
cb_symlink(const struct ufuse_req *req, uint64_t nodeid, const char *name, const char *target)
{
	record(req, nodeid, NULL, 0, name, target, NULL, 0);
}

static void
cb_readlink(const struct ufuse_req *req, uint64_t nodeid)
{
	record(req, nodeid, NULL, 0, NULL, NULL, NULL, 0);
}

static void
cb_statfs(const struct ufuse_req *req, uint64_t nodeid)
{
	record(req, nodeid, NULL, 0, NULL, NULL, NULL, 0);
}

static void
cb_setxattr(const struct ufuse_req *req, uint64_t nodeid, const struct fuse_setxattr_in *in, const char *name,
            const void *value, size_t nbytes)
{
	record(req, nodeid, in, sizeof(*in), name, NULL, value, nbytes);
}

static void
ops_all(struct ufuse_ops *ops)
{
	memset(ops, 0, sizeof(*ops));
	ops->forget = cb_forget;
	ops->interrupt = cb_interrupt;
	ops->lookup = cb_lookup;
	ops->getattr = cb_getattr;
	ops->setattr = cb_setattr;
	ops->readlink = cb_readlink;
	ops->symlink = cb_symlink;
	ops->mknod = cb_mknod;
	ops->mkdir = cb_mkdir;
	ops->unlink = cb_unlink;
	ops->rmdir = cb_rmdir;
	ops->rename = cb_rename;
	ops->link = cb_link;
	ops->open = cb_open;
	ops->read = cb_read;
	ops->write = cb_write;
	ops->statfs = cb_statfs;
	ops->release = cb_release;
	ops->fsync = cb_fsync;
	ops->setxattr = cb_setxattr;
	ops->getxattr = cb_getxattr;
	ops->listxattr = cb_listxattr;
	ops->removexattr = cb_removexattr;
	ops->flush = cb_flush;
	ops->opendir = cb_opendir;
	ops->readdir = cb_readdir;
	ops->releasedir = cb_releasedir;
	ops->fsyncdir = cb_fsyncdir;
	ops->getlk = cb_getlk;
	ops->setlk = cb_setlk;
	ops->setlkw = cb_setlkw;
	ops->access = cb_access;
	ops->create = cb_create;
	ops->bmap = cb_bmap;
	ops->ioctl = cb_ioctl;
	ops->poll = cb_poll;
	ops->fallocate = cb_fallocate;
	ops->readdirplus = cb_readdirplus;
	ops->rename2 = cb_rename2;
	ops->lseek = cb_lseek;
	ops->copy_file_range = cb_copy_file_range;
	ops->syncfs = cb_syncfs;
	ops->tmpfile = cb_tmpfile;
	ops->statx = cb_statx;
}

/**
 * A request as the kernel sends it: the argument struct at its size since protocol 7.39, written out rather
 * than taken from sizeof so a header that changed one fails here, then names and data
 */
struct opcase {
	uint32_t opcode;
	size_t in_nbytes;
	const char *name;
	const char *name2;
	size_t data_nbytes;
};

static const struct opcase opcases[] = {
	{FUSE_LOOKUP, 0, "hello", NULL, 0},
	{FUSE_GETATTR, 16, NULL, NULL, 0},
	{FUSE_SETATTR, 88, NULL, NULL, 0},
	{FUSE_READLINK, 0, NULL, NULL, 0},
	{FUSE_SYMLINK, 0, "link", "target", 0},
	{FUSE_MKNOD, 16, "node", NULL, 0},
	{FUSE_MKDIR, 8, "dir", NULL, 0},
	{FUSE_UNLINK, 0, "file", NULL, 0},
	{FUSE_RMDIR, 0, "dir", NULL, 0},
	{FUSE_RENAME, 8, "from", "to", 0},
	{FUSE_LINK, 8, "newname", NULL, 0},
	{FUSE_OPEN, 8, NULL, NULL, 0},
	{FUSE_READ, 40, NULL, NULL, 0},
	{FUSE_WRITE, 40, NULL, NULL, 5},
	{FUSE_STATFS, 0, NULL, NULL, 0},
	{FUSE_RELEASE, 24, NULL, NULL, 0},
	{FUSE_FSYNC, 16, NULL, NULL, 0},
	{FUSE_SETXATTR, 16, "user.key", NULL, 6},
	{FUSE_GETXATTR, 8, "user.key", NULL, 0},
	{FUSE_LISTXATTR, 8, NULL, NULL, 0},
	{FUSE_REMOVEXATTR, 0, "user.key", NULL, 0},
	{FUSE_FLUSH, 24, NULL, NULL, 0},
	{FUSE_OPENDIR, 8, NULL, NULL, 0},
	{FUSE_READDIR, 40, NULL, NULL, 0},
	{FUSE_RELEASEDIR, 24, NULL, NULL, 0},
	{FUSE_FSYNCDIR, 16, NULL, NULL, 0},
	{FUSE_GETLK, 48, NULL, NULL, 0},
	{FUSE_SETLK, 48, NULL, NULL, 0},
	{FUSE_SETLKW, 48, NULL, NULL, 0},
	{FUSE_ACCESS, 8, NULL, NULL, 0},
	{FUSE_CREATE, 16, "file", NULL, 0},
	{FUSE_BMAP, 16, NULL, NULL, 0},
	{FUSE_IOCTL, 32, NULL, NULL, 7},
	{FUSE_POLL, 24, NULL, NULL, 0},
	{FUSE_FALLOCATE, 32, NULL, NULL, 0},
	{FUSE_READDIRPLUS, 40, NULL, NULL, 0},
	{FUSE_RENAME2, 16, "from", "to", 0},
	{FUSE_LSEEK, 24, NULL, NULL, 0},
	{FUSE_COPY_FILE_RANGE, 56, NULL, NULL, 0},
	{FUSE_SYNCFS, 8, NULL, NULL, 0},
	{FUSE_TMPFILE, 16, "tmp", NULL, 0},
	{FUSE_STATX, 24, NULL, NULL, 0},
};

/** Build the request for 'c', with 'extra' bytes more or one fewer when -1 */
static void
opcase_build(struct fixture *f, const struct opcase *c, uint64_t unique, int extra)
{
	uint8_t in[128];
	uint8_t data[16];
	size_t i;

	for (i = 0; i < sizeof(in); i++) {
		in[i] = (uint8_t)(c->opcode + i);
	}
	for (i = 0; i < sizeof(data); i++) {
		data[i] = (uint8_t)(0xa0 + i);
	}
	// The struct gives the length of the data that follows it
	if (c->opcode == FUSE_WRITE) {
		((struct fuse_write_in *)in)->size = (uint32_t)c->data_nbytes;
	} else if (c->opcode == FUSE_IOCTL) {
		((struct fuse_ioctl_in *)in)->in_size = (uint32_t)c->data_nbytes;
	} else if (c->opcode == FUSE_SETXATTR) {
		((struct fuse_setxattr_in *)in)->size = (uint32_t)c->data_nbytes;
	}
	msg_start(f, c->opcode, unique, 77, in, c->in_nbytes);
	if (c->name) {
		msg_add(f, c->name, strlen(c->name) + 1);
	}
	if (c->name2) {
		msg_add(f, c->name2, strlen(c->name2) + 1);
	}
	msg_add(f, data, c->data_nbytes);
	if (extra > 0) {
		msg_add(f, data, (size_t)extra);
	} else if (extra < 0) {
		f->used--;
		((struct fuse_in_header *)f->buf)->len = (uint32_t)f->used;
	}
}

static void
test_dispatch_opcodes(void)
{
	struct fixture f;
	struct ufuse_ops ops;
	struct fuse_out_header out;
	size_t i;

	ops_all(&ops);
	fixture_open(&f);
	for (i = 0; i < sizeof(opcases) / sizeof(opcases[0]); i++) {
		const struct opcase *c = &opcases[i];
		uint64_t unique = 100 + i;
		uint8_t want_in[128];
		size_t j;

		for (j = 0; j < sizeof(want_in); j++) {
			want_in[j] = (uint8_t)(c->opcode + j);
		}
		memset(&rec, 0, sizeof(rec));
		opcase_build(&f, c, unique, 0);
		if (msg_dispatch(&f, &ops) != 0 || rec.calls != 1) {
			fprintf(stderr, "%s: not dispatched\n", ufuse_opcode_name(c->opcode));
			failures++;
			continue;
		}
		CHECK(rec.opcode == c->opcode && rec.nodeid == 77 && rec.in_nbytes == c->in_nbytes);
		if (c->opcode != FUSE_WRITE && c->opcode != FUSE_IOCTL && c->opcode != FUSE_SETXATTR) {
			CHECK(memcmp(rec.in, want_in, c->in_nbytes) == 0);
		}
		CHECK(strcmp(rec.name, c->name ? c->name : "") == 0);
		CHECK(strcmp(rec.name2, c->name2 ? c->name2 : "") == 0);
		CHECK(rec.data_nbytes == c->data_nbytes);
		for (j = 0; j < c->data_nbytes; j++) {
			CHECK(rec.data[j] == 0xa0 + j);
		}
		CHECK(reply_read(&f, &out, NULL, 0) == sizeof(out));
		CHECK(out.unique == unique && out.error == 0);

		// A byte too many, or too few, is not what the kernel sends, and reaches no callback
		opcase_build(&f, c, unique, 1);
		CHECK(msg_dispatch(&f, &ops) == -EPROTO);
		CHECK(reply_read(&f, &out, NULL, 0) == sizeof(out));
		CHECK(out.unique == unique && out.error == -EIO);
		if (c->in_nbytes || c->name || c->data_nbytes) {
			opcase_build(&f, c, unique, -1);
			CHECK(msg_dispatch(&f, &ops) == -EPROTO);
			CHECK(reply_read(&f, &out, NULL, 0) == sizeof(out));
			CHECK(out.unique == unique && out.error == -EIO);
		}
		CHECK(rec.calls == 1);
		CHECK(ufuse_opdesc(c->opcode) && ufuse_opdesc(c->opcode)->in_nbytes == c->in_nbytes);
	}
	fixture_close(&f);
}

static void
test_dispatch_refuse(void)
{
	struct fixture f;
	struct ufuse_ops ops;
	struct fuse_out_header out;
	struct fuse_write_in w;
	struct fuse_open_in o;

	memset(&ops, 0, sizeof(ops));
	ops.lookup = cb_lookup;
	ops.write = cb_write;
	fixture_open(&f);

	// A length that disagrees with the header cannot be answered at all
	msg_start(&f, FUSE_LOOKUP, 24, FUSE_ROOT_ID, "x", 2);
	CHECK(ufuse_dispatch(&f.s, &ops, NULL, f.buf, f.used - 1) == -EPROTO);
	CHECK(reply_read(&f, &out, NULL, 0) == 0);
	CHECK(rec.calls == 0);

	// No callback, and an opcode this header does not decode, are ENOSYS
	msg_start(&f, FUSE_STATFS, 25, FUSE_ROOT_ID, NULL, 0);
	CHECK(msg_dispatch(&f, &ops) == 0);
	CHECK(reply_read(&f, &out, NULL, 0) == sizeof(out));
	CHECK(out.unique == 25 && out.error == -ENOSYS);
	msg_start(&f, FUSE_SETUPMAPPING, 26, FUSE_ROOT_ID, NULL, 0);
	CHECK(msg_dispatch(&f, &ops) == 0);
	CHECK(reply_read(&f, &out, NULL, 0) == sizeof(out));
	CHECK(out.unique == 26 && out.error == -ENOSYS);

	// Read-only refuses a write, and an open for writing or truncating, even with callbacks for them
	ops.flags = UFUSE_READONLY;
	memset(&w, 0, sizeof(w));
	w.size = 3;
	msg_start(&f, FUSE_WRITE, 27, 2, &w, sizeof(w));
	msg_add(&f, "abc", 3);
	CHECK(msg_dispatch(&f, &ops) == 0);
	CHECK(reply_read(&f, &out, NULL, 0) == sizeof(out));
	CHECK(out.unique == 27 && out.error == -EROFS);
	memset(&o, 0, sizeof(o));
	o.flags = O_RDWR;
	msg_start(&f, FUSE_OPEN, 28, 2, &o, sizeof(o));
	CHECK(msg_dispatch(&f, &ops) == 0);
	CHECK(reply_read(&f, &out, NULL, 0) == sizeof(out));
	CHECK(out.unique == 28 && out.error == -EROFS);
	o.flags = O_RDONLY | O_TRUNC;
	msg_start(&f, FUSE_OPEN, 29, 2, &o, sizeof(o));
	CHECK(msg_dispatch(&f, &ops) == 0);
	CHECK(reply_read(&f, &out, NULL, 0) == sizeof(out));
	CHECK(out.unique == 29 && out.error == -EROFS);
	CHECK(rec.calls == 0);
	// Without the flag, the same write reaches its callback
	ops.flags = 0;
	msg_start(&f, FUSE_WRITE, 30, 2, &w, sizeof(w));
	msg_add(&f, "abc", 3);
	CHECK(msg_dispatch(&f, &ops) == 0);
	CHECK(rec.calls == 1 && rec.data_nbytes == 3 && memcmp(rec.data, "abc", 3) == 0);
	CHECK(reply_read(&f, &out, NULL, 0) == sizeof(out));
	CHECK(out.unique == 30 && out.error == 0);
	fixture_close(&f);
}

static void
test_dispatch_noreply(void)
{
	struct fixture f;
	struct ufuse_ops ops;
	struct fuse_out_header out;
	struct fuse_forget_in fg;
	struct fuse_batch_forget_in bf;
	struct fuse_forget_one one[2];
	struct fuse_interrupt_in intr;

	memset(&ops, 0, sizeof(ops));
	fixture_open(&f);

	// Forgets and interrupts are not answered, with or without a callback
	memset(&fg, 0, sizeof(fg));
	fg.nlookup = 3;
	msg_start(&f, FUSE_FORGET, 31, 9, &fg, sizeof(fg));
	CHECK(msg_dispatch(&f, &ops) == 0);
	memset(&intr, 0, sizeof(intr));
	intr.unique = 31;
	msg_start(&f, FUSE_INTERRUPT, 32, 0, &intr, sizeof(intr));
	CHECK(msg_dispatch(&f, &ops) == 0);
	ops.forget = cb_forget;
	ops.interrupt = cb_interrupt;
	msg_start(&f, FUSE_FORGET, 33, 9, &fg, sizeof(fg));
	CHECK(msg_dispatch(&f, &ops) == 0);
	CHECK(rec.calls == 1 && rec.nodeid == 9 && ((struct fuse_forget_in *)rec.in)->nlookup == 3);
	msg_start(&f, FUSE_INTERRUPT, 34, 0, &intr, sizeof(intr));
	CHECK(msg_dispatch(&f, &ops) == 0);
	CHECK(rec.calls == 2 && ((struct fuse_interrupt_in *)rec.in)->unique == 31);

	// A batch is one callback per entry
	memset(&bf, 0, sizeof(bf));
	bf.count = 2;
	one[0].nodeid = 10;
	one[0].nlookup = 1;
	one[1].nodeid = 20;
	one[1].nlookup = 2;
	msg_start(&f, FUSE_BATCH_FORGET, 35, 0, &bf, sizeof(bf));
	msg_add(&f, one, sizeof(one));
	CHECK(msg_dispatch(&f, &ops) == 0);
	CHECK(rec.calls == 4 && rec.nodeid == 20 && ((struct fuse_forget_in *)rec.in)->nlookup == 2);
	// A batch claiming more entries than it carries is malformed, and still not answered
	bf.count = 3;
	msg_start(&f, FUSE_BATCH_FORGET, 36, 0, &bf, sizeof(bf));
	msg_add(&f, one, sizeof(one));
	CHECK(msg_dispatch(&f, &ops) == -EPROTO);
	CHECK(rec.calls == 4);
	CHECK(reply_read(&f, &out, NULL, 0) == 0);
	fixture_close(&f);
}

// Keeps the request and returns without replying, as a filesystem waiting on its own I/O would
static void
cb_read_deferred(const struct ufuse_req *req, uint64_t ino, const struct fuse_read_in *in)
{
	(void)ino;
	(void)in;
	rec.calls++;
	kept = ufuse_req_defer(req);
}

static void *
reply_kept(void *arg)
{
	static const char data[] = "later";

	(void)arg;
	CHECK(ufuse_defer_reply(&kept, data, sizeof(data)) == 0);
	return NULL;
}

static void
test_dispatch_deferred(void)
{
	struct fixture f;
	struct ufuse_ops ops;
	struct fuse_out_header out;
	struct fuse_read_in rd;
	char payload[16];
	pthread_t thread;

	memset(&ops, 0, sizeof(ops));
	ops.read = cb_read_deferred;
	fixture_open(&f);

	memset(&rd, 0, sizeof(rd));
	rd.size = 4096;
	msg_start(&f, FUSE_READ, 41, 2, &rd, sizeof(rd));
	CHECK(msg_dispatch(&f, &ops) == 0);
	CHECK(rec.calls == 1);
	// Dispatch did not answer for the callback
	CHECK(reply_read(&f, &out, NULL, 0) == 0);

	// The buffer is overwritten by the next request before the kept one is answered, from another thread
	msg_start(&f, FUSE_STATFS, 42, FUSE_ROOT_ID, NULL, 0);
	CHECK(msg_dispatch(&f, &ops) == 0);
	CHECK(reply_read(&f, &out, NULL, 0) == sizeof(out));
	CHECK(out.unique == 42 && out.error == -ENOSYS);
	CHECK(kept.unique == 41 && kept.s == &f.s && kept.ctx == &f);
	CHECK(pthread_create(&thread, NULL, reply_kept, NULL) == 0);
	CHECK(pthread_join(thread, NULL) == 0);
	memset(payload, 0, sizeof(payload));
	CHECK(reply_read(&f, &out, payload, sizeof(payload)) == sizeof(out) + 6);
	CHECK(out.unique == 41 && out.error == 0 && strcmp(payload, "later") == 0);
	CHECK(ufuse_defer_reply_err(&kept, EIO) == 0);
	CHECK(reply_read(&f, &out, NULL, 0) == sizeof(out));
	CHECK(out.unique == 41 && out.error == -EIO);
	fixture_close(&f);
}

static void
cb_init(const struct ufuse_req *req, uint64_t nodeid, const struct fuse_init_in *in, uint64_t *want)
{
	(void)req;
	(void)nodeid;
	CHECK(in->minor >= UFUSE_MIN_MINOR);
	rec.calls++;
	*want |= FUSE_BIG_WRITES;
}

static void
test_dispatch_init(void)
{
	struct fixture f;
	struct ufuse_ops ops;
	struct fuse_out_header out;
	struct fuse_init_in in;
	struct fuse_init_out init;

	memset(&ops, 0, sizeof(ops));
	ops.want = FUSE_ASYNC_READ;
	ops.init = cb_init;
	fixture_open(&f);

	CHECK(sizeof(in) == 64);
	memset(&in, 0, sizeof(in));
	in.major = FUSE_KERNEL_VERSION;
	in.minor = FUSE_KERNEL_MINOR_VERSION;
	in.max_readahead = 65536;
	in.flags = FUSE_ASYNC_READ | FUSE_BIG_WRITES | FUSE_EXPORT_SUPPORT | FUSE_SETXATTR_EXT | FUSE_INIT_EXT;
	in.flags2 = (uint32_t)(FUSE_SECURITY_CTX >> 32);
	msg_start(&f, FUSE_INIT, 51, 0, &in, sizeof(in));
	CHECK(msg_dispatch(&f, &ops) == 0);
	CHECK(rec.calls == 1);
	memset(&init, 0, sizeof(init));
	CHECK(reply_read(&f, &out, &init, sizeof(init)) == sizeof(out) + sizeof(init));
	CHECK(out.unique == 51 && out.error == 0);
	CHECK(init.major == FUSE_KERNEL_VERSION && init.minor == FUSE_KERNEL_MINOR_VERSION);
	// What the hook added is granted; what was offered but not wanted is not
	CHECK(init.flags & FUSE_ASYNC_READ);
	CHECK(init.flags & FUSE_BIG_WRITES);
	CHECK(!(init.flags & FUSE_EXPORT_SUPPORT));
	CHECK(f.s.flags & FUSE_BIG_WRITES);
	// SETXATTR_EXT is always granted, and the request extensions never, so each struct has one size
	CHECK(init.flags & FUSE_SETXATTR_EXT);
	CHECK(init.flags2 == 0 && !(f.s.flags & FUSE_SECURITY_CTX));

	// DESTROY is answered by dispatch
	msg_start(&f, FUSE_DESTROY, 52, 0, NULL, 0);
	CHECK(msg_dispatch(&f, &ops) == 0);
	CHECK(reply_read(&f, &out, NULL, 0) == sizeof(out));
	CHECK(out.unique == 52 && out.error == 0);

	// Kernels older than 7.39, a struct of another size, and a kernel without SETXATTR_EXT fail INIT,
	// without calling the hook
	in.minor = 38;
	msg_start(&f, FUSE_INIT, 53, 0, &in, sizeof(in));
	CHECK(msg_dispatch(&f, &ops) == -EPROTO);
	CHECK(reply_read(&f, &out, NULL, 0) == sizeof(out));
	CHECK(out.unique == 53 && out.error == -EPROTO);
	in.minor = 39;
	msg_start(&f, FUSE_INIT, 54, 0, &in, sizeof(in) - 4);
	CHECK(msg_dispatch(&f, &ops) == -EPROTO);
	CHECK(reply_read(&f, &out, NULL, 0) == sizeof(out));
	CHECK(out.unique == 54 && out.error == -EPROTO);
	CHECK(rec.calls == 1);
	// A newer kernel's longer struct is accepted, and told the header's minor
	in.minor = FUSE_KERNEL_MINOR_VERSION + 1;
	msg_start(&f, FUSE_INIT, 56, 0, &in, sizeof(in));
	msg_add(&f, "\xff\xff\xff\xff\xff\xff\xff\xff", 8);
	CHECK(msg_dispatch(&f, &ops) == 0);
	CHECK(rec.calls == 2);
	memset(&init, 0, sizeof(init));
	CHECK(reply_read(&f, &out, &init, sizeof(init)) == sizeof(out) + sizeof(init));
	CHECK(out.unique == 56 && out.error == 0);
	CHECK(init.minor == FUSE_KERNEL_MINOR_VERSION && f.s.minor == FUSE_KERNEL_MINOR_VERSION);
	CHECK(init.flags & FUSE_SETXATTR_EXT);
	in.flags &= ~(uint32_t)FUSE_SETXATTR_EXT;
	msg_start(&f, FUSE_INIT, 55, 0, &in, sizeof(in));
	CHECK(msg_dispatch(&f, &ops) == -ENOTSUP);
	CHECK(reply_read(&f, &out, NULL, 0) == sizeof(out));
	CHECK(out.unique == 55 && out.error == -EPROTO);
	fixture_close(&f);
}

int
main(void)
{
	test_session();
	test_dirent();
	test_attr();
	test_names();
	test_direntplus();
	test_dispatch_opcodes();
	test_dispatch_refuse();
	test_dispatch_noreply();
	test_dispatch_deferred();
	test_dispatch_init();
	if (failures) {
		fprintf(stderr, "test_unit: %d failed\n", failures);
		return 1;
	}
	printf("test_unit: ok\n");
	return 0;
}
