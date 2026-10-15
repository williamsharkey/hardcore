#!/usr/bin/env python3
"""Late zsh/clone into a real Hardcore window, using only owned fixtures.

No Terminal.app interaction, debugger, process-table inspection or prelaunch
host. The existing native same-user FD handoff receives the clone's PTY.
This demonstrates shell memory + an unsent ZLE line, NOT arbitrary jobs or
Terminal screen/scrollback preservation. Never use it on an existing session.
"""
import argparse
import array
from collections import deque
import fcntl
import json
import os
from pathlib import Path
import plistlib
import re
import select
import shlex
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import time

from probe_mac_zsh_clone_state import Shell, deadline


def fixture_width(character):
    """Cell widths for this fixed fixture alphabet, not general Unicode."""
    if ' ' <= character <= '~' or character == 'é':
        return 1
    if character == '🦊':
        return 2
    raise ValueError(f'unsupported fixture character U+{ord(character):04X}')


def fixture_rows(text, columns, keep=None):
    rows = deque(maxlen=keep)
    count = 0
    def append(row):
        nonlocal count
        count += 1
        if count > 4096:
            raise ValueError('fixture snapshot has too many physical rows')
        rows.append(row + [' '] * (columns - len(row)))
    for line in text.split('\r\n'):
        row = []
        for character in line:
            width = fixture_width(character)
            if len(row) + width > columns:
                append(row)
                row = []
            row.append(character)
            if width == 2:
                row.append('')  # Wide-cell continuation.
        append(row)
    return list(rows)


def fixture_cursor(text, columns, following=''):
    row = column = 0
    for character in text:
        width = fixture_width(character)
        if column + width > columns:
            row, column = row + 1, 0
        column += width
    if column == columns or (following and column + fixture_width(following[0]) > columns):
        row, column = row + 1, 0
    return row, column


def verify_fixture_snapshot(snapshot, columns, rows, draft, cursor, view=0):
    """Decode only the plain-main-screen term_snapshot subset used here.

    The writer emits history/main cells first, then a cursor/mode trailer.
    Unknown escape sequences, alternate/origin/custom-region modes and pending
    wrap trailers fail closed. This does not parse arbitrary terminal output.
    """
    if not (10 <= columns <= 1000 and 3 <= rows <= 500 and view == 0):
        raise ValueError('unsupported snapshot dimensions or scrollback view')
    if not isinstance(snapshot, bytes) or len(snapshot) > 4 * 1024 * 1024:
        raise ValueError('invalid snapshot bytes')
    text = snapshot.decode('utf-8', errors='strict')
    first_cup = re.search(r'\x1b\[[0-9]+;[0-9]+H', text)
    if not text.startswith('\x1b[0m') or first_cup is None:
        raise ValueError('snapshot lacks canonical main body/cursor trailer')
    body, trailer = text[:first_cup.start()], text[first_cup.start():]
    # The fixture has no styling. In particular, do not strip conceal or
    # foreground/background colors and then claim the text was visible.
    body = body.replace('\x1b[0m', '')
    if '\x1b' in body or '\n' in body.replace('\r\n', '') or '\r' in body.replace('\r\n', ''):
        raise ValueError('unsupported escape or row delimiter in snapshot body')
    footer = re.fullmatch(
        r'\x1b\[[0-9]+;[0-9]+H\x1b\(B\x1b7\x1b\(B'
        r'\x1b\[\?1[hl]\x1b\[\?25h\x1b\[\?1004[hl]'
        r'\x1b\[\?1006[hl]\x1b\[\?2004[hl]\x1b\[[246] q'
        r'\x1b\(B\x1b\[([0-9]+);([0-9]+)H', trailer)
    if footer is None:
        raise ValueError('unsupported snapshot cursor/mode trailer')
    actual_cursor = int(footer[1]) - 1, int(footer[2]) - 1
    if not (0 <= actual_cursor[0] < rows and 0 <= actual_cursor[1] < columns):
        raise ValueError('snapshot cursor is out of bounds')
    actual_rows = fixture_rows(body, columns, keep=rows)
    if len(actual_rows) < rows:
        raise ValueError('snapshot is missing main-screen rows')
    if not 0 <= cursor < len(draft):
        raise ValueError('fixture requires a cursor inside the unsent draft')
    expected_rows = fixture_rows('HC> ' + draft, columns)
    if len(expected_rows) > rows:
        raise ValueError('fixture draft does not fit the viewport')
    expected_rows += [[' '] * columns for _ in range(rows - len(expected_rows))]
    if actual_rows[-rows:] != expected_rows:
        raise AssertionError('complete visible draft or surrounding blank cells differ')
    expected_cursor = fixture_cursor('HC> ' + draft[:cursor], columns, draft[cursor:])
    if actual_cursor != expected_cursor:
        raise AssertionError(f'visible cursor {actual_cursor} differs from {expected_cursor}')
    return {'row': actual_cursor[0], 'column': actual_cursor[1],
            'cols': columns, 'rows': rows, 'completeVisibleDraft': True}


