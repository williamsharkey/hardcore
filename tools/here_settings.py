"""Themes and reversible host-font settings for terminal-hosted Hardcore.

Own-screen truecolor themes never alter the host palette. Fonts belong to the
hosting emulator: only local Terminal.app has an adapter, and it performs no
Apple events until the user explicitly enables host font changes. The adapter
addresses the originating TTY, never the selected tab or shared profile.

Protocol references:
https://invisible-island.net/xterm/ctlseqs/ctlseqs.html (OSC 4/10/11/50)
https://support.apple.com/guide/terminal/trmltxt/mac
Terminal.app's shipped Scripting Definition: tab.font name / font size / tty.
"""
from __future__ import annotations

from dataclasses import dataclass
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def rgb(value):
    return ((value >> 16) & 255, (value >> 8) & 255, value & 255)


@dataclass(frozen=True, slots=True)
class Theme:
    key: str
    name: str
    dark: bool
    bg: tuple
    fg: tuple
    cursor: tuple
    selection: tuple
    palette: tuple


# Keep these values in sync with SCHEMES in hardcore.c; a test checks all fields.
THEMES = (
    Theme('h', 'hardcore', True, rgb(0x0C0C0C), rgb(0xCCCCCC), rgb(0xFFA60D), rgb(0x264F78), tuple(map(rgb, (0x0C0C0C, 0xC50F1F, 0x13A10E, 0xC19C00, 0x0037DA, 0x881798, 0x3A96DD, 0xCCCCCC, 0x767676, 0xE74856, 0x16C60C, 0xF9F1A5, 0x3B78FF, 0xB4009E, 0x61D6D6, 0xF2F2F2)))),
    Theme('d', 'Dracula', True, rgb(0x282A36), rgb(0xF8F8F2), rgb(0xF8F8F2), rgb(0x44475A), tuple(map(rgb, (0x21222C, 0xFF5555, 0x50FA7B, 0xF1FA8C, 0xBD93F9, 0xFF79C6, 0x8BE9FD, 0xF8F8F2, 0x6272A4, 0xFF6E6E, 0x69FF94, 0xFFFFA5, 0xD6ACFF, 0xFF92DF, 0xA4FFFF, 0xFFFFFF)))),
    Theme('m', 'Monokai', True, rgb(0x272822), rgb(0xFDFFF1), rgb(0xC0C1B5), rgb(0x57584F), tuple(map(rgb, (0x272822, 0xF92672, 0xA6E22E, 0xE6DB74, 0xFD971F, 0xAE81FF, 0x66D9EF, 0xFDFFF1, 0x6E7066, 0xF92672, 0xA6E22E, 0xE6DB74, 0xFD971F, 0xAE81FF, 0x66D9EF, 0xFDFFF1)))),
    Theme('n', 'Nord', True, rgb(0x2E3440), rgb(0xD8DEE9), rgb(0xECEFF4), rgb(0x434C5E), tuple(map(rgb, (0x3B4252, 0xBF616A, 0xA3BE8C, 0xEBCB8B, 0x81A1C1, 0xB48EAD, 0x88C0D0, 0xE5E9F0, 0x596377, 0xBF616A, 0xA3BE8C, 0xEBCB8B, 0x81A1C1, 0xB48EAD, 0x8FBCBB, 0xECEFF4)))),
    Theme('g', 'Gruvbox Dark', True, rgb(0x282828), rgb(0xEBDBB2), rgb(0xEBDBB2), rgb(0x665C54), tuple(map(rgb, (0x282828, 0xCC241D, 0x98971A, 0xD79921, 0x458588, 0xB16286, 0x689D6A, 0xA89984, 0x928374, 0xFB4934, 0xB8BB26, 0xFABD2F, 0x83A598, 0xD3869B, 0x8EC07C, 0xEBDBB2)))),
    Theme('s', 'Solarized Dark', True, rgb(0x002B36), rgb(0x839496), rgb(0x839496), rgb(0x073642), tuple(map(rgb, (0x073642, 0xDC322F, 0x859900, 0xB58900, 0x268BD2, 0xD33682, 0x2AA198, 0xEEE8D5, 0x335E69, 0xCB4B16, 0x586E75, 0x657B83, 0x839496, 0x6C71C4, 0x93A1A1, 0xFDF6E3)))),
    Theme('o', 'One Dark', True, rgb(0x282C34), rgb(0xDCDFE4), rgb(0xA3B3CC), rgb(0x474E5D), tuple(map(rgb, (0x282C34, 0xE06C75, 0x98C379, 0xE5C07B, 0x61AFEF, 0xC678DD, 0x56B6C2, 0xDCDFE4, 0x5D677A, 0xE06C75, 0x98C379, 0xE5C07B, 0x61AFEF, 0xC678DD, 0x56B6C2, 0xDCDFE4)))),
    Theme('c', 'Catppuccin Mocha', True, rgb(0x1E1E2E), rgb(0xCDD6F4), rgb(0xF5E0DC), rgb(0x585B70), tuple(map(rgb, (0x45475A, 0xF38BA8, 0xA6E3A1, 0xF9E2AF, 0x89B4FA, 0xF5C2E7, 0x94E2D5, 0xBAC2DE, 0x585B70, 0xF7AEC2, 0xC2ECBF, 0xFCD682, 0xAECCFC, 0xF398DA, 0xB1EAE1, 0xA6ADC8)))),
    Theme('k', 'Tokyo Night', True, rgb(0x1A1B26), rgb(0xC0CAF5), rgb(0xC0CAF5), rgb(0x33467C), tuple(map(rgb, (0x15161E, 0xF7768E, 0x9ECE6A, 0xE0AF68, 0x7AA2F7, 0xBB9AF7, 0x7DCFFF, 0xA9B1D6, 0x414868, 0xF7768E, 0x9ECE6A, 0xE0AF68, 0x7AA2F7, 0xBB9AF7, 0x7DCFFF, 0xC0CAF5)))),
    Theme('r', 'Rose Pine', True, rgb(0x191724), rgb(0xE0DEF4), rgb(0xE0DEF4), rgb(0x403D52), tuple(map(rgb, (0x26233A, 0xEB6F92, 0x31748F, 0xF6C177, 0x9CCFD8, 0xC4A7E7, 0xEBBCBA, 0xE0DEF4, 0x6E6A86, 0xEB6F92, 0x31748F, 0xF6C177, 0x9CCFD8, 0xC4A7E7, 0xEBBCBA, 0xE0DEF4)))),
    Theme('G', 'Gruvbox Light', False, rgb(0xFBF1C7), rgb(0x3C3836), rgb(0x3C3836), rgb(0xD5C4A1), tuple(map(rgb, (0xFBF1C7, 0xCC241D, 0x98971A, 0xD79921, 0x458588, 0xB16286, 0x689D6A, 0x7C6F64, 0x928374, 0x9D0006, 0x79740E, 0xB57614, 0x076678, 0x8F3F71, 0x427B58, 0x3C3836)))),
    Theme('S', 'Solarized Light', False, rgb(0xFDF6E3), rgb(0x657B83), rgb(0x657B83), rgb(0xEEE8D5), tuple(map(rgb, (0x073642, 0xDC322F, 0x859900, 0xB58900, 0x268BD2, 0xD33682, 0x2AA198, 0xBBB5A2, 0x002B36, 0xCB4B16, 0x586E75, 0x657B83, 0x839496, 0x6C71C4, 0x93A1A1, 0xFDF6E3)))),
    Theme('O', 'One Light', False, rgb(0xFAFAFA), rgb(0x383A42), rgb(0xA5B4E5), rgb(0xBFCEFF), tuple(map(rgb, (0x383A42, 0xE45649, 0x50A14F, 0xC18401, 0x0184BC, 0xA626A4, 0x0997B3, 0xBABABA, 0x4F525E, 0xE06C75, 0x98C379, 0xD8B36E, 0x61AFEF, 0xC678DD, 0x56B6C2, 0xFFFFFF)))),
    Theme('C', 'Catppuccin Latte', False, rgb(0xEFF1F5), rgb(0x4C4F69), rgb(0xDC8A78), rgb(0xACB0BE), tuple(map(rgb, (0xBCC0CC, 0xD20F39, 0x40A02B, 0xDF8E1D, 0x1E66F5, 0xEA76CB, 0x179299, 0x5C5F77, 0xACB0BE, 0xE7103F, 0x46B02F, 0xE49931, 0x3878F6, 0xEF95D7, 0x19A1A8, 0x6C6F85)))),
    Theme('K', 'Tokyo Night Day', False, rgb(0xE1E2E7), rgb(0x3760BF), rgb(0x3760BF), rgb(0x99A7DF), tuple(map(rgb, (0xE9E9ED, 0xF52A65, 0x587539, 0x8C6C3E, 0x2E7DE9, 0x9854F1, 0x007197, 0x6172B0, 0xA1A6C5, 0xF52A65, 0x587539, 0x8C6C3E, 0x2E7DE9, 0x9854F1, 0x007197, 0x3760BF)))),
    Theme('R', 'Rose Pine Dawn', False, rgb(0xFAF4ED), rgb(0x575279), rgb(0x575279), rgb(0xDFDAD9), tuple(map(rgb, (0xF2E9E1, 0xB4637A, 0x286983, 0xEA9D34, 0x56949F, 0x907AA9, 0xD7827E, 0x575279, 0x9893A5, 0xB4637A, 0x286983, 0xEA9D34, 0x56949F, 0x907AA9, 0xD7827E, 0x575279)))),
)

