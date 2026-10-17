#!/bin/sh
# Boot a kernel, wait, then dump PC/SR/A7 of every vCPU (symbolized) and
# the console tail; quit QEMU through the monitor.
# usage: hang-dump.sh KERNEL SMP WAIT_SECONDS [IMAGE]
TOP=$(cd "$(dirname "$0")/.." && pwd)
K=$1; SMP=$2; WAIT=$3; IMG=${4:-$TOP/images/scratch-boot.img}
S=${TMPDIR:-/tmp}/hang-dump.$$
MON=${TMPDIR:-/tmp}/m$$		# AF_UNIX paths are limited to 108 bytes
mkdir -p "$S"
"$TOP/work/qemu/build/qemu-system-m68k" -M virt -m 512M -smp "$SMP" \
    -accel tcg,thread=multi -display none \
    -monitor unix:$MON,server,nowait -serial file:$S/console \
    -kernel "$K" -append root=ld0a \
    -drive file="$IMG",format=raw,if=none,id=hd0 \
    -device virtio-blk-device,drive=hd0 $QEMU_EXTRA &
QPID=$!
sleep "$WAIT"
python3 - "$MON" "$K" "$TOP/work/tools/bin/m68k--netbsdelf-addr2line" <<'PY'
import socket, sys, time, re, subprocess
mon, kern, a2l = sys.argv[1:4]
s = socket.socket(socket.AF_UNIX); s.connect(mon); time.sleep(0.3); s.recv(65536)
def cmd(c):
    s.send((c + "\n").encode()); time.sleep(1.0)
    out = b""
    s.settimeout(0.5)
    try:
        while True:
            d = s.recv(1 << 20)
            if not d: break
            out += d
    except socket.timeout:
        pass
    return out.decode(errors="replace")
regs = cmd("info registers -a")
for blk in re.split(r"(?=CPU#\d+)", regs):
    m = re.match(r"CPU#(\d+)", blk)
    if not m: continue
    pc = re.search(r"PC = ([0-9a-f]+)", blk).group(1)
    sr = re.search(r"SR = ([0-9a-f]+)", blk).group(1)
    a7 = re.search(r"A7 = ([0-9a-f]+)", blk).group(1)
    fn = subprocess.run([a2l, "-f", "-e", kern, "0x" + pc], capture_output=True, text=True).stdout.split("\n")[0]
    print(f"cpu{m.group(1)}: pc {pc} sr {sr} sp {a7}  {fn}")
s.send(b"quit\n")
PY
wait $QPID 2>/dev/null
echo "--- console tail"
tr -d '\r' < "$S/console" | tail -n "${TAIL:-8}"
rm -rf "$S" "$MON"
