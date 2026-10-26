#!/usr/bin/env python3
"""Scan the 68040 page tables of a running QEMU m68k guest (4K pages)
through the QEMU monitor (UNIX socket) and count, among the resident page
descriptors, those with U0 (bit 8), U1 (bit 9) and UR (bit 11) set.

usage: pte-scan.py MONITOR_SOCKET [-u] [-v]
  -u  scan the user tree (URP) instead of the supervisor tree (SRP)
  -v  list the VAs of the pages with U0/U1/UR set (ranges)

The guest must use the 68040 MMU with 4K pages (TCR = 0x8000), as
NetBSD/virt68k does.  The page tables are read with "xp", one 4K page of
physical memory at a time (cached).
"""
import re
import socket
import sys

L1N, L2N, L3N = 128, 128, 64
PAGE = 4096


class Monitor:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(path)
        self.buf = b""
        self.read_prompt()

    def read_prompt(self):
        while b"(qemu) " not in self.buf:
            d = self.s.recv(1 << 16)
            if not d:
                raise EOFError("monitor closed")
            self.buf += d
        out, _, self.buf = self.buf.partition(b"(qemu) ")
        return out.decode(errors="replace")

    def cmd(self, c):
        self.s.sendall((c + "\n").encode())
        out = self.read_prompt()
        # drop the echo of the command
        return out.split("\n", 1)[1] if "\n" in out else out


class Phys:
    def __init__(self, mon):
        self.mon = mon
        self.pages = {}

    def word(self, pa):
        base = pa & ~(PAGE - 1)
        if base not in self.pages:
            out = self.mon.cmd(f"xp /{PAGE // 4}wx {base:#x}")
            # "ADDRESS: 0xWORD 0xWORD ...": the words are after the colon
            vals = []
            for line in out.splitlines():
                if ":" in line:
                    vals += [int(p, 16) for p in
                             re.findall(r"0x([0-9a-f]+)", line.split(":", 1)[1])]
            assert len(vals) >= PAGE // 4, (hex(base), len(vals))
            self.pages[base] = vals
        return self.pages[base][(pa - base) // 4]


def scan(mon, user, verbose):
    reg = "urp" if user else "srp"
    root = int(mon.cmd(f"p ${reg}").strip().split()[-1], 0)
    mem = Phys(mon)
    counts = {"resident": 0, "U0": 0, "U1": 0, "UR": 0, "indirect": 0}
    marked = []		# (va, flags) of pages with U0/U1/UR
    for i in range(L1N):
        d1 = mem.word(root + 4 * i)
        if not d1 & 2:
            continue
        l2 = d1 & ~0x1ff
        for j in range(L2N):
            d2 = mem.word(l2 + 4 * j)
            if not d2 & 2:
                continue
            l3 = d2 & ~0xff
            for k in range(L3N):
                pa = l3 + 4 * k
                d3 = mem.word(pa)
                pdt = d3 & 3
                if pdt == 0:
                    continue
                if pdt == 2:		# indirect: follow once
                    counts["indirect"] += 1
                    d3 = mem.word(d3 & ~3)
                    if d3 & 3 in (0, 2):
                        continue
                va = (i << 25) | (j << 18) | (k << 12)
                counts["resident"] += 1
                f = ""
                if d3 & 0x100:
                    counts["U0"] += 1
                    f += "0"
                if d3 & 0x200:
                    counts["U1"] += 1
                    f += "1"
                if d3 & 0x800:
                    counts["UR"] += 1
                    f += "R"
                if f:
                    marked.append((va, f))
    print(f"{reg.upper()} {root:#x}: " +
          " ".join(f"{k}={v}" for k, v in counts.items()))
    if verbose:
        rng = []
        for va, f in marked:
            if rng and rng[-1][2] == f and rng[-1][1] + PAGE == va:
                rng[-1][1] = va
            else:
                rng.append([va, va, f])
        for a, b, f in rng:
            print(f"  {a:#010x}-{b + PAGE - 1:#010x} {(b - a) // PAGE + 1:6d} "
                  f"pages  {'U0 ' if '0' in f else ''}{'U1 ' if '1' in f else ''}"
                  f"{'UR' if 'R' in f else ''}")
    return counts


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    if len(args) != 1:
        sys.exit(__doc__)
    mon = Monitor(args[0])
    scan(mon, "-u" in sys.argv, "-v" in sys.argv)


if __name__ == "__main__":
    main()