THEME_BY_KEY = {theme.key: theme for theme in THEMES}
FONT_CHOICES = ('Menlo-Regular', 'Monaco', 'Courier', 'SFMonoTerminal-Regular')
DEFAULT_SETTINGS = {'theme': 'g', 'allow_host_font': False,
                    'font_name': 'Menlo-Regular', 'font_size': 14}


def theme_for(key):
    return THEME_BY_KEY.get(key, THEME_BY_KEY['g'])


def settings_path():
    if os.environ.get('XDG_CONFIG_HOME'):
        root = Path(os.environ['XDG_CONFIG_HOME'])
    elif sys.platform == 'darwin':
        root = Path.home() / 'Library/Application Support'
    else:
        root = Path.home() / '.config'
    return root / 'hardcore/here.json'


def valid_font_name(name):
    return isinstance(name, str) and 0 < len(name) <= 128 and not any(
        ord(c) < 32 or ord(c) == 127 for c in name)


def normalized_settings(settings):
    result = dict(DEFAULT_SETTINGS)
    if not isinstance(settings, dict):
        return result
    if isinstance(settings.get('theme'), str) and settings['theme'] in THEME_BY_KEY:
        result['theme'] = settings['theme']
    if type(settings.get('allow_host_font')) is bool:
        result['allow_host_font'] = settings['allow_host_font']
    if valid_font_name(settings.get('font_name')):
        result['font_name'] = settings['font_name']
    if type(settings.get('font_size')) is int and 6 <= settings['font_size'] <= 48:
        result['font_size'] = settings['font_size']
    return result


