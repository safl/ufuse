// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * hellofs: a read-only filesystem served from memory with ufuse
 *
 *   /hello      a short text
 *   /big        1 MiB in a pattern, see big_byte()
 *   /many/      1000 files, f0000 to f0999, each holding its own name
 *
 * Usage: hellofs MOUNTPOINT          mount, serve until unmounted or signalled, unmount
 *        hellofs --big FILE          write what /big holds to FILE, for a test to compare against
 *
 * Mounting needs CAP_SYS_ADMIN.
 */
#include <dirent.h>
#include <signal.h>
#include <stdlib.h>

#include <ufuse.h>

#define INO_ROOT FUSE_ROOT_ID
#define INO_HELLO 2
#define INO_BIG 3
#define INO_MANY 4
#define INO_FILE0 100
#define NFILES 1000
#define BIG_NBYTES (1024 * 1024)

static const char hello[] = "Hello from ufuse\n";
static volatile sig_atomic_t stopping;

static void
on_signal(int sig)
{
	(void)sig;
	stopping = 1;
}

static uint8_t
big_byte(uint64_t off)
{
	return (uint8_t)(off * 7 + off / 4096);
}

static int
is_file(uint64_t ino)
{
	return ino >= INO_FILE0 && ino < INO_FILE0 + NFILES;
}

static void
file_name(uint64_t ino, char *name, size_t nbytes)
{
	snprintf(name, nbytes, "f%04u", (unsigned int)(ino - INO_FILE0));
}

/** Fills 'attr' for 'ino'; returns 0, or ENOENT for an inode that does not exist */
static int
attr_of(uint64_t ino, struct fuse_attr *attr)
{
	if (ino == INO_ROOT) {
		ufuse_attr_fill(attr, ino, S_IFDIR | 0555, 3, 0);
	} else if (ino == INO_MANY) {
		ufuse_attr_fill(attr, ino, S_IFDIR | 0555, 2, 0);
	} else if (ino == INO_HELLO) {
		ufuse_attr_fill(attr, ino, S_IFREG | 0444, 1, sizeof(hello) - 1);
	} else if (ino == INO_BIG) {
		ufuse_attr_fill(attr, ino, S_IFREG | 0444, 1, BIG_NBYTES);
	} else if (is_file(ino)) {
		ufuse_attr_fill(attr, ino, S_IFREG | 0444, 1, 5);
	} else {
		return ENOENT;
	}
	return 0;
}

static uint64_t
lookup(uint64_t parent, const char *name)
{
	if (parent == INO_ROOT) {
		if (!strcmp(name, "hello")) {
			return INO_HELLO;
		}
		if (!strcmp(name, "big")) {
			return INO_BIG;
		}
		if (!strcmp(name, "many")) {
			return INO_MANY;
		}
	} else if (parent == INO_MANY && name[0] == 'f' && strlen(name) == 5) {
		char *end;
		unsigned long i = strtoul(name + 1, &end, 10);

		if (*end == '\0' && i < NFILES) {
			return INO_FILE0 + i;
		}
	}
	return 0;
}

/** Entry 'index' of directory 'ino'; returns 0 past the last */
static int
dir_entry(uint64_t ino, uint64_t index, char *name, size_t nbytes, uint64_t *child, uint32_t *type)
{
	static const char *const root[] = {".", "..", "hello", "big", "many"};
	static const uint64_t root_ino[] = {INO_ROOT, INO_ROOT, INO_HELLO, INO_BIG, INO_MANY};

	if (ino == INO_ROOT) {
		if (index >= 5) {
			return 0;
		}
		snprintf(name, nbytes, "%s", root[index]);
		*child = root_ino[index];
		*type = index == 2 || index == 3 ? DT_REG : DT_DIR;
		return 1;
	}
	if (index == 0 || index == 1) {
		snprintf(name, nbytes, "%s", index ? ".." : ".");
		*child = index ? INO_ROOT : INO_MANY;
		*type = DT_DIR;
		return 1;
	}
	if (index - 2 >= NFILES) {
		return 0;
	}
	*child = INO_FILE0 + index - 2;
	file_name(*child, name, nbytes);
	*type = DT_REG;
	return 1;
}

static void
serve_read(struct ufuse_session *s, const struct ufuse_req *req)
{
	const struct fuse_read_in *in = (const struct fuse_read_in *)req->arg;
	static uint8_t data[UFUSE_MAX_WRITE];
	uint64_t ino = req->in->nodeid;
	uint64_t size;
	uint64_t n;
	char name[16];

	if (ino == INO_HELLO) {
		size = sizeof(hello) - 1;
	} else if (ino == INO_BIG) {
		size = BIG_NBYTES;
	} else if (is_file(ino)) {
		size = 5;
	} else {
		ufuse_reply_err(s, req->in->unique, EISDIR);
		return;
	}
	n = in->offset >= size ? 0 : size - in->offset;
	if (n > in->size) {
		n = in->size;
	}
	if (n > sizeof(data)) {
		n = sizeof(data);
	}
	for (uint64_t i = 0; i < n; i++) {
		uint64_t off = in->offset + i;

		if (ino == INO_HELLO) {
			data[i] = (uint8_t)hello[off];
		} else if (ino == INO_BIG) {
			data[i] = big_byte(off);
		} else {
			file_name(ino, name, sizeof(name));
			data[i] = (uint8_t)name[off];
		}
	}
	ufuse_reply(s, req->in->unique, data, n);
}

