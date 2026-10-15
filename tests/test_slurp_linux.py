#!/usr/bin/env python3
"""Only synthetic PTYs/processes; never discovers or touches user terminals."""
import ctypes
import fcntl
import os
import pathlib
import select
import signal
import subprocess
import termios
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
HELPER = str(ROOT / "build/slurp-linux")
libc = ctypes.CDLL(None, use_errno=True)


def allow_tracer():
    # Permit this test's sibling helper without changing system ptrace policy.
    if libc.prctl(0x59616D61, ctypes.c_ulong(-1).value, 0, 0, 0):
        raise OSError(ctypes.get_errno(), "PR_SET_PTRACER")


def identity(pid):
    return pathlib.Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[19]


def read_until(fd, expected, seconds=8):
    data = b""
    until = time.monotonic() + seconds
    while expected not in data and time.monotonic() < until:
        if select.select([fd], [], [], 0.1)[0]:
            try:
                chunk = os.read(fd, 65536)
            except OSError:
                break
            if not chunk:
                break
            data += chunk
    assert expected in data, (expected, data)
    return data


def main():
    allow_tracer()
    old, slave = os.openpty()
    pid = os.fork()
    if pid == 0:
        os.close(old)
        os.setsid()
        fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
        for fd in (0, 1, 2):
            os.dup2(slave, fd)
        if slave > 2:
            os.close(slave)
        allow_tracer()
        os.environ.pop("HARDCORE_RC", None)
        os.environ.pop("TERM_PROGRAM", None)
        # A real shell with an in-memory variable and a background job.
        os.execv("/bin/bash", ["bash", "--norc", "--noprofile", "-i"])
    os.close(slave)
    relay = None
    new = dest = None
    try:
        os.write(old, b"export SLURP_MEMORY=retained; sleep 300 &\n")
        read_until(old, b"] ")
        jobs = [int(p) for p in pathlib.Path(f"/proc/{pid}/task/{pid}/children").read_text().split()]
        assert jobs, "background fixture job did not start"
        # A stopped job must stay stopped during transfer, then resume normally.
        os.kill(jobs[0], signal.SIGSTOP)
        until = time.monotonic() + 3
        while pathlib.Path(f"/proc/{jobs[0]}/stat").read_text().rsplit(")", 1)[1].split()[0] != 'T':
            assert time.monotonic() < until, "fixture job did not stop"
            time.sleep(.02)
        start = identity(pid)
        # Identity mismatch must leave the original shell and its input intact.
        bad = subprocess.run([HELPER, "--attach", str(pid), str(int(start) + 1), str(os.getuid())],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
        assert bad.returncode != 0
        os.write(old, b"printf 'ORIGINAL-%s\\n' alive\n")
        read_until(old, b"ORIGINAL-alive")
        new, dest = os.openpty()

        def new_terminal():
            os.setsid()
            fcntl.ioctl(0, termios.TIOCSCTTY, 0)

        relay = subprocess.Popen([HELPER, "--attach", str(pid), start, str(os.getuid())],
                                 stdin=dest, stdout=dest, stderr=dest, preexec_fn=new_terminal)
        os.close(dest)
        dest = None
        read_until(new, b"attached live WSL session")
        assert identity(pid) == start, "shell PID/start time changed"
        assert os.fstat(old).st_rdev == os.stat("/dev/null").st_rdev, "old master not released"
        for job in jobs:
            os.kill(job, 0)
            assert os.getsid(job) == pid, "background job moved out of the original session"
        assert pathlib.Path(f"/proc/{jobs[0]}/stat").read_text().rsplit(")", 1)[1].split()[0] == 'T', "stopped job was resumed unexpectedly"
        os.kill(jobs[0], signal.SIGCONT)
        duplicate, duplicate_slave = os.openpty()
        try:
            second = subprocess.Popen([HELPER, "--attach", str(pid), start, str(os.getuid())],
                                      stdin=duplicate_slave, stdout=duplicate_slave, stderr=duplicate_slave,
                                      preexec_fn=new_terminal)
            os.close(duplicate_slave)
            duplicate_slave = None
            read_until(duplicate, b"session already being attached")
            assert second.wait(timeout=5) != 0
        finally:
            os.close(duplicate)
            if duplicate_slave is not None:
                os.close(duplicate_slave)
        os.write(new, b"printf 'MEMORY-%s-PID-%s\\n' \"$SLURP_MEMORY\" \"$$\"; jobs -p\n")
        read_until(new, f"MEMORY-retained-PID-{pid}".encode())
        # Foreground Ctrl-C still works through the original PTY/job control.
        os.write(new, b"sleep 300\n")
        time.sleep(0.2)
        os.write(new, b"\x03")
        os.write(new, b"printf 'INTERRUPT-%s\\n' works\n")
        read_until(new, b"INTERRUPT-works")
        os.write(new, b"kill $(jobs -p); exit\n")
        relay.wait(timeout=8)
        assert relay.returncode == 0
        print("ok: PID, shell memory, background job stop/resume, input, Ctrl-C, old-master release, identity/duplicate rejection, EOF")
    finally:
        if relay and relay.poll() is None:
            relay.kill()
            relay.wait()
        # Only the synthetic session, including its jobs.
        for proc in pathlib.Path("/proc").iterdir():
            if not proc.name.isdigit():
                continue
            try:
                if os.getsid(int(proc.name)) == pid:
                    os.kill(int(proc.name), signal.SIGKILL)
            except ProcessLookupError:
                pass
        try:
            os.waitpid(pid, 0)
        except ChildProcessError:
            pass
        for fd in (old, new, dest):
            if fd is not None:
                os.close(fd)
        libc.prctl(0x59616D61, 0, 0, 0, 0)


if __name__ == "__main__":
    main()
