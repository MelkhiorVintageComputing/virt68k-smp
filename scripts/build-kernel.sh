#!/bin/sh
# Build a NetBSD/virt68k kernel with the existing tools.
# usage: build-kernel.sh [CONF]     (default GENERIC.MP)
# The kernel ends up in work/obj/sys/arch/virt68k/compile/CONF/netbsd.
set -e
CONF=${1:-GENERIC.MP}
TOP=$(cd "$(dirname "$0")/.." && pwd)
W=$TOP/work
cd "$W/netbsd-src"
./build.sh -U -u -o -m virt68k -j8 -O "$W/obj" -T "$W/tools" -D "$W/destdir" \
    kernel="$CONF" > "$W/build-kernel-$CONF.log" 2>&1 || {
	grep -A5 -E "(error|warning):" "$W/build-kernel-$CONF.log" | head -40; exit 1; }
ls -l "$W/obj/sys/arch/virt68k/compile/$CONF/netbsd"
