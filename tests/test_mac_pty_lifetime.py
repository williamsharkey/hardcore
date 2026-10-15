#!/usr/bin/env python3
"""Research PTY lifetime workarounds on disposable macOS sessions only.

Keeping a slave open, setting CLOCAL, and ignoring SIGHUP do not substitute
for retaining the master. The positive control duplicates the master after
the child starts; it assumes a cooperating owner, not access to Terminal.app.
"""
import ctypes
import errno
import fcntl
import json
import os
import pty
import select
import subprocess
import sys
import termios
import tty

CHILD = r'''
import fcntl,json,os,signal,sys,termios
os.setsid()
fcntl.ioctl(0,termios.TIOCSCTTY,0)
signal.signal(signal.SIGHUP,signal.SIG_IGN)
report=int(sys.argv[1])
os.write(report,b'ready\n')
result={}
try: result['read']=os.read(0,4).decode()
except OSError as e: result['readError']=e.errno
try: result['written']=os.write(1,b'ACK!')
except OSError as e: result['writeError']=e.errno
os.write(report,json.dumps(result).encode()+b'\n')
if 'written' in result:
    os.read(0,1)  # Keep the slave open until the observer receives the output.
'''


def read_line(fd):
    result = bytearray()
    while not result.endswith(b'\n'):
        assert select.select([fd], [], [], 5)[0], 'child report timed out'
        part = os.read(fd, 1)
        assert part, 'child closed report pipe'
        result.extend(part)
    return bytes(result)


def run_case(keep_slave=False, clocal=False, keep_master=False):
    master, slave = pty.openpty()
    read_fd, write_fd = os.pipe()
    duplicate = -1
    child = None
    try:
        tty.setraw(slave)
        if clocal:
            attrs = termios.tcgetattr(slave)
            attrs[2] |= termios.CLOCAL
            termios.tcsetattr(slave, termios.TCSANOW, attrs)
        child = subprocess.Popen([sys.executable, '-c', CHILD, str(write_fd)],
            stdin=slave, stdout=slave, stderr=slave, pass_fds=(write_fd,))
        os.close(write_fd); write_fd = -1
        assert read_line(read_fd) == b'ready\n'
        if not keep_slave:
            os.close(slave); slave = -1
        if keep_master:
            duplicate = os.dup(master)
        os.close(master); master = -1
        if keep_master:
            os.write(duplicate, b'PING')
        result = json.loads(read_line(read_fd))
        if keep_master:
            assert result == {'read': 'PING', 'written': 4}, result
            assert select.select([duplicate], [], [], 5)[0]
            assert os.read(duplicate, 4) == b'ACK!'
            os.write(duplicate, b'X')
        else:
            assert result == {'read': '', 'writeError': errno.EIO}, result
        child.wait(timeout=5)
        assert child.returncode == 0
        print('PASS', {'keepSlave': keep_slave, 'clocal': clocal,
                       'keepMaster': keep_master, **result})
    finally:
        if child and child.poll() is None:
            child.kill(); child.wait(timeout=5)
        for fd in (master, slave, duplicate, read_fd, write_fd):
            if fd >= 0:
                os.close(fd)


def check_reopen():
    libc = ctypes.CDLL(None, use_errno=True)
    libc.ptsname.argtypes = [ctypes.c_int]
    libc.ptsname.restype = ctypes.c_char_p
    master, slave = pty.openpty()
    reopened = -1
    try:
        original_name = libc.ptsname(master)
        reopened = os.open('/dev/ptmx', os.O_RDWR | os.O_NOCTTY)
        new_name = libc.ptsname(reopened)
        assert original_name and new_name and original_name != new_name
        print('PASS reopening /dev/ptmx allocates a different PTY')
    finally:
        for fd in (master, slave, reopened):
            if fd >= 0: os.close(fd)


if __name__ == '__main__':
    if sys.platform != 'darwin':
        raise SystemExit('This probe is specific to macOS')
    run_case()
    run_case(keep_slave=True)
    run_case(keep_slave=True, clocal=True)
    run_case(keep_master=True)
    check_reopen()
