#!/usr/bin/env python3
"""Type commands on the guest's serial console (works even when SSH doesn't).

    scripts/console.py 'dmesg | tail'      run a command, print its output
Logs in as root first if a login prompt is showing.
"""
import socket, sys, time, pathlib

SOCK = pathlib.Path(__file__).resolve().parent.parent / 'run/serial.sock'
MARK = '__SVMD_DONE__'

def talk(s, data, wait_for, timeout=20):
    s.sendall(data.encode())
    buf, end = '', time.time() + timeout
    while time.time() < end and wait_for not in buf:
        try:
            buf += s.recv(65536).decode(errors='replace')
        except socket.timeout:
            pass
    return buf

s = socket.socket(socket.AF_UNIX)
s.connect(str(SOCK))
s.settimeout(0.5)
if 'login:' in talk(s, '\n', 'login:', 3):
    talk(s, 'root\n', '#', 10)
# The console echoes what we type, so build the marker at run time ("" splits
# it): only the command's real output contains it whole.
out = talk(s, f'{sys.argv[1]}; echo __SVMD_""DONE__\n', MARK, 60)
body = out.split('\n', 1)[1] if '\n' in out else out
print(body.split(MARK)[0].rstrip())
