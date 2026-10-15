#!/usr/bin/env python3
"""Observe a live macOS process's exit without adopting, tracing, or stopping it.

Research helper for cooperative slurp. Must register before the process exits.
Requires macOS 11+; permission errors fail closed. This does not modify Claude's
internal task state or replace its reported status.
"""
import argparse
import json
import os
import select
import time

# Darwin sys/event.h; Python does not expose this constant on all versions.
NOTE_EXITSTATUS = 0x04000000


class ExitWatch:
    def __init__(self, pid):
        if pid <= 1:
            raise ValueError('expected a live job PID greater than 1')
        self.pid = pid
        self.queue = select.kqueue()
        try:
            self.queue.control([select.kevent(pid, filter=select.KQ_FILTER_PROC,
                flags=select.KQ_EV_ADD | select.KQ_EV_ONESHOT,
                fflags=select.KQ_NOTE_EXIT | NOTE_EXITSTATUS)], 0, 0)
        except BaseException:
            self.queue.close()
            raise

    def wait(self, timeout=60):
        deadline = time.monotonic() + timeout
        while True:
            remaining = max(0, deadline-time.monotonic())
            events = self.queue.control(None, 1, remaining)
            if not events:
                return None
            event = events[0]
            if event.flags & select.KQ_EV_ERROR:
                raise OSError(event.data, os.strerror(event.data))
            if event.ident != self.pid or not event.fflags & select.KQ_NOTE_EXIT:
                raise RuntimeError('unexpected process event')
            if not event.fflags & NOTE_EXITSTATUS:
                raise RuntimeError('kernel did not provide an exit status')
            status = event.data & 0xffffffff
            if os.WIFEXITED(status):
                return {'pid': self.pid, 'kind': 'exit', 'exitCode': os.WEXITSTATUS(status), 'waitStatus': status}
            if os.WIFSIGNALED(status):
                return {'pid': self.pid, 'kind': 'signal', 'signal': os.WTERMSIG(status), 'waitStatus': status}
            raise RuntimeError('unrecognized exit status')

    def close(self):
        self.queue.close()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pid', type=int)
    parser.add_argument('--timeout', type=float, default=60)
    args = parser.parse_args()
    with ExitWatch(args.pid) as watch:
        print(json.dumps({'pid': args.pid, 'watching': True}), flush=True)
        result = watch.wait(args.timeout)
        print(json.dumps(result or {'pid': args.pid, 'timedOut': True}), flush=True)
        return 0 if result else 2


if __name__ == '__main__':
    raise SystemExit(main())
