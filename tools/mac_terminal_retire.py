#!/usr/bin/env python3
"""Evidence checks for retiring an Apple Terminal source after a handoff.

This module never sends input, closes a window, or changes a process. A
successful decision is deliberately restricted to a single, now-empty source
window. The default full-shell policy remains unavailable: plain text and a
Claude conversation are not a transfer of a shell's live state or its PTY.
"""
import hashlib
import json
import math
import os
import re
import subprocess
import time
import uuid
from typing import NamedTuple


MAX_TEXT_BYTES = 32 * 1024 * 1024
SAMPLE_MAX_AGE = 2.0


def digest(value):
    return hashlib.sha256(json.dumps(value, ensure_ascii=False, sort_keys=True,
                                    separators=(',', ':')).encode()).hexdigest()


def _positive(value):
    return type(value) is int and value > 1


def _text(value):
    return isinstance(value, str) and len(value.encode()) <= MAX_TEXT_BYTES


def _session(value):
    try:
        return isinstance(value, str) and str(uuid.UUID(value)) == value
    except ValueError:
        return False


def _tty(value):
    return isinstance(value, str) and re.fullmatch(r'/dev/ttys[0-9]+', value) is not None


def _time(value):
    return type(value) in (int, float) and math.isfinite(value)


def _draft(value):
    # Text plus cursor is the complete supported draft format. An attachments
    # key must not silently turn an image-bearing draft into an empty draft.
    return (isinstance(value, dict) and set(value) == {'text', 'cursor'} and
            _text(value['text']) and type(value['cursor']) is int and
            0 <= value['cursor'] <= len(value['text'].encode('utf-16-le')) // 2)


def snapshot(request_id, source, scrollback, draft):
    """Bind an exact source identity to payloads actually captured from it.

    ``Birth`` fields use the C-locale lstart value from ``sample_processes``.
    Draft cursor offsets use the JavaScript/Claude UTF-16 convention.
    """
    if not isinstance(request_id, str) or not re.fullmatch(r'[0-9a-f]{32}', request_id):
        raise ValueError('invalid request ID')
    keys = {'terminalPid', 'terminalBirth', 'windowId', 'tty',
            'claudePid', 'claudeBirth', 'sessionId'}
    if not isinstance(source, dict) or set(source) != keys:
        raise ValueError('source identity is incomplete')
    if (not _positive(source['terminalPid']) or not _positive(source['claudePid']) or
            source['terminalPid'] == source['claudePid'] or
            type(source['windowId']) is not int or source['windowId'] <= 0 or
            not _tty(source['tty']) or not _session(source['sessionId']) or
            any(not isinstance(source[k], str) or not source[k].strip()
                for k in ('terminalBirth', 'claudeBirth'))):
        raise ValueError('invalid source identity')
    if not _text(scrollback) or not _draft(draft):
        raise ValueError('unsupported scrollback or draft payload')
    body = {'protocol': 1, 'requestId': request_id, 'source': dict(source),
            'payloads': {'scrollback': scrollback, 'draft': dict(draft)}}
    return {**body, 'sha256': digest(body)}


class ProcessTable(NamedTuple):
    observed_at: float
    rows: tuple


class TtyHandles(NamedTuple):
    observed_at: float
    tty: str
    pids: tuple


def parse_tty_handles(output, exit_code, tty, observed_at):
    """Parse a complete lsof -F pfn query for one exact slave device.

    An empty, successful no-match query exits 1. A caller must reject stderr
    warnings and errors before passing output here: incomplete visibility is
    not evidence that no detached process still holds this slave open.
    """
    if (not isinstance(output, str) or not _tty(tty) or not _time(observed_at) or
            exit_code not in (0, 1)):
        raise ValueError('invalid source device inventory')
    if not output and exit_code == 1:
        return TtyHandles(observed_at, tty, ())
    if not output or exit_code != 0:
        raise ValueError('incomplete source device inventory')
    pids, pid, pending_fd, matched_pid = set(), None, False, False
    for line in output.splitlines():
        if line.startswith('p') and line[1:].isdigit():
            if pending_fd or (pid is not None and not matched_pid):
                raise ValueError('truncated source device inventory')
            pid = int(line[1:])
            if pid <= 0:
                raise ValueError('invalid source device owner')
            matched_pid = False
        elif line.startswith('f') and len(line) > 1 and pid is not None and not pending_fd:
            pending_fd = True
        elif line == 'n' + tty and pending_fd:
            pids.add(pid)
            pending_fd = False
            matched_pid = True
        else:
            raise ValueError('unsupported source device inventory')
    if pending_fd or not matched_pid or not pids:
        raise ValueError('truncated source device inventory')
    return TtyHandles(observed_at, tty, tuple(sorted(pids)))


def sample_tty_handles(tty):
    """Read only: include detached jobs whose controlling TTY is absent."""
    if not _tty(tty):
        raise ValueError('invalid source TTY')
    result = subprocess.run(['/usr/sbin/lsof', '-nP', '-F', 'pfn', '--', tty],
                            capture_output=True, text=True, timeout=3,
                            env=dict(os.environ, LC_ALL='C'))
    if result.stderr.strip():
        raise ValueError('source device inventory reported incomplete visibility')
    return parse_tty_handles(result.stdout, result.returncode, tty, time.time())


def parse_process_table(output, observed_at):
    """Parse the full successful output of ps, rejecting partial/malformed rows.

    Expected columns: pid=,ppid=,tty=,lstart=. Callers must not pass a filtered
    process listing; an omitted shell or background job would defeat this gate.
    """
    if not isinstance(output, str) or not _time(observed_at):
        raise ValueError('invalid process inventory')
    rows, seen = [], set()
    for line in output.splitlines():
        parts = line.split()
        if len(parts) != 8 or not parts[0].isdigit() or not parts[1].isdigit():
            raise ValueError('incomplete process inventory')
        pid, ppid = int(parts[0]), int(parts[1])
        birth = ' '.join(parts[3:])
        # ps lstart is fixed-width English under LC_ALL=C, unlike args which
        # could contain arbitrary newlines or look like additional processes.
        if (pid <= 0 or ppid < 0 or pid in seen or
                not re.fullmatch(r'[A-Z][a-z]{2} [A-Z][a-z]{2} [0-9]{1,2} '
                                 r'[0-9]{2}:[0-9]{2}:[0-9]{2} [0-9]{4}', birth)):
            raise ValueError('invalid process identity')
        tty = parts[2]
        if tty.startswith('ttys'):
            tty = '/dev/' + tty
        rows.append({'pid': pid, 'ppid': ppid, 'tty': tty, 'birth': birth})
        seen.add(pid)
    if not rows or not any(row['pid'] == 1 for row in rows):
        raise ValueError('process inventory is empty or filtered')
    return ProcessTable(observed_at, tuple(rows))


def sample_processes():
    """Read only: obtain a complete process inventory with bounded runtime."""
    env = dict(os.environ, LC_ALL='C')
    result = subprocess.run(['/bin/ps', '-axo', 'pid=,ppid=,tty=,lstart='],
                            capture_output=True, text=True, timeout=2, env=env,
                            check=True)
    return parse_process_table(result.stdout, time.time())


def retirement_decision(captured, receipt, terminal, processes, *,
                        handles=None, scope='full-shell', now=None):
    """Return explicit unmet guarantees; never perform the close itself.

    ``receipt`` must contain the actual payloads read back in Hardcore, the
    bound snapshot SHA-256, source/destination session IDs, and an ``attached``
    state produced after a real attachment round trip. An exec attempt or a
    destination's startup-ready event does not meet this contract.

    ``terminal`` is a fresh observation with terminalPid, terminalBirth,
    windowId, tty, tabTtys and scrollback. A close implementation must revalidate
    this exact object at the action boundary and close only that one window.
    """
    reasons = []
    now = time.time() if now is None else now
    if scope == 'full-shell':
        reasons.append('The live shell, its in-memory command history, and full PTY state have not been transferred.')
    elif scope != 'claude-and-text':
        reasons.append('The requested preservation scope is unsupported.')
    try:
        source = captured['source']
        verified = snapshot(captured['requestId'], source,
                            captured['payloads']['scrollback'], captured['payloads']['draft'])
        if captured != verified:
            raise ValueError('source snapshot changed')
    except (KeyError, TypeError, ValueError, UnicodeError):
        return {'ok': False, 'reasons': reasons + ['The source snapshot is missing, unsupported, or corrupt.']}
    if not isinstance(receipt, dict):
        receipt = {}
    if (receipt.get('protocol') != 1 or receipt.get('requestId') != captured['requestId'] or
            receipt.get('sourceSha256') != captured['sha256'] or
            receipt.get('sourceSessionId') != source['sessionId']):
        reasons.append('The receiving receipt is not bound to this exact source and snapshot.')
    if receipt.get('state') != 'attached' or not _session(receipt.get('destinationSessionId')):
        reasons.append('Hardcore has not acknowledged an actual attached Claude session.')
    if receipt.get('destinationSessionId') == source['sessionId']:
        reasons.append('The destination is not a separately identified handoff session.')
    if receipt.get('payloads') != captured['payloads']:
        reasons.append('Hardcore has not read back the exact scrollback and text draft, including cursor position.')
    if not isinstance(terminal, dict):
        terminal = {}
    if any(terminal.get(key) != source[key] for key in ('terminalPid', 'terminalBirth', 'windowId', 'tty')):
        reasons.append('The source Terminal process, window, or TTY changed.')
    if terminal.get('tabTtys') != [source['tty']]:
        reasons.append('The source window is not exactly one verified tab; other tabs must remain open.')
    if terminal.get('scrollback') != captured['payloads']['scrollback']:
        reasons.append('The source scrolling buffer changed after it was copied.')
    if (not isinstance(processes, ProcessTable) or not _time(now) or
            not 0 <= now - processes.observed_at <= SAMPLE_MAX_AGE):
        reasons.append('A fresh complete process inventory is unavailable.')
        return {'ok': False, 'reasons': reasons}
    if (not _time(receipt.get('attachedAt')) or
            not 0 <= processes.observed_at - receipt['attachedAt'] <= 120):
        reasons.append('The process inventory does not follow a recent attachment acknowledgment.')
    rows = {row['pid']: row for row in processes.rows}
    terminal_row = rows.get(source['terminalPid'])
    if not terminal_row or terminal_row['birth'] != source['terminalBirth']:
        reasons.append('The original Terminal process is no longer the same live process.')
    source_row = rows.get(source['claudePid'])
    if source_row and source_row['birth'] == source['claudeBirth']:
        reasons.append('The original Claude frontend is still running.')
    occupants = [row['pid'] for row in processes.rows if row['tty'] == source['tty']]
    if occupants:
        reasons.append('Processes still use the source TTY: ' + ', '.join(map(str, occupants)) + '. The shell or jobs must be retained.')
    if (not isinstance(handles, TtyHandles) or handles.tty != source['tty'] or
            not 0 <= now - handles.observed_at <= SAMPLE_MAX_AGE or
            not _time(receipt.get('attachedAt')) or handles.observed_at < receipt['attachedAt']):
        reasons.append('A fresh complete inventory of open source-PTY handles is unavailable.')
    elif handles.pids:
        reasons.append('Processes still hold the source PTY open: ' + ', '.join(map(str, handles.pids)) + '. Detached jobs must be retained.')
    destination = receipt.get('destination', {})
    if not isinstance(destination, dict):
        destination = {}
    pids = []
    for label in ('claude', 'client', 'hardcore'):
        pid, birth = destination.get(label + 'Pid'), destination.get(label + 'Birth')
        row = rows.get(pid) if _positive(pid) else None
        if (not row or not isinstance(birth, str) or not birth or row['birth'] != birth or
                row['tty'] == source['tty']):
            reasons.append('The receiving ' + label + ' process is not independently alive with its recorded identity.')
        pids.append(pid)
    if len(set(str(pid) for pid in pids)) != 3:
        reasons.append('The Claude host, attached client, and Hardcore window are not independently identified.')
    return {'ok': not reasons, 'reasons': reasons}
