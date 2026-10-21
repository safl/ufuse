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
serve(struct ufuse_session *s, const struct ufuse_req *req)
{
	uint64_t unique = req->in->unique;

	switch (req->in->opcode) {
	case FUSE_OPEN: {
		struct fuse_open_out out;
		static uint64_t next;

		memset(&out, 0, sizeof(out));
		out.fh = next++ % NOPENS;
		position[out.fh] = 0;
		ufuse_reply(s, unique, &out, sizeof(out));
		return;
	}
	case FUSE_READ: {
		const struct fuse_read_in *in = (const struct fuse_read_in *)req->arg;
		uint64_t *pos = &position[in->fh % NOPENS];
		size_t n = *pos >= content_nbytes ? 0 : content_nbytes - (size_t)*pos;

		if (n > in->size) {
			n = in->size;
		}
		ufuse_reply(s, unique, content + *pos, n);
		*pos += n;
		return;
	}
	case FUSE_WRITE: {
		const struct fuse_write_in *in = (const struct fuse_write_in *)req->arg;
		const uint8_t *data = (const uint8_t *)req->arg + sizeof(*in);
		struct fuse_write_out out;
		size_t n = in->size;

		// Each write replaces the content, the way a shell's "echo x > dev" means it
		if (n > sizeof(content) || sizeof(*in) + n > req->arg_nbytes) {
			ufuse_reply_err(s, unique, EFBIG);
			return;
		}
		memcpy(content, data, n);
		content_nbytes = n;
		memset(&out, 0, sizeof(out));
		out.size = (uint32_t)n;
		ufuse_reply(s, unique, &out, sizeof(out));
		return;
	}
	case FUSE_RELEASE:
	case FUSE_FLUSH:
		ufuse_reply(s, unique, NULL, 0);
		return;
	default:
		ufuse_reply_err(s, unique, ENOSYS);
		return;
	}
}

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
		serve(&s, &req);
	}
	ufuse_cuse_close(&s);
	return 0;
}
