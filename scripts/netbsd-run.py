#!/usr/bin/env python3
"""Boot NetBSD/virt68k on QEMU, log in on the serial console, run commands.

usage: netbsd-run.py [options] [-c CMD]...

The console is QEMU's stdio serial port.  After "login:" the script logs
in as root (no password on the live image), runs each -c command, waits
for the shell prompt after each, then runs "shutdown -p now" and waits for
QEMU to exit.  The full console transcript is written to --log.  Exit
status: 0 if the guest powered off cleanly, 1 otherwise.
"""
import argparse
import os
import re
import select
import subprocess
import sys
import time

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROMPT = re.compile(rb"\n[^\n]*# $")


class Console:
    def __init__(self, cmd, log, echo):
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT)
        self.buf = b""
        self.log = open(log, "wb")
        self.echo = echo

    def expect(self, pattern, timeout):
        rx = re.compile(pattern) if isinstance(pattern, bytes) else pattern
        end = time.time() + timeout
        while True:
            m = rx.search(self.buf)
            if m:
                out = self.buf[:m.end()]
                self.buf = self.buf[m.end():]
                return out
            left = end - time.time()
            if left <= 0:
                raise TimeoutError(pattern)
            r, _, _ = select.select([self.p.stdout], [], [], min(left, 1.0))
            if r:
                data = os.read(self.p.stdout.fileno(), 65536)
                if not data:
                    raise EOFError("QEMU exited")
                data = data.replace(b"\r", b"")
                self.log.write(data)
                self.log.flush()
                if self.echo:
                    sys.stdout.buffer.write(data)
                    sys.stdout.buffer.flush()
                self.buf += data

    def send(self, s):
        self.p.stdin.write(s.encode() + b"\n")
        self.p.stdin.flush()

    def wait_exit(self, timeout):
        end = time.time() + timeout
        while self.p.poll() is None and time.time() < end:
            try:
                self.expect(rb"\x00NEVER", min(5, end - time.time()))
            except (TimeoutError, EOFError):
                pass
        if self.p.poll() is None:
            self.p.kill()
            return None
        return self.p.returncode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=f"{TOP}/work/qemu/build/qemu-system-m68k")
    ap.add_argument("--kernel", default=f"{TOP}/images/netbsd-GENERIC")
    ap.add_argument("--image", default=f"{TOP}/images/netbsd-test.img")
    ap.add_argument("--smp", type=int, default=1)
    ap.add_argument("--thread", default="multi", choices=["single", "multi"])
    ap.add_argument("--mem", default="512M")  # >= ~1000M: see doc
    ap.add_argument("--append", default="root=ld0a")
    ap.add_argument("--log", default=f"{TOP}/images/netbsd-run.log")
    ap.add_argument("--boot-timeout", type=int, default=900)
    ap.add_argument("--cmd-timeout", type=int, default=3600)
    ap.add_argument("--echo", action="store_true")
    ap.add_argument("--disk", action="append", default=[],
                    help="extra raw disk image (ld1, ld2, ...)")
    ap.add_argument("--net", action="store_true",
                    help="add a virtio-net device on QEMU user networking")
    ap.add_argument("-c", "--command", action="append", default=[])
    ap.add_argument("--ddb", action="append", default=[],
                    help="ddb command to run if the kernel drops into ddb")
    ap.add_argument("--put", action="append", default=[],
                    help="LOCAL:REMOTE, copy a text file into the guest")
    a = ap.parse_args()

    qemu = [a.qemu, "-M", "virt", "-m", a.mem, "-smp", str(a.smp),
            "-accel", f"tcg,thread={a.thread}", "-display", "none",
            "-monitor", "none", "-serial", "stdio",
            "-kernel", a.kernel, "-append", a.append]
    # virtio-mmio slots are used from the top down and NetBSD numbers
    # the disks by ascending address: create the extra disks first so
    # that the root disk stays ld0.
    for i, d in enumerate(a.disk, 1):
        qemu += ["-drive", f"file={d},format=raw,if=none,id=hd{i}",
                 "-device", f"virtio-blk-device,drive=hd{i}"]
    qemu += ["-drive", f"file={a.image},format=raw,if=none,id=hd0",
             "-device", "virtio-blk-device,drive=hd0"]
    if a.net:
        qemu += ["-netdev", "user,id=n0",
                 "-device", "virtio-net-device,netdev=n0"]
    c = Console(qemu, a.log, a.echo)
    t0 = time.time()
    try:
        c.expect(rb"login: $", a.boot_timeout)
        print(f"login prompt after {time.time() - t0:.0f}s", flush=True)
        c.send("root")
        c.expect(PROMPT, 120)
        c.send("PS1='# '; export PS1")
        c.expect(PROMPT, 60)
        c.send("stty -echo")
        c.expect(PROMPT, 60)
        for spec in a.put:
            local, remote = spec.split(":", 1)
            # typed through the console: "cat" until ^D
            c.send(f"cat > {remote}")
            time.sleep(0.5)
            for line in open(local).read().splitlines():
                c.send(line)
                time.sleep(0.01)
            c.p.stdin.write(b"\x04")
            c.p.stdin.flush()
            c.expect(PROMPT, 60)
            print(f"put {local} -> {remote}", flush=True)
        for i, cmd in enumerate(a.command):
            t1 = time.time()
            # the marker is built by the shell, so its echo cannot match
            c.send(f"{cmd}; echo __DONE{i}_$?__ | tr -d _ | sed 's/^/@@/'")
            out = c.expect(re.compile(rb"\n@@DONE%d([0-9]+)\n|db\{\d+\}> $" % i),
                           a.cmd_timeout)
            if re.search(rb"db\{\d+\}> $", out):
                print("*** kernel entered ddb", flush=True)
                log = open(a.log, "rb").read().decode(errors="replace")
                areg = re.findall(r"areg: ((?:[0-9A-F]{8} ?){8})", log)
                a3 = areg[-1].split()[3] if areg else "0"
                for dc in a.ddb:
                    dc = dc.replace("{A3}", "0x" + a3)
                    c.p.stdin.write((dc + "\r").encode())	# ddb wants CR
                    c.p.stdin.flush()
                    o = c.expect(re.compile(rb"db\{\d+\}> $"), 120)
                    print(f"db> {dc}\n" + o.decode(errors="replace"), flush=True)
                c.p.kill()
                return 1
            lines = [l for l in out.decode(errors="replace").split("\n")[:-2]
                     if "__DONE" not in l and "export PS1" not in l
                     and not l.startswith("stty")
                     and l.strip() not in ("", "#")]
            status = re.search(rb"@@DONE%d([0-9]+)" % i, out).group(1)
            if status != b"0":
                lines.append(f"[exit status {status.decode()}]")
            print(f"$ {cmd}   [{time.time() - t1:.1f}s]")
            print("\n".join(lines), flush=True)
        c.send("shutdown -p now")
    except (TimeoutError, EOFError) as e:
        print(f"ERROR: {type(e).__name__}: {e} (see {a.log})", flush=True)
        c.p.kill()
        return 1
    rc = c.wait_exit(300)
    print(f"QEMU exit status {rc}, total {time.time() - t0:.0f}s", flush=True)
    return 0 if rc == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
