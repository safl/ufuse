#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>
#
# Creates a character device with cuseecho, writes to it and reads it back, and checks it goes away with
# the process. Needs root and the cuse module; skips (77) without them.
#
#   tests/test_cuseecho.sh build/example/cuseecho
set -eu

CUSEECHO=$1
[ -c /dev/cuse ] || modprobe cuse 2>/dev/null || true
if [ "$(id -u)" != 0 ] || [ ! -c /dev/cuse ]; then
	echo "test_cuseecho: skipped, needs root and /dev/cuse"
	exit 77
fi

name=ufuse-test-$$
work=$(mktemp -d)
pid=""
cleanup() {
	[ -z "$pid" ] || kill "$pid" 2>/dev/null || true
	[ -z "$pid" ] || wait "$pid" 2>/dev/null || true
	rm -rf "$work"
}
trap cleanup EXIT

fail() {
	echo "FAIL: $*" >&2
	cat "$work/log" >&2 || true
	exit 1
}

"$CUSEECHO" "$name" 2>"$work/log" &
pid=$!
tries=0
until [ -c "/dev/$name" ]; do
	tries=$((tries + 1))
	[ "$tries" -le 50 ] || fail "/dev/$name did not appear"
	kill -0 "$pid" 2>/dev/null || fail "cuseecho exited"
	sleep 0.1
done

[ -z "$(cat "/dev/$name")" ] || fail "a fresh device is not empty"
printf 'hello cuse' >"/dev/$name"
[ "$(cat "/dev/$name")" = "hello cuse" ] || fail "read back: $(cat "/dev/$name")"
printf 'again' >"/dev/$name"
[ "$(cat "/dev/$name")" = "again" ] || fail "a second write was not read back"

kill -TERM "$pid"
wait "$pid" || fail "cuseecho exited with an error"
pid=""
tries=0
while [ -e "/dev/$name" ]; do
	tries=$((tries + 1))
	[ "$tries" -le 50 ] || fail "/dev/$name stayed after cuseecho stopped"
	sleep 0.1
done

grep -h serving "$work/log" || true
echo "test_cuseecho: ok"
