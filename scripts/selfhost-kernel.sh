#!/bin/sh
# Run inside the NetBSD guest: build the GENERIC.MP kernel natively from
# the source disk (ld1) with parallel make, then send it to the host.
# usage: selfhost-kernel.sh JOBS [HOST_PORT]
J=${1:-8}; PORT=${2:-}
set -e
mkdir -p /usr/src
mount -t ffs /dev/ld1c /usr/src
cd /usr/src/sys/arch/virt68k/conf
config GENERIC.MP
cd ../compile/GENERIC.MP
echo "building with -j$J on $(sysctl -n hw.ncpu) CPUs, $(sysctl -n hw.physmem) bytes"
date
/usr/bin/time make -j$J > ../build.log 2>&1 || { tail -n 30 ../build.log; exit 1; }
date
tail -n 3 ../build.log
ls -l netbsd
cksum netbsd
size netbsd
if [ -n "$PORT" ]; then
	timeout 60 dhcpcd -1 -w vioif0 > /dev/null 2>&1 || true
	nc -N 10.0.2.2 $PORT < netbsd && echo "sent to host"
fi