def fixture_history_events(data):
    parts = data.split(b'\0')
    if parts[-1:] != [b''] or (len(parts) - 1) % 2:
        raise ValueError('invalid fixture history records')
    pairs = list(zip(parts[:-1:2], parts[1:-1:2]))
    if any(not number.isdigit() for number, _ in pairs):
        raise ValueError('invalid fixture history event number')
    return pairs


def verify_fixture_state(reference, received):
    for value in (reference, received):
        if not value['state'].startswith('PID=') or '\n' not in value['state']:
            raise ValueError('missing shell identity record')
        events = fixture_history_events(value['history'])
        if sum(event == b'HC_NATIVE_UNFLUSHED_SENTINEL' for _, event in events) != 2:
            raise AssertionError('exact unflushed history sentinels are missing')
    if reference['state'].split('\n', 1)[1] != received['state'].split('\n', 1)[1]:
        raise AssertionError('captured shell memory differs from the source')
    if reference['history'] != received['history']:
        raise AssertionError('history event identities, values or order changed')


def read_pty_geometry(descriptor):
    rows, columns, _, _ = struct.unpack('HHHH', fcntl.ioctl(
        descriptor, termios.TIOCGWINSZ, b'\0' * 8))
    return {'columns': columns, 'rows': rows,
            'foregroundPgrp': os.tcgetpgrp(descriptor)}


def verify_fixture_geometry(shell_geometry, pty_geometry, columns, rows, expected_pgrp):
    if not (10 <= columns <= 1000 and 3 <= rows <= 500):
        raise ValueError('invalid fixture terminal dimensions')
    if tuple(shell_geometry) != (columns, rows):
        raise AssertionError(f'shell geometry {shell_geometry} differs from {(columns, rows)}')
    if (pty_geometry.get('columns'), pty_geometry.get('rows')) != (columns, rows):
        raise AssertionError('kernel PTY geometry differs from the terminal grid')
    if expected_pgrp <= 1 or pty_geometry.get('foregroundPgrp') != expected_pgrp:
        raise AssertionError('owned shell is not the PTY foreground process group')


def send(sock, obj):
    data = plistlib.dumps(obj, fmt=plistlib.FMT_BINARY)
    sock.sendall(struct.pack('!I', len(data)) + data)


def receive(sock, pump=None):
    limit = time.monotonic() + (sock.gettimeout() or 20)
    def exact(size):
        data = b''
        while len(data) < size:
            if pump is not None:
                pump()
            remaining = limit - time.monotonic()
            if remaining <= 0:
                raise TimeoutError('native packet deadline reached')
            if not select.select([sock], [], [], min(.05, remaining))[0]:
                continue
            part = sock.recv(size - len(data))
            if not part:
                raise EOFError('native receiver closed before acknowledgement')
            data += part
        return data
    size = struct.unpack('!I', exact(4))[0]
    if not 0 < size < 4 * 1024 * 1024:
        raise ValueError('invalid native packet size')
    return plistlib.loads(exact(size))


def request(path, payload):
    with socket.socket(socket.AF_UNIX) as peer:
        peer.settimeout(5)
        peer.connect(str(path))
        send(peer, payload)
        return receive(peer)


