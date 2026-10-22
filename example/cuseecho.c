// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * cuseecho: a character device served with ufuse that reads back what was last written to it
 *
 * Usage: cuseecho NAME       create /dev/NAME, serve until signalled
 *
 * Needs the cuse module and access to /dev/cuse, root by default.
 */
#include <signal.h>

#include <ufuse.h>

static volatile sig_atomic_t stopping;
static uint8_t content[4096];
static size_t content_nbytes;

// A character device has no file position: CUSE passes every read with offset 0. Each open's position is kept
// here instead, under the handle OPEN returned, so a reader reaches the end of the content and sees EOF.
#define NOPENS 64
static uint64_t position[NOPENS];

static void
on_signal(int sig)
{
	(void)sig;
	stopping = 1;
}

static void
dev_open(const struct ufuse_req *req, uint64_t ino, const struct fuse_open_in *in)
{
	struct fuse_open_out out;
	static uint64_t next;

	(void)ino;
	(void)in;
	memset(&out, 0, sizeof(out));
	out.fh = next++ % NOPENS;
	position[out.fh] = 0;
	ufuse_req_reply(req, &out, sizeof(out));
}

static void
dev_read(const struct ufuse_req *req, uint64_t ino, const struct fuse_read_in *in)
{
	uint64_t *pos = &position[in->fh % NOPENS];
	size_t n = *pos >= content_nbytes ? 0 : content_nbytes - (size_t)*pos;

	(void)ino;
	if (n > in->size) {
		n = in->size;
	}
	ufuse_req_reply(req, content + *pos, n);
	*pos += n;
}

static void
dev_write(const struct ufuse_req *req, uint64_t ino, const struct fuse_write_in *in, const void *data, size_t n)
{
	struct fuse_write_out out;

	(void)ino;
	(void)in;
	// Each write replaces the content, the way a shell's "echo x > dev" means it
	if (n > sizeof(content)) {
		ufuse_req_reply_err(req, EFBIG);
		return;
	}
	memcpy(content, data, n);
	content_nbytes = n;
	memset(&out, 0, sizeof(out));
	out.size = (uint32_t)n;
	ufuse_req_reply(req, &out, sizeof(out));
}

static void
dev_release(const struct ufuse_req *req, uint64_t ino, const struct fuse_release_in *in)
{
	(void)ino;
	(void)in;
	ufuse_req_reply(req, NULL, 0);
}

static void
dev_flush(const struct ufuse_req *req, uint64_t ino, const struct fuse_flush_in *in)
{
	(void)ino;
	(void)in;
	ufuse_req_reply(req, NULL, 0);
}

static const struct ufuse_ops ops = {
	.open = dev_open,
	.read = dev_read,
	.write = dev_write,
	.release = dev_release,
	.flush = dev_flush,
};

int
main(int argc, char **argv)
{
	static uint8_t buf[UFUSE_BUF_NBYTES];
	struct ufuse_session s;
	struct ufuse_req req;
	struct sigaction sa;
	int err;

	if (argc != 2) {
		fprintf(stderr, "usage: cuseecho NAME\n");
		return 64;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	ufuse_session_init(&s);
	err = ufuse_cuse_open(&s);
	if (err) {
		fprintf(stderr, "cuseecho: open /dev/cuse: %s\n", strerror(-err));
		return 1;
	}
	err = (int)ufuse_read(&s, buf, sizeof(buf), &req);
	if (err < 0 || ufuse_cuse_init_reply(&s, &req, argv[1], 0, 0, 0)) {
		fprintf(stderr, "cuseecho: CUSE_INIT failed\n");
		ufuse_cuse_close(&s);
		return 1;
	}
	fprintf(stderr, "cuseecho: serving /dev/%s, protocol 7.%u\n", argv[1], s.minor);

	while (!stopping) {
		ssize_t n = ufuse_read(&s, buf, sizeof(buf), &req);

		if (n == -EINTR || n == -EAGAIN || n == -ENOENT) {
			continue;
		}
		if (n < 0) {
			break;
		}
		ufuse_dispatch(&s, &ops, NULL, buf, (size_t)n);
	}
	ufuse_cuse_close(&s);
	return 0;
}
