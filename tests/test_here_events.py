#!/usr/bin/env python3
"""Deterministic event batches and suspend/resume for the terminal frontend."""
import importlib.util
import os
from pathlib import Path
import selectors
import signal
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("hc_here_events", ROOT/"tools/here.py")
here = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = here
SPEC.loader.exec_module(here)


class Host:
    input_fd, output_fd = 0, 1

    def __init__(self):
        self.writes = []

    def enter(self):
        self.write(here.HostTerminal.ENTER)

    def restore(self):
        self.write(here.HostTerminal.LEAVE)

    def write(self, data):
        self.writes.append(data.encode() if isinstance(data, str) else data)


class Child:
    def __init__(self, fd):
        self.fd = fd
        self.pending = bytearray()
        self.terminal = here.Terminal(80, 23)
        self.scroll, self.exited, self.label = 0, None, "owned fake"
        self.reads = self.closes = 0

    def queue(self, data):
        self.pending.extend(data)

    def resize(self, cols, rows):
        self.terminal.resize(cols, rows)

    def poll(self):
        pass

    def read(self):
        self.reads += 1
        if self.fd < 0:
            raise OSError(9, "stale PTY descriptor")
        return True

    def write(self):
        self.pending.clear()

    def close(self):
        self.closes += 1
        self.fd = -1


class EventSelector:
    def __init__(self, batches):
        self.mapping, self.batches, self.calls = {}, batches, 0

    def register(self, fd, events, data=None):
        key = selectors.SelectorKey(fd, fd, events, data)
        self.mapping[fd] = key
        return key

    def modify(self, fd, events, data=None):
        return self.register(fd, events, data)

    def unregister(self, fd):
        return self.mapping.pop(fd)

    def get_map(self):
        return self.mapping

    def select(self, timeout=None):
        self.calls += 1
        if self.calls > len(self.batches):
            raise AssertionError("frontend failed to finish the bounded event fixture")
        return self.batches[self.calls-1](self)

    def close(self):
        self.mapping.clear()


class Events(unittest.TestCase):
    def application(self, children):
        host = Host();app = here.Application("/bin/sh", "/", host=host)
        app.selector.close()

        def spawn(command=None):
            app.tabs = children[:]
            for child in children:
                app.selector.register(child.fd, selectors.EVENT_READ, child)
            app.choose(0)

        app.spawn = spawn
        return app, host

    def run_fixture(self, app, inputs):
        ticks = [0.0]

        def now():
            ticks[0] += .02
            return ticks[0]

        with patch.object(here.signal, "signal", return_value=signal.SIG_DFL), \
             patch.object(here.os, "get_terminal_size", return_value=os.terminal_size((80, 24))), \
             patch.object(here.os, "read", side_effect=inputs), \
             patch.object(here.os, "kill") as kill, \
             patch.object(here.time, "monotonic", side_effect=now):
            self.assertEqual(app.run(), 0)
        return kill

    def test_closing_one_tab_ignores_its_already_selected_event(self):
        closing, survivor = Child(101), Child(102)
        app, _ = self.application([closing, survivor])
        app.selector = EventSelector([
            lambda selector: [(selector.mapping[0], selectors.EVENT_READ),
                              (selector.mapping[101], selectors.EVENT_READ)],
            lambda selector: [(selector.mapping[0], selectors.EVENT_READ)],
        ])
        self.run_fixture(app, [b"\x02xy", b"\x02qy"])
        self.assertEqual(app.selector.calls, 2)  # survivor stayed alive until explicit quit
        self.assertEqual(closing.reads, 0)
        self.assertEqual((closing.closes, survivor.closes), (1, 1))

    def test_quit_stops_dispatching_the_rest_of_a_ready_batch(self):
        child = Child(101);app, _ = self.application([child])
        app.selector = EventSelector([
            lambda selector: [(selector.mapping[0], selectors.EVENT_READ),
                              (selector.mapping[101], selectors.EVENT_READ)],
        ])
        self.run_fixture(app, [b"\x02qy"])
        self.assertEqual(child.reads, 0)
        self.assertEqual(child.closes, 1)

    def test_last_tab_close_stops_dispatch_with_an_empty_tab_list(self):
        child = Child(101);app, _ = self.application([child])
        app.selector = EventSelector([
            lambda selector: [(selector.mapping[0], selectors.EVENT_READ),
                              (selector.mapping[101], selectors.EVENT_READ)],
        ])
        self.run_fixture(app, [b"\x02xy"])
        self.assertEqual(child.reads, 0)
        self.assertEqual(child.closes, 1)

    def test_resume_restores_child_modes_after_host_terminal_reset(self):
        child = Child(101)
        child.terminal.app_cursor = child.terminal.app_keypad = True
        child.terminal.mouse, child.terminal.cursor_shape = 1003, 5
        app, host = self.application([child])

        def suspend_after_first_paint(selector):
            app.suspend = True
            return []

        app.selector = EventSelector([
            suspend_after_first_paint,
            lambda selector: [(selector.mapping[0], selectors.EVENT_READ)],
        ])
        kill = self.run_fixture(app, [b"\x02qy"])
        kill.assert_called_once_with(os.getpid(), signal.SIGSTOP)
        entries = [i for i, data in enumerate(host.writes) if data == here.HostTerminal.ENTER]
        self.assertEqual(len(entries), 2)
        after_resume = b"".join(host.writes[entries[1]+1:])
        for mode in (b"\x1b[?1003h", b"\x1b[?1h", b"\x1b=", b"\x1b[5 q"):
            self.assertIn(mode, after_resume)


if __name__ == "__main__":
    unittest.main()
