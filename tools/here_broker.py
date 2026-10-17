#!/usr/bin/env python3
"""Private, persistent PTY owner for ``hardcore here`` (POSIX).

The foreground renderer is replaceable. Only this process owns PTY masters;
losing its client never closes a shell, job, screen, or unflushed shell history.
The protocol is newline-delimited bounded JSON over an owner-only Unix socket.
"""
from __future__ import annotations

import argparse
import base64
import errno
import fcntl
import json
import os
from pathlib import Path
import pty
import re
import selectors
import signal
import socket
import stat
import struct
import subprocess
import sys
import termios
import time
import uuid

try:
    from here_terminal import Terminal, MAX_COLS, MAX_ROWS, encode_rows, decode_rows
except ImportError:
    # Useful while developing the standalone engine; installed builds ship it.
    from here import Terminal, MAX_COLS, MAX_ROWS

VERSION = 1
MAX_TABS = 16
MAX_INPUT = 65536
MAX_REQUEST = 256 * 1024
MAX_FRAME = 16 * 1024 * 1024
MAX_OUTPUT = 32 * 1024 * 1024
MAX_PENDING = 2 * 1024 * 1024
_CHILDREN = {}  # Keep Popen handles to reap owned brokers on explicit shutdown.
NAME = re.compile(r'[A-Za-z0-9][A-Za-z0-9_.-]{0,47}\Z')


class BrokerError(RuntimeError):
    pass


def runtime_directory(root=None):
    path = Path(root) if root else Path('/tmp') / f'hardcore-here-{os.getuid()}'
    try:
        path.mkdir(mode=0o700)
    except FileExistsError:
        pass
    info = path.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
        raise BrokerError(f'Unsafe session directory: {path} (must be owned by you and mode 0700)')
    return path


def socket_path(name, root=None):
    if not isinstance(name, str) or not NAME.fullmatch(name):
        raise BrokerError('Session names use 1–48 letters, numbers, dots, underscores, or hyphens')
    return runtime_directory(root) / (name + '.sock')


def _same_user(sock):
    try:
        if hasattr(sock, 'getpeereid'):
            return sock.getpeereid()[0] == os.getuid()
        if hasattr(socket, 'SO_PEERCRED'):
            credentials = sock.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize('3i'))
            return struct.unpack('3i', credentials)[1] == os.getuid()
        # Darwin LOCAL_PEERCRED is exposed as getpeereid on supported Python.
        # Directory/socket ownership remains the authorization boundary there.
        return True
    except OSError:
        return False


def _encode(value):
    data = json.dumps(value, ensure_ascii=True, separators=(',', ':'), allow_nan=False).encode() + b'\n'
    if len(data) > MAX_FRAME:
        raise BrokerError('Session state exceeds the frame limit')
    return data


def _safe_connect(path, timeout=1):
    info = path.lstat()
    if not stat.S_ISSOCK(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
        raise BrokerError('Unsafe session socket')
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(timeout)
    try:
        sock.connect(str(path))
        if not _same_user(sock):
            raise BrokerError('Session belongs to another user')
    except BaseException:
        sock.close()
        raise
    return sock


def _read_message(sock, limit=MAX_FRAME):
    data = bytearray()
    while b'\n' not in data:
        part = sock.recv(min(65536, limit + 1 - len(data)))
        if not part:
            raise BrokerError('Session connection closed')
        data.extend(part)
        if len(data) > limit:
            raise BrokerError('Session frame is too large')
    first, remainder = data.split(b'\n', 1)
    return json.loads(first), remainder


_LIBPROC = None


def _process_name(pid):
    global _LIBPROC
    if sys.platform == 'darwin':
        try:
            import ctypes
            if _LIBPROC is None:
                _LIBPROC = ctypes.CDLL('/usr/lib/libproc.dylib')
                _LIBPROC.proc_name.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32]
                _LIBPROC.proc_name.restype = ctypes.c_int
            buffer = ctypes.create_string_buffer(256)
            if _LIBPROC.proc_name(pid, buffer, len(buffer)) > 0:
                return buffer.value.decode('utf-8', errors='replace')
        except (OSError, AttributeError):
            pass
    else:
        try:
            return Path(f'/proc/{pid}/comm').read_text().strip()[:256]
        except OSError:
            pass
    return ''


