#!/bin/sh
# Build work/qemu (m68k-softmmu only).  Configures on first use.
TOP=$(cd "$(dirname "$0")/.." && pwd)
export PATH=$TOP/work/tools-venv/bin:$PATH
B=$TOP/work/qemu/build
if [ ! -f "$B/build.ninja" ]; then
    mkdir -p "$B" && cd "$B" &&
    ../configure --target-list=m68k-softmmu --enable-debug-tcg \
        --disable-docs --disable-werror || exit 1
fi
ninja -C "$B" -j"${JOBS:-10}" qemu-system-m68k "$@"
