// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

// What ufuse does without a kernel: session defaults, directory entry packing, attributes, opcode names.
// Written in the subset of C and C++ both accept; test_unit_cxx.cpp compiles it as C++.
#include <dirent.h>
#include <stdlib.h>

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
	CHECK(strcmp(ufuse_opcode_name(0xdead), "UNKNOWN") == 0);
}

int
main(void)
{
	test_session();
	test_dirent();
	test_attr();
	test_names();
	if (failures) {
		fprintf(stderr, "test_unit: %d failed\n", failures);
		return 1;
	}
	printf("test_unit: ok\n");
	return 0;
}