class Session:
    """An owned shell and its terminal state; no frontend fd is inherited."""
    def __init__(self, identifier, command, cwd, cols, rows, close_fds=()):
        self.id = identifier
        self.pending = bytearray()
        self.revision = 0
        self.terminal = Terminal(cols, rows, lambda data: self.queue(data, strict=False))
        self.exited = None
        self.fd = -1
        self.cwd = str(Path(cwd).resolve())
        self.label = Path(command[0]).name
        self.command = command
        self.last_output = 0.0
        self.program = self.label
        self.program_at = 0.0
        self.pid, self.fd = pty.fork()
        if not self.pid:
            try:
                for fd in close_fds:
                    if fd > 2:
                        try:
                            os.close(fd)
                        except OSError:
                            pass
                for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP, signal.SIGPIPE, signal.SIGTSTP):
                    signal.signal(sig, signal.SIG_DFL)
                os.chdir(self.cwd)
                fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack('HHHH', rows, cols, 0, 0))
                env = dict(os.environ, TERM='xterm-256color', COLORTERM='truecolor', TERM_PROGRAM='hardcore-here')
                for key in ('HARDCORE_CTL', 'HARDCORE_WINDOW_PID', 'HARDCORE_TAB_ID'):
                    env.pop(key, None)
                os.execvpe(command[0], command, env)
            except Exception as error:
                os.write(2, f'hardcore here: {error}\r\n'.encode(errors='replace'))
                os._exit(127)
        os.set_blocking(self.fd, False)
        self.resize(cols, rows)

    def queue(self, data, strict=True):
        if self.fd < 0:
            return
        if len(self.pending) + len(data) > MAX_PENDING:
            if not strict:
                return
            raise BrokerError('Shell input queue is full; wait before pasting more')
        self.pending.extend(data)

    def resize(self, cols, rows):
        if (cols, rows) != (self.terminal.cols, self.terminal.rows):
            self.terminal.resize(cols, rows)
            self.revision += 1
        if self.fd >= 0:
            try:
                fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack('HHHH', rows, cols, 0, 0))
            except OSError:
                pass

    def read(self):
        try:
            data = os.read(self.fd, 65536)
        except BlockingIOError:
            return True
        except OSError as error:
            if error.errno == errno.EIO:
                return False
            raise
        if not data:
            return False
        self.terminal.feed(data)
        self.revision += 1
        self.last_output = time.monotonic()
        return True

    def write(self):
        if not self.pending:
            return
        try:
            count = os.write(self.fd, self.pending)
            del self.pending[:count]
        except BlockingIOError:
            pass
        except OSError as error:
            if error.errno not in (errno.EIO, errno.EPIPE):
                raise
            self.pending.clear()

    def poll(self):
        if self.exited is None:
            try:
                pid, status = os.waitpid(self.pid, os.WNOHANG)
                if pid:
                    self.exited = os.waitstatus_to_exitcode(status)
            except ChildProcessError:
                self.exited = 0

    def current_directory(self):
        # OSC 7 from the shell takes precedence over the initial directory.
        cwd = getattr(self.terminal, 'cwd', '')
        if cwd and os.path.isdir(cwd):
            return cwd
        try:
            cwd = os.readlink(f'/proc/{self.pid}/cwd')
            if os.path.isdir(cwd):
                return cwd
        except OSError:
            pass
        # macOS has no /proc. This runs only when creating a tab, never while
        # repainting or streaming output, and inspects only our owned shell.
        if sys.platform == 'darwin':
            try:
                result = subprocess.run(['/usr/sbin/lsof', '-a', '-p', str(self.pid), '-d', 'cwd', '-Fn'],
                                        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=.3)
                for line in result.stdout.decode('utf-8', errors='replace').splitlines():
                    if line.startswith('n/') and os.path.isdir(line[1:]):
                        return line[1:]
            except (OSError, subprocess.TimeoutExpired):
                pass
        return self.cwd

    def metadata(self):
        term = self.terminal
        now = time.monotonic()
        if self.fd >= 0 and now - self.program_at >= .5:
            try:
                foreground = os.tcgetpgrp(self.fd)
                if foreground > 0:
                    self.program = _process_name(foreground) or self.label
            except OSError:
                pass
            self.program_at = now
        return dict(id=self.id, pid=self.pid, label=self.label, program=self.program, title=term.title,
                    cwd=getattr(term, 'cwd', '') or self.cwd, exited=self.exited,
                    revision=self.revision, history_pushed=term.history_pushed,
                    activity=self.last_output, bell_count=getattr(term, 'bell_count', 0),
                    pending=len(self.pending), cols=term.cols, rows=term.rows)

    def close(self):
        groups = {self.pid} if self.fd >= 0 or self.exited is None else set()
        if groups:
            try:
                result = subprocess.run(['ps', '-A', '-o', 'pid='], stdout=subprocess.PIPE,
                                        stderr=subprocess.DEVNULL, timeout=.3)
                for value in result.stdout.split():
                    try:
                        pid = int(value)
                        if os.getsid(pid) == self.pid:
                            groups.add(os.getpgid(pid))
                    except (OSError, ValueError):
                        pass
            except (OSError, subprocess.TimeoutExpired):
                pass
        if self.fd >= 0:
            try:
                foreground = os.tcgetpgrp(self.fd)
                if foreground > 0 and os.getsid(foreground) == self.pid:
                    groups.add(foreground)
            except OSError:
                pass
            os.close(self.fd)
            self.fd = -1
        groups = {group for group in groups if group > 1 and group != os.getpgrp()}
        for sig in (signal.SIGHUP, signal.SIGTERM, signal.SIGKILL):
            for group in tuple(groups):
                try:
                    os.killpg(group, sig)
                except (ProcessLookupError, PermissionError):
                    groups.discard(group)
            deadline = time.monotonic() + .2
            while groups and time.monotonic() < deadline:
                self.poll()
                for group in tuple(groups):
                    try:
                        os.killpg(group, 0)
                    except (ProcessLookupError, PermissionError):
                        groups.discard(group)
                if groups:
                    time.sleep(.01)
        deadline = time.monotonic() + .5
        while self.exited is None and time.monotonic() < deadline:
            self.poll()
            if self.exited is None:
                time.sleep(.01)