static void
serve_readdir(struct ufuse_session *s, const struct ufuse_req *req)
{
	const struct fuse_read_in *in = (const struct fuse_read_in *)req->arg;
	static uint8_t buf[UFUSE_MAX_WRITE];
	size_t size = in->size < sizeof(buf) ? in->size : sizeof(buf);
	size_t used = 0;
	uint64_t index = in->offset;
	char name[16];
	uint64_t child;
	uint32_t type;

	// An entry's offset is the index of the one after it, so a READDIR picks up where the last stopped
	while (dir_entry(req->in->nodeid, index, name, sizeof(name), &child, &type)) {
		size_t took = ufuse_dirent_add(buf, used, size, name, child, index + 1, type);

		if (!took) {
			break;
		}
		used += took;
		index++;
	}
	ufuse_reply(s, req->in->unique, buf, used);
}

static void
serve(struct ufuse_session *s, const struct ufuse_req *req)
{
	uint64_t unique = req->in->unique;
	uint64_t ino = req->in->nodeid;

	switch (req->in->opcode) {
	case FUSE_LOOKUP: {
		struct fuse_entry_out out;
		uint64_t child = lookup(ino, (const char *)req->arg);

		memset(&out, 0, sizeof(out));
		if (!child) {
			ufuse_reply_err(s, unique, ENOENT);
			return;
		}
		out.nodeid = child;
		out.entry_valid = 1;
		out.attr_valid = 1;
		attr_of(child, &out.attr);
		ufuse_reply(s, unique, &out, sizeof(out));
		return;
	}
	case FUSE_GETATTR: {
		struct fuse_attr_out out;

		memset(&out, 0, sizeof(out));
		out.attr_valid = 1;
		if (attr_of(ino, &out.attr)) {
			ufuse_reply_err(s, unique, ENOENT);
			return;
		}
		ufuse_reply(s, unique, &out, sizeof(out));
		return;
	}
	case FUSE_OPEN:
	case FUSE_OPENDIR: {
		const struct fuse_open_in *in = (const struct fuse_open_in *)req->arg;
		struct fuse_open_out out;

		if ((in->flags & O_ACCMODE) != O_RDONLY) {
			ufuse_reply_err(s, unique, EROFS);
			return;
		}
		memset(&out, 0, sizeof(out));
		ufuse_reply(s, unique, &out, sizeof(out));
		return;
	}
	case FUSE_READ:
		serve_read(s, req);
		return;
	case FUSE_READDIR:
		serve_readdir(s, req);
		return;
	case FUSE_STATFS: {
		struct fuse_statfs_out out;

		memset(&out, 0, sizeof(out));
		out.st.bsize = 4096;
		out.st.frsize = 4096;
		out.st.namelen = 255;
		out.st.files = NFILES + 4;
		ufuse_reply(s, unique, &out, sizeof(out));
		return;
	}
	case FUSE_RELEASE:
	case FUSE_RELEASEDIR:
	case FUSE_FLUSH:
		ufuse_reply(s, unique, NULL, 0);
		return;
	case FUSE_FORGET:
	case FUSE_BATCH_FORGET:
		// The kernel expects no reply to a forget
		return;
	case FUSE_SETATTR:
	case FUSE_MKNOD:
	case FUSE_MKDIR:
	case FUSE_UNLINK:
	case FUSE_RMDIR:
	case FUSE_RENAME:
	case FUSE_CREATE:
	case FUSE_WRITE:
		ufuse_reply_err(s, unique, EROFS);
		return;
	default:
		ufuse_reply_err(s, unique, ENOSYS);
		return;
	}
}

static int
dump_big(const char *path)
{
	FILE *f = fopen(path, "wb");

	if (!f) {
		perror("hellofs");
		return 1;
	}
	for (uint64_t off = 0; off < BIG_NBYTES; off++) {
		fputc(big_byte(off), f);
	}
	return fclose(f) ? 1 : 0;
}

int
main(int argc, char **argv)
{
	static uint8_t buf[UFUSE_BUF_NBYTES];
	struct ufuse_session s;
	struct ufuse_req req;
	struct sigaction sa;
	int err;

	if (argc == 3 && !strcmp(argv[1], "--big")) {
		return dump_big(argv[2]);
	}
	if (argc != 2) {
		fprintf(stderr, "usage: hellofs MOUNTPOINT | hellofs --big FILE\n");
		return 64;
	}

	// No SA_RESTART, so a signal interrupts the blocking read and the loop sees 'stopping'
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	ufuse_session_init(&s);
	ufuse_unmount(&s, argv[1]);
	err = ufuse_mount(&s, argv[1], "hellofs", "hellofs", MS_RDONLY, NULL);
	if (err) {
		fprintf(stderr, "hellofs: mount: %s\n", strerror(-err));
		return 1;
	}

	err = (int)ufuse_read(&s, buf, sizeof(buf), &req);
	if (err < 0 || ufuse_init_reply(&s, &req, FUSE_ASYNC_READ | FUSE_BIG_WRITES)) {
		fprintf(stderr, "hellofs: INIT failed\n");
		ufuse_unmount(&s, argv[1]);
		return 1;
	}
	fprintf(stderr, "hellofs: serving %s, protocol 7.%u\n", argv[1], s.minor);

	while (!stopping) {
		ssize_t n = ufuse_read(&s, buf, sizeof(buf), &req);

		if (n == -EINTR || n == -EAGAIN || n == -ENOENT) {
			continue;
		}
		if (n == -ENODEV) {
			break;
		}
		if (n < 0) {
			fprintf(stderr, "hellofs: read: %s\n", strerror((int)-n));
			break;
		}
		if (req.in->opcode == FUSE_DESTROY) {
			ufuse_reply(&s, req.in->unique, NULL, 0);
			break;
		}
		serve(&s, &req);
	}
	ufuse_unmount(&s, argv[1]);
	return 0;
}
