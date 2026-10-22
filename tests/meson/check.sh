#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>
#
# Checks meson.build the two ways a Meson project takes ufuse: installed, found
# through ufuse.pc, and fetched by a wrap-git of this checkout's HEAD. Each
# builds example/hellofs.c as a consumer; the wrapped build must not build
# ufuse's own examples and tests. Run from the repository's top.
set -eu

top=$PWD
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

consumer() {
	mkdir -p "$tmp/$1"
	cp tests/meson/meson.build example/hellofs.c "$tmp/$1/"
}

meson setup --prefix="$tmp/usr" "$tmp/b" > /dev/null
meson install -C "$tmp/b" > /dev/null
pc=$(find "$tmp/usr" -name ufuse.pc)
test -n "$pc"
test -f "$tmp/usr/include/ufuse.h"
if grep -q '^Libs:.*-l' "$pc"; then echo "ufuse.pc links a library" >&2; exit 1; fi
consumer installed
PKG_CONFIG_PATH=$(dirname "$pc") meson setup --wrap-mode=nofallback "$tmp/c1" "$tmp/installed" > /dev/null
ninja -C "$tmp/c1" > /dev/null
test -x "$tmp/c1/hellofs"

consumer wrapped
mkdir -p "$tmp/wrapped/subprojects"
cat > "$tmp/wrapped/subprojects/ufuse.wrap" << WRAP
[wrap-git]
url = $top
revision = $(git rev-parse HEAD)
depth = 1

[provide]
dependency_names = ufuse
WRAP
PKG_CONFIG_PATH= meson setup --wrap-mode=forcefallback "$tmp/c2" "$tmp/wrapped" > /dev/null
ninja -C "$tmp/c2" > /dev/null
test -x "$tmp/c2/hellofs"
if [ -n "$(find "$tmp/c2/subprojects" -name 'test_unit*' -o -name cuseecho)" ]; then
	echo "a wrapped ufuse built its own examples or tests" >&2
	exit 1
fi
echo "meson: installed and wrapped consumers built"
