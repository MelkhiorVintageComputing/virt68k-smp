#!/usr/bin/env python3
"""Receive one file over TCP (guest: nc -N 10.0.2.2 PORT < file).
usage: recv-file.py PORT OUTPUT"""
import socket, sys
port, out = int(sys.argv[1]), sys.argv[2]
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", port)); s.listen(1)
c, _ = s.accept()
n = 0
with open(out, "wb") as f:
    while True:
        d = c.recv(1 << 16)
        if not d:
            break
        f.write(d); n += len(d)
print(f"received {n} bytes into {out}")