def take_snapshot(path, clone_pid, pump=None):
    """Borrow a snapshot, close every received FD, and abort the take."""
    descriptors = []
    with socket.socket(socket.AF_UNIX) as preview:
        try:
            preview.settimeout(5)
            preview.connect(str(path))
            send(preview, {'op': 'take'})
            packet = receive(preview, pump=pump)
            marker, ancillary, flags, _ = preview.recvmsg(1, socket.CMSG_SPACE(256 * 4))
            for level, kind, payload in ancillary:
                if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                    integers = array.array('i')
                    integers.frombytes(payload[:len(payload) - len(payload) % integers.itemsize])
                    descriptors.extend(integers)
            if marker != b'F' or flags & socket.MSG_CTRUNC:
                raise ValueError('invalid snapshot descriptor packet')
            if len(packet.get('tabs', [])) != 1 or len(descriptors) != 1:
                raise ValueError('snapshot must contain exactly one owned tab')
            tab = packet['tabs'][0]
            if tab.get('pid') != clone_pid:
                raise ValueError('snapshot belongs to an unexpected shell')
            return dict(tab, probePtyGeometry=read_pty_geometry(descriptors[0]))
        finally:
            # No ready/go acknowledgement: disconnect returns the donor to its
            # normal input loop. FD cleanup also runs on malformed packets.
            errors = []
            for descriptor in descriptors:
                try:
                    os.close(descriptor)
                except OSError as error:
                    errors.append(str(error))
            if errors and sys.exc_info()[0] is None:
                raise RuntimeError('snapshot descriptor cleanup failed: ' + '; '.join(errors))


def control(path, value):
    fd = os.open(path, os.O_WRONLY | os.O_NONBLOCK)
    try:
        os.write(fd, (value + '\n').encode())
    finally:
        os.close(fd)


def observed(root, pid, stage):
    prefix = root / f'{stage}-{pid}'
    try:
        serial = int(prefix.with_suffix('.ready').read_text())
        generation = root / f'{stage}-{pid}-{serial}'
        value = {'serial': serial, 'draft': generation.with_suffix('.draft').read_text(),
                 'cursor': int(generation.with_suffix('.cursor').read_text()),
                 'state': generation.with_suffix('.state').read_text(),
                 'history': generation.with_suffix('.history').read_bytes(),
                 'geometry': tuple(map(int, generation.with_suffix('.geometry').read_text().split()))}
        return value if serial == int(prefix.with_suffix('.ready').read_text()) else None
    except (OSError, ValueError):
        return None