class Peer:
    def __init__(self, sock):
        self.sock = sock
        self.incoming = bytearray()
        self.outgoing = bytearray()
        self.attached = False
        self.watch = set()
        self.revisions = {}
        self.snapshots = {}
        self.metadata = None
        self.close_after_write = False
        self.accepted = time.monotonic()

    def emit(self, value):
        frame = _encode(value)
        if len(self.outgoing) + len(frame) > MAX_OUTPUT:
            raise BrokerError('Client is not consuming session output')
        self.outgoing.extend(frame)


class Broker:
    def __init__(self, name, shell, cwd, command, cols=80, rows=23, root=None):
        self.name, self.shell, self.cwd = name, shell, cwd
        self.command = command or [shell, '-i']
        self.cols, self.rows = self.dimensions(cols, rows)
        self.path = socket_path(name, root)
        self.selector = selectors.DefaultSelector()
        self.server = None
        self.peers = {}
        self.controller = None
        self.tabs = []
        self.active = 0
        self.next_id = 1
        self.stopping = False
        self.created = time.time()
        self.lock_fd = -1
        self.last_emit = 0.0
        self.theme = None

    @staticmethod
    def dimensions(cols, rows):
        if type(cols) is not int or type(rows) is not int:
            raise BrokerError('Terminal dimensions must be integers')
        return max(2, min(MAX_COLS, cols)), max(1, min(MAX_ROWS, rows))

    def register_session(self, command=None, cwd=None):
        if len(self.tabs) >= MAX_TABS:
            raise BrokerError(f'At most {MAX_TABS} tabs per session')
        command = command or [self.shell, '-i']
        if not isinstance(command, list) or not command or len(command) > 128 or any(
                not isinstance(s, str) or '\0' in s or len(s) > 65536 for s in command):
            raise BrokerError('Invalid shell command')
        if not command[0]:
            raise BrokerError('Shell executable must not be empty')
        if cwd is None:
            cwd = self.tabs[self.active].current_directory() if self.tabs else self.cwd
        if not isinstance(cwd, str) or not os.path.isdir(cwd):
            raise BrokerError('New tab directory does not exist')
        fds = list(self.selector.get_map()) + [self.lock_fd]
        tab = Session(self.next_id, command, cwd, self.cols, self.rows, fds)
        self.next_id += 1
        if self.theme:
            tab.terminal.set_theme(*self.theme)
        self.tabs.append(tab)
        self.active = len(self.tabs) - 1
        self.selector.register(tab.fd, selectors.EVENT_READ, tab)
        return tab

    def state(self):
        return dict(name=self.name, pid=os.getpid(), created=self.created,
                    active=self.active, attached=bool(self.controller), cols=self.cols, rows=self.rows,
                    tabs=[tab.metadata() for tab in self.tabs])

    def disconnect(self, peer):
        if self.controller is peer:
            self.controller = None
        fd = peer.sock.fileno()
        try:
            self.selector.unregister(peer.sock)
        except (KeyError, ValueError):
            pass
        self.peers.pop(fd, None)
        peer.sock.close()

    def handle(self, peer, request):
        if not isinstance(request, dict) or not isinstance(request.get('op'), str):
            raise BrokerError('Expected a command object')
        op = request['op']
        response = dict(event='reply', request=request.get('request'), ok=True)
        if op == 'hello':
            if request.get('version') != VERSION:
                raise BrokerError('Incompatible session protocol')
            if self.controller and self.controller is not peer:
                if request.get('takeover') is not True:
                    raise BrokerError('Session already attached; detach its other client first, or explicitly use takeover')
                old = self.controller
                old.emit(dict(event='detached', reason='Another client explicitly took over'))
                old.attached = False
                old.close_after_write = True
            self.controller = peer
            peer.attached = True
            peer.metadata = None
            response['state'] = self.state()
        elif op == 'status':
            response['state'] = self.state()
        elif op == 'stop':
            # Explicit owner-only administrative operation, including a detached session.
            self.stopping = True
        else:
            if self.controller is not peer or not peer.attached:
                raise BrokerError('Attach before controlling this session')
            if op == 'input':
                value = request.get('data')
                if not isinstance(value, str) or len(value) > (MAX_INPUT + 2) // 3 * 4:
                    raise BrokerError('Input chunk is too large')
                try:
                    data = base64.b64decode(value, validate=True)
                except (ValueError, TypeError):
                    raise BrokerError('Invalid input encoding') from None
                if len(data) > MAX_INPUT:
                    raise BrokerError('Input chunk is too large')
                tab = self.tab(request)
                tab.queue(data)
                response.update(tab=tab.id, pending=len(tab.pending))
            elif op == 'resize':
                self.cols, self.rows = self.dimensions(request.get('cols'), request.get('rows'))
                for tab in self.tabs:
                    tab.resize(self.cols, self.rows)
            elif op == 'choose':
                tab = self.tab(request)
                self.active = self.tabs.index(tab)
                # The selected tab needs complete searchable history even if
                # its background preview has been receiving screen-only deltas.
                peer.revisions.pop(tab.id, None)
            elif op == 'new':
                response['tab'] = self.register_session(request.get('command'), request.get('cwd')).id
            elif op == 'close':
                tab = self.tab(request)
                if tab.fd >= 0:
                    self.selector.unregister(tab.fd)
                tab.close()
                self.tabs.remove(tab)
                self.active = min(self.active, len(self.tabs)-1)
                if not self.tabs:
                    self.stopping = True
            elif op == 'watch':
                ids = request.get('tabs', [])
                if not isinstance(ids, list) or len(ids) > MAX_TABS or any(type(i) is not int for i in ids):
                    raise BrokerError('Invalid watched tabs')
                peer.watch = set(ids)
            elif op == 'snapshot':
                tab = self.tab(request)
                peer.revisions.pop(tab.id, None)
                peer.snapshots.pop(tab.id, None)
                peer.watch.add(tab.id)
            elif op == 'detach':
                peer.attached = False
                self.controller = None
                peer.close_after_write = True
            elif op == 'theme':
                palette = request.get('palette')
                fg, bg = request.get('foreground'), request.get('background')
                colors = (palette if isinstance(palette, list) else []) + [fg, bg]
                if not isinstance(palette, list) or len(palette) != 16 or any(
                        not isinstance(c, list) or len(c) != 3 or any(type(v) is not int or not 0 <= v <= 255 for v in c)
                        for c in colors):
                    raise BrokerError('Invalid theme colors')
                self.theme = (palette, fg, bg)
                for tab in self.tabs:
                    tab.terminal.set_theme(palette, fg, bg)
                    tab.revision += 1
                peer.revisions.clear()
                peer.snapshots.clear()
            else:
                raise BrokerError('Unknown session command')
        peer.emit(response)

    def tab(self, request):
        identifier = request.get('tab')
        if identifier is None and self.tabs:
            return self.tabs[self.active]
        for tab in self.tabs:
            if type(identifier) is int and tab.id == identifier:
                return tab
        raise BrokerError('Tab no longer exists')

    def requests(self, peer):
        # A paste containing many framed commands must not starve PTY reads.
        for _ in range(64):
            if b'\n' not in peer.incoming:
                break
            line, _, remainder = peer.incoming.partition(b'\n')
            peer.incoming = bytearray(remainder)
            request = json.loads(line)
            try:
                self.handle(peer, request)
            except (BrokerError, OSError, ValueError) as error:
                peer.emit(dict(event='reply', request=request.get('request') if isinstance(request, dict) else None,
                               ok=False, error=str(error)))

    def terminal_update(self, peer, tab):
        term = tab.terminal
        before = peer.snapshots.get(tab.id)
        shape = (term.cols, term.rows)
        active = bool(self.tabs and self.tabs[self.active] is tab)
        full = before is None or before['shape'] != shape or (active and not before.get('history_synced'))
        history_synced = before.get('history_synced', False) if before else False
        if full:
            event = dict(event='terminal', tab=tab.id, snapshot=term.snapshot(include_history=active))
            history_synced = active
        else:
            snapshot = term.snapshot(include_history=False, include_screen=False)
            changes = {}
            for name in ('normal', 'alternate'):
                grid = getattr(term, name).grid
                indices = [i for i, row in enumerate(grid) if tuple(row) != before[name][i]]
                if indices:
                    changes[name] = dict(indices=indices, cells=encode_rows(grid[i] for i in indices))
            delta = term.history_pushed - before['pushed']
            history = None
            history_changed = delta or len(term.history) != before['history_length']
            if history_changed and not active:
                history_synced = False
            if history_changed and active:
                # Appends are cheap. Clear/reflow/reset requires one complete history.
                append = (0 <= delta <= len(term.history) and
                          len(term.history) == min(term.history.maxlen, before['history_length'] + delta))
                count = delta if append else len(term.history)
                history = dict(append=append, cells=encode_rows(list(term.history)[-count:] if count else []),
                               wrapped=list(term.history_wrapped)[-count:] if count else [],
                               used=list(term.history_used)[-count:] if count else [], limit=term.history.maxlen)
            event = dict(event='terminal_delta', tab=tab.id, snapshot=snapshot, changes=changes, history=history)
        peer.emit(event)
        peer.snapshots[tab.id] = dict(shape=shape, pushed=term.history_pushed, history_length=len(term.history),
                                     history_synced=history_synced,
                                     normal=[tuple(row) for row in term.normal.grid],
                                     alternate=[tuple(row) for row in term.alternate.grid])
        peer.revisions[tab.id] = tab.revision

    def send_changes(self):
        peer = self.controller
        if not peer or peer.outgoing:
            return
        state = self.state()
        if state != peer.metadata:
            peer.emit(dict(event='state', state=state))
            peer.metadata = state
        current = self.tabs[self.active] if self.tabs else None
        live = {tab.id for tab in self.tabs}
        peer.watch.intersection_update(live)
        peer.revisions = {key: value for key, value in peer.revisions.items() if key in live}
        peer.snapshots = {key: value for key, value in peer.snapshots.items() if key in live}
        watched = peer.watch | ({current.id} if current else set())
        # Prioritize the chosen terminal and stream initial background snapshots
        # over successive writes. Sixteen full histories must never overflow the
        # peer queue merely because all previews were requested together.
        ordered = ([current] if current else []) + [tab for tab in self.tabs if tab is not current]
        for tab in ordered:
            if tab.id in watched and peer.revisions.get(tab.id) != tab.revision:
                self.terminal_update(peer, tab)
                if len(peer.outgoing) >= 2 * 1024 * 1024:
                    break

    def serve(self):
        os.umask(0o077)
        lockpath = self.path.with_suffix('.lock')
        self.lock_fd = os.open(lockpath, os.O_RDWR | os.O_CREAT | getattr(os, 'O_NOFOLLOW', 0), 0o600)
        lockinfo = os.fstat(self.lock_fd)
        if not stat.S_ISREG(lockinfo.st_mode) or lockinfo.st_uid != os.getuid() or lockinfo.st_mode & 0o077:
            raise BrokerError('Unsafe session lock')
        try:
            fcntl.flock(self.lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise BrokerError('Session already exists') from None
        if self.path.exists() or self.path.is_symlink():
            info = self.path.lstat()
            if not stat.S_ISSOCK(info.st_mode) or info.st_uid != os.getuid():
                raise BrokerError('Unsafe existing socket path')
            self.path.unlink()
        self.server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.server.bind(str(self.path))
        self.server.listen(8)
        self.server.setblocking(False)
        self.selector.register(self.server, selectors.EVENT_READ, None)
        old = {}
        for sig in (signal.SIGTERM, signal.SIGINT):
            old[sig] = signal.signal(sig, lambda *_: setattr(self, 'stopping', True))
        old[signal.SIGHUP] = signal.signal(signal.SIGHUP, signal.SIG_IGN)
        try:
            self.register_session(self.command, self.cwd)
            while not self.stopping:
                for tab in self.tabs:
                    tab.poll()
                    if tab.fd >= 0:
                        self.selector.modify(tab.fd, selectors.EVENT_READ | (selectors.EVENT_WRITE if tab.pending else 0), tab)
                if time.monotonic() - self.last_emit >= 1/30:
                    try:
                        self.send_changes()
                    except (BrokerError, OSError):
                        if self.controller:
                            self.disconnect(self.controller)
                    self.last_emit = time.monotonic()
                for peer in list(self.peers.values()):
                    try:
                        self.requests(peer)
                    except (OSError, ValueError, RecursionError, BrokerError):
                        self.disconnect(peer)
                        continue
                    if peer.close_after_write and not peer.outgoing:
                        self.disconnect(peer)
                    elif not peer.attached and not peer.outgoing and time.monotonic() - peer.accepted > 5:
                        self.disconnect(peer)
                    else:
                        self.selector.modify(peer.sock, selectors.EVENT_READ | (selectors.EVENT_WRITE if peer.outgoing else 0), peer)
                for key, events in self.selector.select(.025 if self.controller else .1):
                    if key.data is None:
                        sock, _ = self.server.accept()
                        if len(self.peers) >= 8 or not _same_user(sock):
                            sock.close()
                            continue
                        sock.setblocking(False)
                        peer = Peer(sock)
                        self.peers[sock.fileno()] = peer
                        self.selector.register(sock, selectors.EVENT_READ, peer)
                    elif isinstance(key.data, Session):
                        tab = key.data
                        if tab not in self.tabs or tab.fd != key.fd:
                            continue
                        if events & selectors.EVENT_READ and not tab.read():
                            self.selector.unregister(tab.fd)
                            os.close(tab.fd)
                            tab.fd = -1
                            tab.poll()
                        if events & selectors.EVENT_WRITE and tab.fd >= 0:
                            tab.write()
                    else:
                        peer = key.data
                        if peer.sock.fileno() < 0:
                            continue
                        try:
                            if events & selectors.EVENT_READ:
                                data = peer.sock.recv(65536)
                                if not data:
                                    self.disconnect(peer)
                                    continue
                                peer.incoming.extend(data)
                                if len(peer.incoming) > MAX_REQUEST:
                                    raise BrokerError('Session command too large')
                                self.requests(peer)
                            if events & selectors.EVENT_WRITE and peer.outgoing:
                                count = peer.sock.send(peer.outgoing[:65536])
                                del peer.outgoing[:count]
                        except (OSError, ValueError, RecursionError, BrokerError):
                            self.disconnect(peer)
            # Deliver the explicit stop acknowledgement before hanging up.
            for peer in self.peers.values():
                if peer.outgoing:
                    try:
                        peer.sock.send(peer.outgoing[:65536])
                    except OSError:
                        pass
        finally:
            for tab in self.tabs:
                tab.close()
            for peer in list(self.peers.values()):
                self.disconnect(peer)
            if self.server:
                self.server.close()
                self.path.unlink(missing_ok=True)
            self.selector.close()
            if self.lock_fd >= 0:
                os.close(self.lock_fd)
            for sig, handler in old.items():
                signal.signal(sig, handler)


class BrokerClient:
    """Nonblocking client: register ``fileno()`` for reads; poll/drain each tick.

    ``state`` contains session/tab metadata. ``terminals[id]`` contains live
    Terminal objects. ``send(op, **arguments)`` queues a request; returned events
    include asynchronous errors. Call ``flush()`` every loop (or select writable
    when ``pending`` is nonempty). Closing this client only detaches it.
    """
    def __init__(self, name, *, root=None, takeover=False):
        self.name = name
        self.sock = _safe_connect(socket_path(name, root))
        self.pending = bytearray()
        self.incoming = bytearray()
        self.sequence = 0
        self.state = {}
        self.terminals = {}
        self.closed = False
        self.sock.sendall(_encode(dict(op='hello', version=VERSION, takeover=takeover, request=0)))
        reply, remainder = _read_message(self.sock)
        if not reply.get('ok'):
            self.sock.close()
            raise BrokerError(reply.get('error', 'Cannot attach session'))
        self.state = reply.get('state', {})
        self.incoming.extend(remainder)
        self.sock.setblocking(False)

    def fileno(self):
        return self.sock.fileno()

    def send(self, op, **arguments):
        if self.closed:
            raise BrokerError('Session connection closed')
        self.sequence += 1
        data = _encode(dict(op=op, request=self.sequence, **arguments))
        if len(data) > MAX_REQUEST or len(self.pending) + len(data) > MAX_PENDING:
            raise BrokerError('Session request queue is full')
        self.pending.extend(data)
        self.flush()
        return self.sequence

    def input(self, data, tab=None):
        requests = []
        for offset in range(0, len(data), MAX_INPUT):
            requests.append(self.send('input', tab=tab, data=base64.b64encode(data[offset:offset+MAX_INPUT]).decode('ascii')))
        return requests

    def flush(self):
        if self.closed or not self.pending:
            return
        try:
            count = self.sock.send(self.pending)
            del self.pending[:count]
        except BlockingIOError:
            pass
        except OSError as error:
            self.close()
            raise BrokerError(f'Session disconnected: {error}') from error

    def drain(self):
        if self.closed:
            return []
        events = []
        for _ in range(256):
            try:
                part = self.sock.recv(65536)
            except BlockingIOError:
                break
            except OSError:
                part = b''
            if not part:
                self.closed = True
                events.append(dict(event='disconnected'))
                break
            self.incoming.extend(part)
            if len(self.incoming) > MAX_OUTPUT:
                self.close()
                raise BrokerError('Session response exceeds limit')
        while b'\n' in self.incoming:
            line, _, rest = self.incoming.partition(b'\n')
            self.incoming = bytearray(rest)
            if len(line) > MAX_FRAME:
                self.close()
                raise BrokerError('Session frame exceeds limit')
            try:
                message = json.loads(line)
            except (ValueError, RecursionError) as error:
                self.close()
                raise BrokerError('Invalid session response') from error
            if not isinstance(message, dict):
                self.close()
                raise BrokerError('Invalid session response')
            if message.get('event') == 'state':
                self.state = message['state']
                live = {tab['id'] for tab in self.state['tabs']}
                self.terminals = {key: value for key, value in self.terminals.items() if key in live}
            elif message.get('event') == 'terminal':
                identifier = message['tab']
                if identifier in self.terminals:
                    self.terminals[identifier].apply_snapshot(message['snapshot'])
                else:
                    self.terminals[identifier] = Terminal.from_snapshot(message['snapshot'])
            elif message.get('event') == 'terminal_delta':
                identifier = message['tab']
                term = self.terminals.get(identifier)
                if term is None:
                    self.send('snapshot', tab=identifier)
                    continue
                term.apply_snapshot(message['snapshot'])
                for name, change in message.get('changes', {}).items():
                    if name not in ('normal', 'alternate'):
                        raise BrokerError('Invalid terminal buffer')
                    rows = decode_rows(change['cells'], max_rows=MAX_ROWS)
                    indices = change['indices']
                    if len(rows) != len(indices) or any(type(i) is not int or not 0 <= i < term.rows for i in indices):
                        raise BrokerError('Invalid changed screen rows')
                    for row, index in zip(rows, indices):
                        if len(row) != term.cols:
                            raise BrokerError('Invalid changed row width')
                        getattr(term, name).grid[index] = row
                history = message.get('history')
                if history is not None:
                    from collections import deque
                    rows = decode_rows(history['cells'])
                    wrapped, used = history['wrapped'], history['used']
                    limit = history['limit']
                    if not (type(limit) is int and 1 <= limit <= 4000 and len(rows) == len(wrapped) == len(used)):
                        raise BrokerError('Invalid history update')
                    if any(len(row) != term.cols for row in rows) or any(type(v) is not bool for v in wrapped) or any(
                            type(v) is not int or not 0 <= v <= term.cols for v in used):
                        raise BrokerError('Invalid history rows')
                    if not history['append']:
                        term.history = deque(maxlen=limit)
                        term.history_wrapped = deque(maxlen=limit)
                        term.history_used = deque(maxlen=limit)
                    term.history.extend(rows)
                    term.history_wrapped.extend(wrapped)
                    term.history_used.extend(used)
            events.append(message)
        self.flush()
        return events

    def poll(self, timeout=0):
        if not self.closed:
            import select
            select.select([self.sock], [self.sock] if self.pending else [], [], timeout)
        return self.drain()

    def close(self):
        self.closed = True
        self.sock.close()


# Helpers below are startup/administrative operations, never render-loop work.
def status_session(name, root=None, timeout=.5):
    with _safe_connect(socket_path(name, root), timeout) as sock:
        sock.sendall(_encode(dict(op='status')))
        value, _ = _read_message(sock)
        if not value.get('ok'):
            raise BrokerError(value.get('error', 'Cannot inspect session'))
        return value['state']


def list_sessions(root=None):
    result = []
    for path in sorted(runtime_directory(root).glob('*.sock')):
        try:
            result.append(status_session(path.stem, root, .15))
        except (OSError, ValueError, BrokerError):
            pass
    return result


def stop_session(name, root=None):
    with _safe_connect(socket_path(name, root)) as sock:
        sock.sendall(_encode(dict(op='stop')))
        reply, _ = _read_message(sock)
        if not reply.get('ok'):
            raise BrokerError(reply.get('error', 'Cannot stop session'))
    process = _CHILDREN.pop(str(socket_path(name, root)), None)
    if process is not None:
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            _CHILDREN[str(socket_path(name, root))] = process


def start_session(name=None, *, shell=None, cwd=None, command=None, cols=80, rows=23, root=None):
    name = name or ('here-' + uuid.uuid4().hex[:8])
    path = socket_path(name, root)
    try:
        status_session(name, root, .2)
    except (FileNotFoundError, ConnectionRefusedError):
        pass
    else:
        raise BrokerError(f'Session {name} already exists')
    args = [sys.executable, str(Path(__file__).resolve()), '--serve', name,
            '--shell', shell or os.environ.get('SHELL') or '/bin/sh', '--cwd', cwd or os.getcwd(),
            '--cols', str(cols), '--rows', str(rows)]
    if root:
        args += ['--runtime', str(root)]
    if command:
        args += ['--command-json', json.dumps(command)]
    process = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL, start_new_session=True, close_fds=True)
    _CHILDREN[str(path)] = process
    deadline = time.monotonic() + 4
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise BrokerError('Session owner failed to start')
        try:
            state = status_session(name, root, .1)
            if state['tabs']:
                return name
        except (OSError, ValueError, BrokerError):
            pass
        time.sleep(.02)
    process.terminate()
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
    raise BrokerError('Timed out starting session')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serve', required=True)
    parser.add_argument('--shell', default='/bin/sh')
    parser.add_argument('--cwd', default=os.getcwd())
    parser.add_argument('--cols', type=int, default=80)
    parser.add_argument('--rows', type=int, default=23)
    parser.add_argument('--runtime')
    parser.add_argument('--command-json')
    args = parser.parse_args(argv)
    try:
        broker = Broker(args.serve, args.shell, args.cwd,
                        json.loads(args.command_json) if args.command_json else None,
                        args.cols, args.rows, args.runtime)
        broker.serve()
    except (OSError, ValueError, BrokerError) as error:
        print(f'hardcore here: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
