<!--
SPDX-License-Identifier: BSD-3-Clause
Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>
-->

# ufuse: the FUSE and CUSE wire protocol, header-only

A filesystem or a character device in user space talks to the Linux kernel over one file descriptor:
`/dev/fuse` for a filesystem, `/dev/cuse` for a character device. The kernel writes requests to it and reads
replies back, and every message is a struct of the kernel's `<linux/fuse.h>`. ufuse is that conversation in a
single C header, `include/ufuse.h`, usable from C and C++, with no dependency beyond the kernel's UAPI
headers.

It does the parts every server repeats and nothing else: opening and mounting a session, the `INIT` and
`CUSE_INIT` handshakes, reading a request, writing a reply, packing directory entries. What to answer is the
caller's. There are no callbacks, threads or allocations; a session is a file descriptor to poll beside the
caller's own.

## Design goals

- **Header-only**, a single self-contained header
- **Zero-dependency**, beyond the Linux UAPI headers
- **C and C++**, gnu11 and C++17
- **Minimal**: the protocol, not a filesystem framework
- **Loop-agnostic**: the caller reads, dispatches and replies in its own loop

## Usage

```c
#include <ufuse.h>

static uint8_t buf[UFUSE_BUF_NBYTES];
struct ufuse_session s;
struct ufuse_req req;

ufuse_session_init(&s);
ufuse_mount(&s, "/mnt/view", "pool0", "aisiofs", MS_RDONLY, NULL);
ufuse_read(&s, buf, sizeof(buf), &req);
ufuse_init_reply(&s, &req, FUSE_ASYNC_READ);

for (;;) {
        ssize_t n = ufuse_read(&s, buf, sizeof(buf), &req);

        if (n == -EINTR || n == -EAGAIN || n == -ENOENT)
                continue;
        if (n < 0)
                break;                     /* -ENODEV: unmounted */
        switch (req.in->opcode) {
        case FUSE_GETATTR:
                /* fill a struct fuse_attr_out, then */
                ufuse_reply(&s, req.in->unique, &out, sizeof(out));
                break;
        default:
                ufuse_reply_err(&s, req.in->unique, ENOSYS);
        }
}
ufuse_unmount(&s, "/mnt/view");
```

`example/hellofs.c` is a complete read-only filesystem and `example/cuseecho.c` a character device. Mounting
is a `mount(2)` call and needs `CAP_SYS_ADMIN`; there is no `fusermount` fallback yet.

## Building and testing

```
meson setup build
meson compile -C build
meson test -C build                 # the unit test, as C and as C++
sudo meson test -C build            # also mounts hellofs and creates a cuseecho device
```

The tests that need root skip without it.

## Related work

The protocol is public and versioned, and the kernel's `<linux/fuse.h>` and the `fuse(4)` manual page
[[fuse4]] define it; a walk through the messages is in [[wire]]. Servers reach it in one of three ways.

**Through libfuse.** libfuse [[libfuse]] is the reference implementation. Its low-level API, and
`cuse_lowlevel` for character devices, are the closest equivalent of ufuse; a caller can even receive and
process requests in its own loop. It remains a shared library to build and ship, with its own session and
buffer management, mounting directly as root and through the setuid `fusermount3` helper otherwise, and it is
licensed LGPLv2 [[cuse_ll]]. NTFS-3G bundles a trimmed copy, fuse-lite, linked statically to avoid the
dependency [[fuselite]]; it is still libfuse's structure, made smaller.

**Re-implementing the protocol in another language.** bazil.org/fuse [[bazil]] implements the protocol from
scratch in Go, and go-fuse [[gofuse]] offers a raw interface over the wire protocol, which it can also serve
over transports other than the kernel's. In Rust, fuser [[fuser]] replaces libfuse and on Linux uses it only
optionally, for mounting and unmounting, and fuse-backend-rs [[fusebackend]] separates the ABI, the API and
the transport, so the same server answers `/dev/fuse` or virtio-fs. These show that a server needs little
beyond the kernel's structs, but none of them is reachable from C or C++.

**Speaking it directly, inside a larger project.** xNVMe's control-plane work [[xnvme]] talks to `/dev/cuse`
with `<linux/fuse.h>` and no libfuse, to mimic the kernel NVMe driver's ioctl interface for controllers it
drives from user space. The protocol handling there is internal and interleaved with the NVMe ioctl handling.
ufuse is that protocol layer on its own, for filesystems as well as character devices, so xNVMe's mimic and a
read-only management view such as aisiofs's can share it.

**Not yet covered.** Since Linux 6.14 a server may receive requests through io_uring instead of `read(2)`,
with a queue per CPU [[uring]]. It still needs `/dev/fuse` for setup, notifications and interrupts, so it
extends the session here rather than replacing it; ufuse does not do it yet.

[fuse4]: https://man7.org/linux/man-pages/man4/fuse.4.html "fuse(4), Linux manual page"
[wire]: https://nuetzlich.net/the-fuse-wire-protocol/ "The FUSE wire protocol"
[libfuse]: https://github.com/libfuse/libfuse "libfuse"
[cuse_ll]: https://android.googlesource.com/platform/external/libfuse/+/refs/heads/android12L-release/include/cuse_lowlevel.h "libfuse's cuse_lowlevel.h, distributed under the LGPLv2"
[fuselite]: https://git.freifunk-franken.de/mirror/openwrt-packages/src/branch/master/utils/ntfs-3g/Makefile "NTFS-3G's bundled minimal FUSE library, as packaged by OpenWrt"
[bazil]: https://github.com/bazil/fuse "bazil.org/fuse"
[gofuse]: https://github.com/hanwen/go-fuse "go-fuse"
[fuser]: https://docs.rs/crate/fuser/latest "fuser"
[fusebackend]: https://docs.rs/crate/fuse-backend-rs/latest "fuse-backend-rs"
[xnvme]: https://github.com/safl/xNVMe/tree/homi_series "xNVMe, the homi_series branch, lib/cuse"
[uring]: https://docs.kernel.org/6.14/filesystems/fuse-io-uring.html "FUSE-over-io-uring design documentation"

## License

BSD-3-Clause; see `LICENSE`.
