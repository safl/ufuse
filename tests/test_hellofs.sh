#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>
#
# Mounts hellofs and reads it back through the kernel: names, sizes, contents at offsets, a directory that
# takes several READDIR replies, and refused writes. Needs root and /dev/fuse; skips (77) without them.
#
#   tests/test_hellofs.sh build/example/hellofs
set -eu

HELLOFS=$1
if [ "$(id -u)" != 0 ] || [ ! -c /dev/fuse ]; then
	echo "test_hellofs: skipped, needs root and /dev/fuse"
	exit 77
fi

mnt=$(mktemp -d)
work=$(mktemp -d)
pid=""
cleanup() {
	[ -z "$pid" ] || kill "$pid" 2>/dev/null || true
	[ -z "$pid" ] || wait "$pid" 2>/dev/null || true
	umount -l "$mnt" 2>/dev/null || true
	rmdir "$mnt" 2>/dev/null || true
	rm -rf "$work"
}
trap cleanup EXIT

fail() {
	echo "FAIL: $*" >&2
	cat "$work/log" >&2 || true
	exit 1
}

"$HELLOFS" "$mnt" 2>"$work/log" &
pid=$!
tries=0
until grep -q serving "$work/log" 2>/dev/null; do
	tries=$((tries + 1))
	[ "$tries" -le 50 ] || fail "hellofs did not come up"
	kill -0 "$pid" 2>/dev/null || fail "hellofs exited"
	sleep 0.1
done

[ "$(findmnt -n -o FSTYPE "$mnt")" = "fuse.hellofs" ] || fail "not mounted as fuse.hellofs"
[ "$(findmnt -n -o OPTIONS "$mnt" | cut -d, -f1)" = "ro" ] || fail "not mounted read-only"
[ "$(ls "$mnt" | tr '\n' ' ')" = "big hello many " ] || fail "root lists: $(ls "$mnt")"
[ "$(cat "$mnt/hello")" = "Hello from ufuse" ] || fail "hello holds: $(cat "$mnt/hello")"
[ "$(stat -c %s "$mnt/big")" = 1048576 ] || fail "big is $(stat -c %s "$mnt/big") bytes"

"$HELLOFS" --big "$work/big"
cmp "$mnt/big" "$work/big" || fail "big differs from its pattern"
dd if="$mnt/big" of="$work/got" bs=1 skip=123457 count=5000 2>/dev/null
dd if="$work/big" of="$work/want" bs=1 skip=123457 count=5000 2>/dev/null
cmp "$work/got" "$work/want" || fail "a read at an unaligned offset differs"

# A thousand names take several READDIR replies, each continuing at the offset the last one ended on
[ "$(ls "$mnt/many" | wc -l)" = 1000 ] || fail "many lists $(ls "$mnt/many" | wc -l) entries"
[ "$(ls "$mnt/many" | head -1)" = f0000 ] && [ "$(ls "$mnt/many" | tail -1)" = f0999 ] || fail "many's first or last name"
[ "$(cat "$mnt/many/f0123")" = f0123 ] || fail "f0123 holds: $(cat "$mnt/many/f0123")"
[ ! -e "$mnt/nope" ] || fail "a name that does not exist was found"

if touch "$mnt/new" 2>/dev/null; then fail "creating a file succeeded"; fi
if sh -c "echo x > '$mnt/hello'" 2>/dev/null; then fail "writing a file succeeded"; fi

kill -TERM "$pid"
wait "$pid" || fail "hellofs exited with an error"
pid=""
if findmnt "$mnt" >/dev/null; then fail "still mounted after hellofs stopped"; fi

echo "test_hellofs: ok"