def load_settings(path=None):
    try:
        with Path(path or settings_path()).open('rb') as source:
            data = source.read(16385)
        if len(data) > 16384:
            return dict(DEFAULT_SETTINGS)
        return normalized_settings(json.loads(data))
    except (OSError, ValueError, UnicodeError):
        return dict(DEFAULT_SETTINGS)


def save_settings(settings, path=None):
    """Atomically replace only this frontend's settings, never native config.ini."""
    settings = normalized_settings(settings)
    destination = Path(path or settings_path())
    destination.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix='.here-', dir=destination.parent)
    try:
        with os.fdopen(fd, 'w', encoding='utf-8') as out:
            json.dump(settings, out, ensure_ascii=False, sort_keys=True)
            out.write('\n')
            out.flush()
            os.fsync(out.fileno())
        os.replace(temporary, destination)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    return settings


class HostFontError(RuntimeError):
    pass


# JSON is passed as argv, never interpolated into AppleScript source. Read and
# write the deprecated *tab* font properties intentionally: changing a shared
# settings set could change other tabs and the user's persisted profile.
_TERMINAL_FONT_SCRIPT = r'''
function run(argv) {
    const p = JSON.parse(argv[0]);
    const app = Application('com.apple.Terminal');
    if (!app.running()) throw new Error('Terminal is not running');
    const matches = [];
    for (const w of app.windows()) {
        for (const t of w.tabs()) {
            if (t.tty() === p.tty) matches.push({tab:t, window:w});
        }
    }
    if (matches.length !== 1) throw new Error('Originating Terminal tab not found');
    const t = matches[0].tab, windowID = matches[0].window.id();
    if (p.window_id !== undefined && p.window_id !== windowID)
        throw new Error('Originating Terminal window changed');
    function read() {
        return {tty:p.tty, window_id:windowID, name:t.fontName(), size:t.fontSize()};
    }
    const before = read();
    if (p.action === 'read') return JSON.stringify(before);
    if (p.action !== 'set') throw new Error('Unknown font operation');
    t.fontName = p.name;
    t.fontSize = p.size;
    return JSON.stringify(read());
}
'''