def run(root, executable, clone_only=False):
    shell = Shell(root, zle=True)
    gui = None
    peer = child = None
    outputs = {'source': bytearray(), 'clone': bytearray(), 'receiver': bytearray()}

    def drain():
        # An emulator consumes output while waiting for input. File-based
        # acknowledgements alone can otherwise leave a shell blocked in a
        # redraw against the finite PTY output queue.
        descriptors = [('source', shell.master), ('clone', shell.new_master)]
        if gui is not None and gui.stdout is not None and not gui.stdout.closed:
            descriptors.append(('receiver', gui.stdout.fileno()))
        for name, fd in descriptors:
            if fd < 0:
                continue
            for _ in range(16):
                if not select.select([fd], [], [], 0)[0]:
                    break
                try:
                    part = os.read(fd, 65536)
                except OSError:
                    break
                if not part:
                    break
                remaining = 2 * 1024 * 1024 - len(outputs[name])
                outputs[name].extend(part[:remaining])
                if len(part) > remaining:
                    raise AssertionError('unexpected fixture output volume')

    def until(check, timeout=5):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            drain()
            answer = check()
            if answer:
                return answer
            time.sleep(.02)
        raise AssertionError('fixture did not reach the expected state')

    def wait_screen(stage, draft, cursor):
        # A shell's file acknowledgement can precede ZLE's redraw. Clear once,
        # then retry only read-only transactions, never any buffer mutation.
        previous = observed(root, clone_pid, 'observed')
        previous_serial = previous['serial'] if previous else 0
        control(ctl, 'type \x0c\x18\x16')
        last_observation_request = time.monotonic()
        snapshot_deadline = time.monotonic() + 8
        snapshot_error = None
        while time.monotonic() < snapshot_deadline:
            drain()
            current = observed(root, clone_pid, 'observed')
            if not current or current['serial'] <= previous_serial:
                if time.monotonic() - last_observation_request >= .1:
                    control(ctl, 'type \x18\x16')
                    last_observation_request = time.monotonic()
                time.sleep(.02)
                continue
            tab = take_snapshot(endpoint, clone_pid, pump=drain)
            snapshot = tab['snapshot']
            dimensions = {key: tab.get(key) for key in ('cols', 'rows', 'view')}
            dimensions.update(shellGeometry=current['geometry'],
                              ptyGeometry=tab['probePtyGeometry'],
                              observationSerial=current['serial'])
            (root / f'{stage}-snapshot.ansi').write_bytes(snapshot)
            (root / f'{stage}-snapshot.json').write_text(json.dumps(dimensions, indent=2))
            try:
                assert current['draft'] == draft and current['cursor'] == cursor, \
                    'live editor draft/cursor changed during screen verification'
                verify_fixture_state(original, current)
                verify_fixture_geometry(current['geometry'], tab['probePtyGeometry'],
                                        tab['cols'], tab['rows'], clone_pid)
                screen = verify_fixture_snapshot(snapshot, tab['cols'], tab['rows'],
                                                 draft, cursor, tab['view'])
                return dict(screen, shellGeometry=current['geometry'],
                            ptyGeometry=tab['probePtyGeometry'])
            except (AssertionError, ValueError) as error:
                snapshot_error = error
                previous_serial = current['serial']
                control(ctl, 'type \x18\x16')  # Refresh evidence, never edit or force dimensions.
                last_observation_request = time.monotonic()
                time.sleep(.05)
        raise AssertionError('native draft/cursor snapshot never matched: ' + str(snapshot_error))

    try:
        home = root / 'home'
        (home / 'Library/Application Support').mkdir(parents=True)
        runtime, ctl = root / 'run', root / 'receiver.ctl'
        shell.new_master, shell.new_slave = os.openpty()
        tty = os.ttyname(shell.new_slave)
        # forkpty may start with a zero kernel size, while zsh defaults to 80x24.
        # Give both owned PTYs a real common size; later import must resize the
        # receiver itself and deliver the kernel's ordinary SIGWINCH.
        for descriptor in (shell.master, shell.new_slave):
            fcntl.ioctl(descriptor, termios.TIOCSWINSZ, struct.pack('HHHH', 32, 110, 0, 0))
        setup = root / 'fixture.zsh'
        setup.write_text('HC_ROOT=' + shlex.quote(str(root)) + '\nHC_RECEIVER=' + shlex.quote(tty) + '\n' + r'''
HC_LOCAL='kept in memory café 🦊'
typeset -a HC_ARRAY=(first 'two words β')
typeset -A HC_MAP=(side 'value λ')
function hc_saved_function() { print -r -- "$HC_LOCAL:${HC_ARRAY[2]}:${HC_MAP[side]}" }
alias hc_saved_alias=hc_saved_function
setopt EXTENDED_GLOB
HISTSIZE=500 SAVEHIST=500 HISTFILE="$HC_ROOT/unflushed.history"
print -s -- HC_NATIVE_UNFLUSHED_SENTINEL
cd "$HC_ROOT"
zmodload zsh/clone zsh/parameter
function hc_capture() {
  local stage=$1 destination event
  (( HC_CAPTURE_SERIAL++ ))
  destination="$HC_ROOT/$stage-$$-$HC_CAPTURE_SERIAL"
  print -rn -- "$BUFFER" > "$destination.draft"
  print -r -- "$CURSOR" > "$destination.cursor"
  print -r -- "$COLUMNS $LINES" > "$destination.geometry"
  {
    print -r -- "PID=$$ TTY=$TTY"
    typeset -p HC_LOCAL HC_ARRAY HC_MAP
    whence -v hc_saved_alias hc_saved_function
    functions hc_saved_function
    print -r -- "OPTION=$options[extendedglob] CWD=$PWD"
    hc_saved_function
  } > "$destination.state"
  for event in ${(onk)history}; do
    printf '%s\0%s\0' "$event" "${history[$event]}"
  done > "$destination.history"
  print -r -- "$HC_CAPTURE_SERIAL" > "$HC_ROOT/$stage-$$.ready"
}
function hc_clone_widget() {
  local draft=$BUFFER cursor=$CURSOR
  clone "$HC_RECEIVER" || return
  BUFFER=$draft CURSOR=$cursor
  # This one-time proof writes to the terminal. Invalidate/redraw the editor
  # around it; later observation widgets only write private evidence files.
  zle -I
  if print -r -- "HC_CTTY_PROOF_$$" > /dev/tty; then
    print -r -- yes > "$HC_ROOT/initial-$$.ctty"
  else
    print -r -- no > "$HC_ROOT/initial-$$.ctty"
  fi
  hc_capture initial
  zle reset-prompt
}
function hc_observe_widget() { hc_capture observed }
zle -N hc_clone_widget
zle -N hc_observe_widget
bindkey '^X^X' hc_clone_widget
bindkey '^X^V' hc_observe_widget
print -r -- HC_NATIVE_FIXTURE_READY
''')
        shell.command(shell.master, 'source ' + shlex.quote(str(setup)))
        shell.expect(shell.master, b'\r?\nHC_NATIVE_FIXTURE_READY\r?\n')
        shell.expect(shell.master, b'HC> ')
        shell.command(shell.master, 'print -s -- HC_NATIVE_UNFLUSHED_SENTINEL; print -r -- HC_HISTORY_READY')
        shell.expect(shell.master, b'\r?\nHC_HISTORY_READY\r?\n')
        shell.expect(shell.master, b'HC> ')
        assert not (root / 'unflushed.history').exists()
        # Observe the editor's geometry before entering a draft. Do not assign
        # shell variables or manually signal it to conceal a lost resize.
        geometry_deadline = time.monotonic() + 5
        last_observation_request = 0
        while time.monotonic() < geometry_deadline:
            if time.monotonic() - last_observation_request >= .1:
                os.write(shell.master, b'\x18\x16')
                last_observation_request = time.monotonic()
            drain()
            initial = observed(root, shell.pid, 'observed')
            if initial and initial['geometry'] == (110, 32):
                source_geometry = read_pty_geometry(shell.master)
                verify_fixture_geometry(initial['geometry'], source_geometry, 110, 32, shell.pid)
                break
            time.sleep(.02)
        else:
            raise AssertionError('source editor did not acknowledge its initial PTY size')
        forbidden = root / 'MUST_NOT_EXECUTE'
        draft = "printf '%s' 'UNSENT café 🦊'; touch " + shlex.quote(str(forbidden))
        cursor = draft.index('🦊') + 1  # Inside printf text, after the wide glyph.
        os.write(shell.master, draft.encode() + b'\x1b[D' * (len(draft) - cursor) + b'\x18\x16')
        original = until(lambda: (value if (value := observed(root, shell.pid, 'observed'))
                                  and value['serial'] > initial['serial']
                                  and value['draft'] == draft and value['cursor'] == cursor else None))
        assert original['draft'] == draft and original['cursor'] == cursor
        verify_fixture_geometry(original['geometry'], read_pty_geometry(shell.master), 110, 32, shell.pid)
        # Prompt text can precede ZLE entering raw mode. Observe an actual
        # editor widget before taking the mode snapshot for the receiving PTY.
        mode = termios.tcgetattr(shell.master)
        assert not mode[3] & termios.ICANON
        termios.tcsetattr(shell.new_slave, termios.TCSANOW, mode)
        os.write(shell.master, b'\x18\x18')
        records = until(lambda: (paths if len(paths := list(root.glob('initial-*.ready'))) == 2 else None))
        pids = {int(path.stem.split('-')[1]) for path in records}
        assert shell.pid in pids
        clone_pid = (pids - {shell.pid}).pop()
        before = until(lambda: observed(root, clone_pid, 'initial'))
        assert before['draft'] == draft and before['cursor'] == cursor
        clone_geometry = read_pty_geometry(shell.new_master)
        verify_fixture_geometry(before['geometry'], clone_geometry, 110, 32, clone_pid)
        assert f'PID={clone_pid} TTY={tty}' in before['state']
        # $TTY and copied shell options alone cannot prove that acquiring the
        # controlling terminal succeeded. Verify /dev/tty reaches this PTY.
        assert (root / f'initial-{clone_pid}.ctty').read_text().strip() == 'yes', \
            'clone could not open/write its controlling terminal'
        until(lambda: f'HC_CTTY_PROOF_{clone_pid}'.encode() in outputs['clone'])
        assert b'could not make ' not in outputs['clone'], \
            'zsh reported failure acquiring the controlling terminal'
        expected_state = ('kept in memory café 🦊', 'two words β', 'value λ',
                          'hc_saved_alias is an alias', 'hc_saved_function is a shell function',
                          'OPTION=on', str(root))
        assert all(value in original['state'] for value in expected_state)
        verify_fixture_state(original, before)
        partial = {
            'proofVersion': 3, 'stage': 'clone-state-verified', 'sourcePid': shell.pid, 'clonePid': clone_pid,
            'newTty': tty, 'shellMemoryVerified': True, 'unflushedHistoryVerified': True,
            'sourcePtyGeometry': source_geometry, 'initialClonePtyGeometry': clone_geometry,
            'initialShellGeometry': before['geometry'],
            'controllingTerminalVerified': True,
            'unsentDraft': draft, 'cursor': cursor, 'nativeReceiverVerified': False,
            'automaticTerminalClosureEligible': False}
        (root / 'partial-result.json').write_text(json.dumps(partial, indent=2, ensure_ascii=False))
        if clone_only:
            return partial

        # An already-created shell and PTY enter Hardcore's ordinary receiving
        # path. Only the new shell's screen starts blank; no source screen
        # preservation is asserted by this experiment.
        peer, child = socket.socketpair()
        peer.settimeout(20)
        env = {key: value for key, value in os.environ.items() if not key.startswith('HARDCORE')}
        env.update(HOME=str(home), HARDCORE_RUNTIME_DIR=str(runtime), HARDCORE_CTL=str(ctl), HARDCORE_NO_ACTIVATE='1')
        gui = subprocess.Popen([str(executable), '--adopt-fd', str(child.fileno())],
            env=env, pass_fds=(child.fileno(),), stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        os.set_blocking(gui.stdout.fileno(), False)
        child.close(); child = None
        tab = {'pid': clone_pid, 'cols': 110, 'rows': 32, 'snapshot': b'', 'osc': b'',
               'params': [0] * 32, 'marks': [], 'st': 0, 'np': 0, 'utf8_need': 0,
               'dir': str(root), 'run': '', 'hist': ''}
        send(peer, {'protocol': 1, 'tabs': [tab], 'active': 0, 'frame': '{{80, 80}, {1000, 700}}'})
        peer.sendmsg([b'F'], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [shell.new_master]))])
        ready = receive(peer, pump=drain)
        assert ready.get('ready') is True, ready
        send(peer, {'go': True})
        peer.close(); peer = None
        os.close(shell.new_master); shell.new_master = -1
        os.close(shell.new_slave); shell.new_slave = -1
        until(ctl.exists, timeout=10)
        endpoint = runtime / f'{gui.pid}.sock'
        until(endpoint.exists, timeout=10)
        info = request(endpoint, {'op': 'list'})
        assert len(info['tabs']) == 1 and info['tabs'][0]['pid'] == clone_pid
        # A read-only widget may be retried if the clone is still returning to
        # ZLE. Never retry the subsequent edit/undo-style event itself.
        deadline = time.monotonic() + 5
        while not observed(root, clone_pid, 'observed'):
            if time.monotonic() >= deadline:
                raise AssertionError('native receiver did not reach its cloned editor')
            control(ctl, 'type \x18\x16')
            time.sleep(.1)
        adopted = observed(root, clone_pid, 'observed')
        assert adopted['draft'] == draft and adopted['cursor'] == cursor
        verify_fixture_state(original, adopted)
        initial_screen = wait_screen('before-source-close', draft, cursor)
        partial.update(stage='native-draft-verified-before-source-close',
                       nativeReceiverVerified=True, nativeSnapshotExactDraft=True,
                       nativeSnapshotCursorVerified=True, nativeSnapshotCursor=initial_screen,
                       nativeShellAndPtyGeometryVerified=True,
                       sourceMasterClosed=False)
        (root / 'partial-result.json').write_text(json.dumps(partial, indent=2, ensure_ascii=False))

        # Remove the original terminal's last master. The source shell is our
        # child, so waitpid suffices; no restricted process-table API is used.
        os.close(shell.master); shell.master = -1
        until(lambda: os.waitpid(shell.pid, os.WNOHANG)[0], timeout=5)
        shell.reaped = True
        # Redraw, edit at the preserved middle cursor, then observe without
        # pressing Return or executing the deliberately dangerous draft.
        control(ctl, 'type \x0cX\x18\x16')
        expected = draft[:cursor] + 'X' + draft[cursor:]
        after = until(lambda: (value if (value := observed(root, clone_pid, 'observed')) and
                               value['draft'] == expected else None), timeout=10)
        assert after['cursor'] == cursor + 1
        verify_fixture_state(original, after)
        assert not forbidden.exists()
        screen = wait_screen('receiver', expected, after['cursor'])
        assert not forbidden.exists()
        result = {'proofVersion': 3, 'sourcePid': shell.pid, 'clonePid': clone_pid, 'hardcorePid': gui.pid,
            'newTty': tty, 'initialWindowPaintAcknowledged': True, 'sourceMasterClosed': True,
            'sourceShellExited': True, 'shellMemoryVerified': True, 'unflushedHistoryVerified': True,
            'unsentDraft': draft, 'initialCursor': cursor, 'editedDraft': expected,
            'editedCursor': after['cursor'], 'nativeInputAtRestoredCursorVerified': True,
            'nativeSnapshotExactDraft': True, 'nativeSnapshotCursorVerified': True,
            'nativeShellAndPtyGeometryVerified': True,
            'nativeDraftAndCursorVerifiedBeforeSourceClose': True,
            'beforeSourceCloseSnapshotCursor': initial_screen,
            'nativeSnapshotCursor': screen,
            'screenProofScope': 'Complete terminal cell snapshot and cursor; no exact GPU pixel proof.',
            'draftExecuted': False,
            'existingJobsTransferred': False, 'sourceScreenTransferred': False,
            'automaticTerminalClosureEligible': False}
        return result
    finally:
        cleanup_errors = []
        try:
            drain()
        except (OSError, AssertionError):
            pass  # Preserve the original failure while still cleaning up.
        for name, output in outputs.items():
            try:
                (root / ('receiver.log' if name == 'receiver' else f'{name}-pty-output.bin')).write_bytes(output)
            except OSError as error:
                cleanup_errors.append(f'write {name} log: {error}')
        for sock in (peer, child):
            if sock is not None:
                try: sock.close()
                except OSError as error: cleanup_errors.append(f'close socket: {error}')
        if gui is not None:
            try:
                if gui.poll() is None: gui.terminate()
                try: gui.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    gui.kill(); gui.wait(timeout=2)
            except (OSError, subprocess.TimeoutExpired) as error:
                cleanup_errors.append(f'reap direct GUI child: {error}')
            if gui.stdout is not None:
                try: gui.stdout.close()
                except OSError as error: cleanup_errors.append(f'close GUI pipe: {error}')
        try:
            shell.cleanup()
        except RuntimeError as error:
            cleanup_errors.append(str(error))
        if cleanup_errors:
            try: (root / 'native-cleanup-errors.json').write_text(json.dumps(cleanup_errors))
            except OSError: pass
            if sys.exc_info()[0] is None:
                raise RuntimeError('owned fixture cleanup incomplete: ' + '; '.join(cleanup_errors))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, default=Path('build/hardcore'))
    parser.add_argument('--clone-only', action='store_true',
                        help='verify shell memory and draft only; do not start the native receiver')
    args = parser.parse_args()
    if sys.platform != 'darwin': parser.error('macOS required')
    os.umask(0o077)
    root = Path(tempfile.mkdtemp(prefix='hc-zsh-native-', dir='/tmp')).resolve()
    print('Private diagnostics:', root, flush=True)
    try:
        executable = None if args.clone_only else args.executable.resolve(strict=True)
        with deadline(90):
            result = run(root, executable, clone_only=args.clone_only)
    except BaseException as error:
        try: (root / 'failure.txt').write_text(repr(error)[:12000])
        except OSError: pass
        raise
    (root / 'result.json').write_text(json.dumps(result, indent=2, ensure_ascii=False))
    print(json.dumps(result, indent=2, ensure_ascii=False), flush=True)


if __name__ == '__main__':
    main()
