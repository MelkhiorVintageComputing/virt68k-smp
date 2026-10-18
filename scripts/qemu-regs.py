#!/usr/bin/env python3
"""Dump PC/SR/SP of every vCPU of a running QEMU (monitor on a UNIX socket),
symbolized with the kernel, and the kernel text addresses found in the top
of each stack (a rough backtrace).
usage: qemu-regs.py MONITOR_SOCKET KERNEL [STACK_WORDS]"""
import re, socket, subprocess, sys, time, os

mon, kern = sys.argv[1], sys.argv[2]
nwords = int(sys.argv[3]) if len(sys.argv) > 3 else 96
TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
A2L = f"{TOP}/work/tools/bin/m68k--netbsdelf-addr2line"
NM = f"{TOP}/work/tools/bin/m68k--netbsdelf-nm"

syms = sorted((int(a, 16), n) for a, t, n in
              (l.split()[:3] for l in subprocess.run([NM, "-n", kern],
               capture_output=True, text=True).stdout.splitlines()
               if len(l.split()) >= 3) if t in "tT")
lo, hi = syms[0][0], syms[-1][0]

def sym(a):
    import bisect
    i = bisect.bisect_right([x[0] for x in syms], a) - 1
    return f"{syms[i][1]}+{a - syms[i][0]:#x}" if i >= 0 else hex(a)

s = socket.socket(socket.AF_UNIX); s.connect(mon); time.sleep(0.3); s.recv(65536)
def cmd(c, wait=0.7):
    s.send((c + "\n").encode()); time.sleep(wait)
    out = b""; s.settimeout(0.5)
    try:
        while True:
            d = s.recv(1 << 20)
            if not d: break
            out += d
    except socket.timeout:
        pass
    return out.decode(errors="replace")

regs = cmd("info registers -a", 1.5)
for blk in re.split(r"(?=CPU#\d+)", regs):
    m = re.match(r"CPU#(\d+)", blk)
    if not m: continue
    n = int(m.group(1))
    pc = int(re.search(r"PC = ([0-9a-f]+)", blk).group(1), 16)
    sr = re.search(r"SR = ([0-9a-f]+)", blk).group(1)
    a7 = int(re.search(r"A7 = ([0-9a-f]+)", blk).group(1), 16)
    print(f"cpu{n}: pc {pc:08x} sr {sr} sp {a7:08x}  {sym(pc)}")
    cmd(f"cpu {n}")
    mem = cmd(f"x /{nwords}wx {a7:#x}", 1.0)
    ra = [int(w, 16) for w in re.findall(r"0x([0-9a-f]{8})", mem)]
    print("   stack:", " ".join(sym(w) for w in ra if lo <= w <= hi)[:900])
s.close()