class HostFontSession:
    """Opt-in, per-TTY host font transaction with exact restoration.

    Call restore() in the frontend's finally block, suspend() before SIGTSTP,
    and resume() after SIGCONT. This cannot restore after SIGKILL/power loss.
    It never sends OSC font/palette commands: many emulators ignore them, and
    resetting defaults is not equivalent to restoring the user's live values.
    """
    def __init__(self, fd=0, env=None, platform=None, runner=None, tty_name=None):
        env = os.environ if env is None else env
        platform = sys.platform if platform is None else platform
        self._runner = subprocess.run if runner is None else runner
        self.original = None
        self.current = None
        self.desired = None
        self.active = False
        self._suspended = False
        self.reason = ''
        try:
            self.tty = os.ttyname(fd) if tty_name is None else tty_name
        except OSError:
            self.tty = ''
        if platform != 'darwin' or env.get('TERM_PROGRAM') != 'Apple_Terminal':
            self.reason = 'Host font control is supported in local macOS Terminal only'
        elif any(env.get(k) for k in ('SSH_TTY', 'SSH_CONNECTION', 'TMUX', 'STY')):
            self.reason = 'Host font control is unavailable through SSH or a multiplexer'
        elif not re.fullmatch(r'/dev/tty[a-zA-Z0-9]+', self.tty):
            self.reason = 'Could not identify the originating Terminal TTY'
        self.available = not self.reason

    def _call(self, action, **fields):
        if not self.available:
            raise HostFontError(self.reason)
        payload = {'action': action, 'tty': self.tty, **fields}
        try:
            result = self._runner(['/usr/bin/osascript', '-l', 'JavaScript', '-e',
                                   _TERMINAL_FONT_SCRIPT, json.dumps(payload)],
                                  stdin=subprocess.DEVNULL, capture_output=True,
                                  text=True, timeout=3)
        except (OSError, subprocess.SubprocessError) as exc:
            raise HostFontError('Terminal font request failed or timed out') from exc
        if result.returncode:
            # Error text can contain user data; keep the UI diagnostic bounded.
            detail = str(result.stderr or '').strip().replace('\n', ' ')[:240]
            raise HostFontError(detail or 'Terminal denied font control')
        try:
            state = json.loads(result.stdout)
            valid = isinstance(state, dict) and state.get('tty') == self.tty and \
                type(state.get('window_id')) is int and state['window_id'] > 0 and \
                valid_font_name(state.get('name')) and \
                type(state.get('size')) in (int, float) and \
                math.isfinite(state['size']) and 1 <= state['size'] <= 200
            if not valid:
                raise ValueError('invalid host font state')
            if self.original and state['window_id'] != self.original['window_id']:
                raise ValueError('originating window changed')
            return state
        except (ValueError, TypeError, KeyError) as exc:
            raise HostFontError('Terminal did not provide a restorable font snapshot') from exc

    def apply(self, name, size, allowed=False):
        """Read the exact original before the first write; roll back failed writes."""
        if allowed is not True:
            raise HostFontError('Enable Allow host font changes first')
        if not valid_font_name(name) or type(size) is not int or not 6 <= size <= 48:
            raise HostFontError('Choose a valid font name and size from 6 to 48')
        if not self.available:
            raise HostFontError(self.reason)
        if self.original is None:
            self.original = self._call('read')
        self.active = True  # a timed-out/partial write still needs restoration
        try:
            current = self._call('set', window_id=self.original['window_id'], name=name, size=size)
            if current['name'] != name or current['size'] != size:
                raise HostFontError('Terminal could not apply the requested font')
        except HostFontError as exc:
            try:
                self.restore()
            except HostFontError:
                raise HostFontError(f'{exc}; original font restoration is still pending') from exc
            raise
        self.current = current
        self.desired = {'name': name, 'size': size}
        self._suspended = False
        return dict(current)

    def restore(self):
        """Restore the exact saved font and size; retain snapshot on failure."""
        if not self.active or self.original is None:
            return False
        state = self._call('set', window_id=self.original['window_id'],
                           name=self.original['name'], size=self.original['size'])
        if state['name'] != self.original['name'] or state['size'] != self.original['size']:
            raise HostFontError('Terminal did not restore the original font')
        self.current = dict(state)
        self.active = False
        self.original = None
        return True

    def suspend(self):
        self._suspended = self.active
        return self.restore()

    def resume(self):
        if not self._suspended or not self.desired:
            return False
        desired = dict(self.desired)
        # Capture the host's current font again, honoring changes made while
        # suspended. It becomes the new restoration point for this activation.
        self.original = None
        self.apply(desired['name'], desired['size'], allowed=True)
        return True
