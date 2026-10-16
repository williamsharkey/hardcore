#!/usr/bin/env python3
"""Hardcore Files: a small keyboard-driven file browser, using only Python.

Run: python3 files.py [DIRECTORY]
Arrows/j/k navigate; Enter opens; Left/Backspace goes to the parent; b goes
back; ] goes forward; g chooses a folder; / filters; . shows hidden files;
s/S changes sort/direction; p previews text; Space uses macOS Quick Look;
o opens in the default app; n creates a folder; r renames; c copies; m moves;
t/Delete moves to Trash; R refreshes; ? shows help; q closes Files.

Copy/move never replace existing destinations. Move and Trash use atomic
no-replace renames; cross-volume moves are refused without changing source.
Trash uses ~/.Trash with a unique name, never permanent deletion. Symlinks are
copied/moved as links, and previews are bounded to 64 KiB of regular files.
"""
from __future__ import annotations

import argparse
import ctypes
import curses
from dataclasses import dataclass
import datetime as dt
import errno
import locale
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import unicodedata
import uuid


PREVIEW_LIMIT = 64 * 1024
SORTS = ("name", "size", "modified", "kind")


def absolute(path, base=None):
    path = Path(path).expanduser()
    return Path(os.path.abspath(path if path.is_absolute() else Path(base or Path.cwd()) / path))


def exists(path):
    """A broken symlink is still an existing destination."""
    return os.path.lexists(path)


def safe_text(value):
    result = []
    for char in str(value):
        code = ord(char)
        if unicodedata.category(char) in ("Cc", "Cs", "Cf"):
            escaped = f"\\x{code:02x}" if code <= 255 else f"\\u{code:04x}"
            result.append({10: r"\n", 13: r"\r", 9: r"\t"}.get(code, escaped))
        else:
            result.append(char)
    return "".join(result)


def width(text):
    return sum(0 if unicodedata.combining(c) else 2 if unicodedata.east_asian_width(c) in "WF" else 1 for c in text)


def clipped(text, columns):
    text = safe_text(text)
    if columns <= 0:
        return ""
    if width(text) <= columns:
        return text
    result, used = [], 0
    for char in text:
        size = width(char)
        if used + size > columns - 1:
            break
        result.append(char)
        used += size
    return "".join(result) + "…"


def wrapped_lines(lines, columns):
    """Wrap confirmation paths without hiding any part of a long filename."""
    columns = max(2, columns)
    result = []
    for line in lines:
        part, used = [], 0
        for char in safe_text(line):
            size = width(char)
            if part and used + size > columns:
                result.append("".join(part))
                part, used = [], 0
            part.append(char)
            used += size
        result.append("".join(part))
    return result


@dataclass(frozen=True)
class Entry:
    path: Path
    name: str
    is_dir: bool
    is_link: bool
    size: int
    modified: float
    kind: str


def list_entries(directory, hidden=False, query="", sort="name", reverse=False):
    if sort not in SORTS:
        raise ValueError("Unknown sort order")
    result = []
    with os.scandir(directory) as entries:
        for item in entries:
            if not hidden and item.name.startswith("."):
                continue
            if query.casefold() not in item.name.casefold():
                continue
            try:
                info = item.stat(follow_symlinks=False)
                link = stat.S_ISLNK(info.st_mode)
                folder = item.is_dir(follow_symlinks=True)
            except FileNotFoundError:
                continue  # A background program removed it during enumeration.
            except OSError:
                info, link, folder = None, item.is_symlink(), False
            result.append(Entry(Path(directory) / item.name, item.name, folder, link,
                                info.st_size if info else 0, info.st_mtime if info else 0,
                                "folder" if folder else Path(item.name).suffix.casefold()))
    def key(entry):
        primary = {"name": entry.name.casefold(), "size": entry.size,
                   "modified": entry.modified, "kind": entry.kind}[sort]
        return primary, entry.name.casefold(), entry.name
    # Folder grouping stays stable even when the requested order reverses.
    return sorted((e for e in result if e.is_dir), key=key, reverse=reverse) + sorted(
        (e for e in result if not e.is_dir), key=key, reverse=reverse)


def valid_name(name):
    if not name or name in (".", "..") or "/" in name or "\0" in name or any(
            unicodedata.category(c) in ("Cc", "Cs") for c in name):
        raise ValueError("Use a filename, without slashes or control characters")
    return name


def destination_for(source, value, base):
    target = absolute(value, base)
    return target / Path(source).name if target.is_dir() else target


def suggested_copy(source):
    source = absolute(source)
    suffix = source.suffix if not source.is_dir() else ""
    stem = source.name[:-len(suffix)] if suffix else source.name
    for number in range(1, 10000):
        name = f"{stem} copy{(' ' + str(number)) if number > 1 else ''}{suffix}"
        candidate = source.with_name(name)
        if not exists(candidate):
            return candidate
    raise FileExistsError("Choose a new destination name")


def check_destination(source, destination):
    source, destination = absolute(source), absolute(destination)
    info = source.lstat()
    if not (stat.S_ISREG(info.st_mode) or stat.S_ISDIR(info.st_mode) or stat.S_ISLNK(info.st_mode)):
        raise ValueError("Only regular files, folders, and symbolic links can be copied or moved")
    if exists(destination):
        raise FileExistsError(errno.EEXIST, "Destination already exists; nothing was replaced", str(destination))
    if not destination.parent.is_dir():
        raise FileNotFoundError(errno.ENOENT, "Destination folder does not exist", str(destination.parent))
    if stat.S_ISDIR(info.st_mode):
        original = source.resolve()
        target = destination.parent.resolve() / destination.name
        if target == original or original in target.parents:
            raise ValueError("A folder cannot be copied or moved into itself")
    return source, destination, info


def copy_path(source, destination):
    source, destination, info = check_destination(source, destination)
    if stat.S_ISLNK(info.st_mode):
        os.symlink(os.readlink(source), destination)
    elif stat.S_ISDIR(info.st_mode):
        # copytree creates the root exclusively and preserves symlinks.
        # On failure, retain the partial copy for inspection; never delete
        # data that might have been created by another process in that tree.
        shutil.copytree(source, destination, symlinks=True)
    else:
        with source.open("rb") as reader, destination.open("xb") as writer:
            shutil.copyfileobj(reader, writer)
        shutil.copystat(source, destination, follow_symlinks=False)
    return destination


def move_path(source, destination):
    source, destination, _ = check_destination(source, destination)
    libc = ctypes.CDLL(None, use_errno=True)
    if sys.platform == "darwin":
        rename = libc.renamex_np
        rename.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]
        rename.restype = ctypes.c_int
        result = rename(os.fsencode(source), os.fsencode(destination), 0x4)  # RENAME_EXCL
    elif sys.platform.startswith("linux") and hasattr(libc, "renameat2"):
        rename = libc.renameat2
        rename.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
        rename.restype = ctypes.c_int
        result = rename(-100, os.fsencode(source), -100, os.fsencode(destination), 1)  # RENAME_NOREPLACE
    else:
        raise OSError(errno.ENOTSUP, "This system cannot guarantee a move without replacing files")
    if result:
        error = ctypes.get_errno()
        if error == errno.EXDEV:
            raise OSError(error, "Move across volumes is unavailable. Copy, then Trash the original")
        raise OSError(error, os.strerror(error), str(destination))
    return destination


def rename_path(source, name):
    source = absolute(source)
    return move_path(source, source.with_name(valid_name(name)))


def create_folder(directory, name):
    target = absolute(directory) / valid_name(name)
    target.mkdir()
    return target


def trash_path(source, trash_root=None):
    source = absolute(source)
    trash = absolute(trash_root if trash_root is not None else Path.home() / ".Trash")
    trash.mkdir(mode=0o700, parents=True, exist_ok=True)
    if source == trash or trash.resolve() == source.resolve():
        raise ValueError("The Trash itself cannot be moved to Trash")
    for attempt in range(10):
        if attempt:
            suffix = source.suffix if not source.is_dir() else ""
            stem = source.name[:-len(suffix)] if suffix else source.name
            name = f"{stem} ({dt.datetime.now():%Y%m%d-%H%M%S}-{uuid.uuid4().hex[:8]}){suffix}"
        else:
            name = source.name
        try:
            return move_path(source, trash / name)
        except FileExistsError:
            continue
        except OSError as error:
            if error.errno == errno.EXDEV:
                raise OSError(error.errno, "This item is on another volume. Move it to Trash in Finder; the original is unchanged") from error
            raise
    raise FileExistsError("Could not find an unused Trash name; original retained")


def read_preview(path, limit=PREVIEW_LIMIT):
    path = Path(path)
    # O_NONBLOCK prevents a replaced path or FIFO from blocking the browser.
    with os.fdopen(os.open(path, os.O_RDONLY | os.O_NONBLOCK), "rb") as stream:
        if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):
            raise ValueError("Text preview is available for regular files; use Space for Quick Look")
        raw = stream.read(limit + 1)
    truncated = len(raw) > limit
    raw = raw[:limit]
    if b"\0" in raw:
        text = "Binary file — use Space for Quick Look or o to open.\n\n" + " ".join(f"{b:02x}" for b in raw[:128])
    else:
        text = raw.decode("utf-8", errors="replace")
    # Strip terminal controls from file contents before any curses drawing.
    lines = [safe_text(line) for line in text.splitlines()]
    if truncated:
        lines += ["", f"[Preview limited to {limit // 1024} KiB]"]
    return lines or ["[Empty file]"]


def open_external(path, preview=False):
    if sys.platform != "darwin":
        if preview:
            raise ValueError("Quick Look is available on macOS; press p for text preview")
        command = ["xdg-open", str(absolute(path))]
    else:
        command = ["qlmanage", "-p", str(absolute(path))] if preview else ["open", str(absolute(path))]
    subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL, start_new_session=True)


class Browser:
    def __init__(self, directory):
        self.directory = absolute(directory)
        self.hidden, self.query, self.sort, self.reverse = False, "", "name", False
        self.entries, self.selected, self.scroll = [], 0, 0
        self.back, self.forward, self.positions = [], [], {}
        self.message = "? Help · Changes ask before proceeding · Trash never permanently deletes"
        self.refresh()

    def current(self):
        return self.entries[self.selected] if self.entries else None

    def refresh(self, select=None):
        previous = self.current().name if self.current() else None
        self.entries = list_entries(self.directory, self.hidden, self.query, self.sort, self.reverse)
        wanted = select or previous
        self.selected = next((i for i, e in enumerate(self.entries) if e.name == wanted),
                             min(self.selected, max(0, len(self.entries)-1)))

    def navigate(self, target, history=True):
        target = absolute(target, self.directory)
        # Verify access first: failed navigation must not alter the current list/history.
        listing = list_entries(target, self.hidden, "", self.sort, self.reverse)
        if history and target != self.directory:
            self.back.append(self.directory)
            self.forward.clear()
        self.positions[self.directory] = self.current().name if self.current() else None
        self.directory, self.query, self.entries, self.scroll = target, "", listing, 0
        wanted = self.positions.get(target)
        self.selected = next((i for i, e in enumerate(listing) if e.name == wanted), 0)
        self.message = ""

    def travel(self, forward=False):
        origin, other = (self.forward, self.back) if forward else (self.back, self.forward)
        if origin:
            target, previous = origin[-1], self.directory
            self.navigate(target, history=False)
            origin.pop()
            other.append(previous)


HELP = [
    "Move around", "  ↑/↓ or j/k  Select       Enter/→  Open folder or file",
    "  ←/Backspace  Parent      b / ]    Back / Forward",
    "  g  Go to folder         /  Filter names     .  Hidden files",
    "  s  Sort by name/size/modified/kind          S  Reverse sort",
    "", "Work with the selected item", "  p  Text preview         Space  Quick Look (macOS)",
    "  o  Open in default app  r  Rename          n  New folder",
    "  c  Copy                 m  Move            t/Delete  Trash",
    "", "Copy and move accept an existing folder or a complete new filename.",
    "Existing files are never replaced. Rename, copy, move, and Trash ask first.",
    "Trash keeps the item in ~/.Trash; drag it back out using Finder to restore.",
    "Cross-volume moves are refused; copy first, then Trash the original.",
    "For Trash on another volume, use Finder; Files leaves the original untouched.",
    "Symlinks are copied/moved as links. Text preview reads at most 64 KiB.",
    "", "R  Refresh   ?  Help   q/Esc  Close Files", "", "Press q or Escape to return."
]


class Interface:
    def __init__(self, screen, browser):
        self.screen, self.browser = screen, browser
        screen.keypad(True)
        try:
            curses.curs_set(0)
            curses.set_escdelay(30)
        except (curses.error, AttributeError):
            pass
        self.accent = curses.A_BOLD
        if curses.has_colors():
            curses.start_color()
            try:
                curses.use_default_colors()
                curses.init_pair(1, curses.COLOR_CYAN, -1)
                self.accent |= curses.color_pair(1)
            except curses.error:
                pass

    def draw_text(self, y, x, text, attr=0, columns=None):
        height, columns_total = self.screen.getmaxyx()
        if y < 0 or y >= height or x < 0 or x >= columns_total:
            return
        available = max(0, columns_total-x-1)
        if columns is not None:
            available = min(available, columns)
        try:
            self.screen.addstr(y, x, clipped(text, available), attr)
        except curses.error:
            pass

    def render(self):
        self.screen.erase()
        height, columns = self.screen.getmaxyx()
        if height < 9 or columns < 30:
            self.draw_text(0, 0, "Files needs a larger window. q closes.")
            self.screen.refresh()
            return
        browser = self.browser
        self.draw_text(0, 1, "Files", self.accent)
        self.draw_text(0, 9, browser.directory)
        self.draw_text(1, 1, f"{len(browser.entries)} items · Sort: {browser.sort}{' ↓' if browser.reverse else ' ↑'} · Hidden: {'on' if browser.hidden else 'off'}", curses.A_DIM)
        if browser.query:
            self.draw_text(2, 1, f"Filter: {browser.query}  (/ to change)", self.accent)
        else:
            self.draw_text(2, 1, "Name", curses.A_DIM)
        name_width = columns-31 if columns >= 65 else columns-2
        if columns >= 65:
            self.draw_text(2, columns-29, "Size       Modified", curses.A_DIM)
        visible = height-7
        browser.scroll = max(0, min(browser.scroll, browser.selected))
        if browser.selected >= browser.scroll+visible:
            browser.scroll = browser.selected-visible+1
        for y, entry in enumerate(browser.entries[browser.scroll:browser.scroll+visible], 3):
            index = browser.scroll+y-3
            attribute = curses.A_REVERSE if index == browser.selected else self.accent if entry.is_dir else 0
            self.draw_text(y, 0, " "*(columns-1), attribute)
            icon = "@" if entry.is_link else "/" if entry.is_dir else " "
            self.draw_text(y, 1, f"{icon} {entry.name}", attribute, name_width-1)
            if columns >= 65:
                size = "—" if entry.is_dir else human_size(entry.size)
                date = dt.datetime.fromtimestamp(entry.modified).strftime("%Y-%m-%d %H:%M") if entry.modified else "unavailable"
                self.draw_text(y, columns-29, f"{size:>8}  {date}", attribute)
        if not browser.entries:
            self.draw_text(4, 2, "No matching items" if browser.query else "This folder is empty", curses.A_DIM)
        self.draw_text(height-3, 1, browser.message)
        self.draw_text(height-2, 1, "Enter Open · p Preview · / Filter · . Hidden · s Sort", curses.A_DIM)
        self.draw_text(height-1, 1, "n Folder · r Rename · c Copy · m Move · t Trash · ? Help · q Close", curses.A_DIM)
        self.screen.refresh()

    def prompt(self, title, initial=""):
        text, cursor = initial, len(initial)
        while True:
            self.render()
            height, columns = self.screen.getmaxyx()
            row = max(0, height-3)
            self.draw_text(row, 0, " "*(columns-1), curses.A_REVERSE)
            self.draw_text(row, 1, title, self.accent)
            self.draw_text(max(0, height-2), 0, " "*(columns-1))
            # Scroll the single-line editor horizontally to keep its cursor visible.
            start = cursor
            while start and width(text[start-1:cursor]) < max(1, columns-5):
                start -= 1
            self.draw_text(height-2, 1, text[start:])
            self.draw_text(height-1, 0, " "*(columns-1))
            self.draw_text(height-1, 1, "Enter Accept · Esc Cancel · Ctrl-U Clear", curses.A_DIM)
            try:
                curses.curs_set(1)
                self.screen.move(max(0, height-2), min(columns-2, 1+width(safe_text(text[start:cursor]))))
            except curses.error:
                pass
            self.screen.refresh()
            key = self.screen.get_wch()
            if key in ("\n", "\r", curses.KEY_ENTER):
                result = text
                break
            if key == "\x1b":
                result = None
                break
            if key in (curses.KEY_BACKSPACE, "\x7f", "\b"):
                if cursor:
                    text, cursor = text[:cursor-1]+text[cursor:], cursor-1
            elif key == curses.KEY_DC:
                text = text[:cursor]+text[cursor+1:]
            elif key == curses.KEY_LEFT:
                cursor = max(0, cursor-1)
            elif key == curses.KEY_RIGHT:
                cursor = min(len(text), cursor+1)
            elif key in (curses.KEY_HOME, "\x01"):
                cursor = 0
            elif key in (curses.KEY_END, "\x05"):
                cursor = len(text)
            elif key == "\x15":
                text, cursor = "", 0
            elif isinstance(key, str) and key.isprintable():
                text = text[:cursor]+key+text[cursor:]
                cursor += len(key)
        try:
            curses.curs_set(0)
        except curses.error:
            pass
        return result

    def confirm(self, action, source, target=None):
        details = ["From:", str(source)]
        if target is not None:
            details += ["", "To:", str(target)]
        self.viewer(action, details, confirm=True)
        return self.confirmed

    def viewer(self, title, lines, confirm=False):
        top = left = 0
        self.confirmed = False
        while True:
            self.screen.erase()
            height, columns = self.screen.getmaxyx()
            self.draw_text(0, 1, title, self.accent)
            visible = max(1, height-3)
            rendered = wrapped_lines(lines, columns-3) if confirm else lines
            top = min(top, max(0, len(rendered)-visible))
            for y, line in enumerate(rendered[top:top+visible], 1):
                self.draw_text(y, 1, line[left:])
            self.draw_text(height-1, 1, "y Proceed · n/Esc Cancel · ↑↓ Scroll" if confirm else "↑↓ PgUp/PgDn Scroll · ←→ Pan · q/Esc Return", curses.A_DIM)
            self.screen.refresh()
            key = self.screen.get_wch()
            if confirm:
                if key in ("y", "Y"):
                    self.confirmed = True
                    return
                if key not in (curses.KEY_DOWN, "j", curses.KEY_UP, "k", curses.KEY_NPAGE,
                               curses.KEY_PPAGE, curses.KEY_HOME, curses.KEY_END, curses.KEY_RESIZE):
                    return
            if key in ("q", "\x1b"):
                return
            if key in (curses.KEY_DOWN, "j"):
                top = min(max(0, len(rendered)-visible), top+1)
            elif key in (curses.KEY_UP, "k"):
                top = max(0, top-1)
            elif key == curses.KEY_NPAGE:
                top = min(max(0, len(rendered)-visible), top+visible)
            elif key == curses.KEY_PPAGE:
                top = max(0, top-visible)
            elif key == curses.KEY_HOME:
                top = 0
            elif key == curses.KEY_END:
                top = max(0, len(rendered)-visible)
            elif key == curses.KEY_RIGHT:
                left += max(1, columns//3)
            elif key == curses.KEY_LEFT:
                left = max(0, left-max(1, columns//3))

    def operate(self, key):
        browser = self.browser
        entry = browser.current()
        if key == "n":
            name = self.prompt("New folder name")
            if name:
                target = create_folder(browser.directory, name)
                browser.refresh(target.name)
                browser.message = f"Created {safe_text(target.name)}"
            return
        if entry is None:
            browser.message = "Select an item first"
            return
        if key == "p":
            self.viewer(entry.name, read_preview(entry.path))
        elif key in (" ", "o"):
            open_external(entry.path, preview=key == " ")
            browser.message = f"Opened {safe_text(entry.name)}"
        elif key == "r":
            name = self.prompt("Rename to", entry.name)
            if name and name != entry.name:
                valid_name(name)
                target = entry.path.with_name(name)
                if self.confirm("Rename", entry.path, target):
                    rename_path(entry.path, name)
                    browser.refresh(name)
                    browser.message = f"Renamed to {safe_text(name)}"
        elif key in ("c", "m"):
            action = "Copy" if key == "c" else "Move"
            initial = str(suggested_copy(entry.path)) if key == "c" else str(browser.directory.parent)+"/"
            value = self.prompt(f"{action} to folder or new filename", initial)
            if value:
                target = destination_for(entry.path, value, browser.directory)
                check_destination(entry.path, target)
                if self.confirm(action, entry.path, target):
                    (copy_path if key == "c" else move_path)(entry.path, target)
                    browser.refresh(target.name if target.parent == browser.directory else None)
                    browser.message = f"{action} complete: {safe_text(target)}"
        elif key in ("t", curses.KEY_DC):
            if self.confirm("Move to Trash", entry.path):
                target = trash_path(entry.path)
                browser.refresh()
                browser.message = f"In Trash: {safe_text(target.name)} · Restore using Finder"

    def run(self):
        while True:
            self.render()
            try:
                key = self.screen.get_wch()
                browser = self.browser
                if key in ("q", "\x1b"):
                    return
                if key in (curses.KEY_DOWN, "j"):
                    browser.selected = min(max(0, len(browser.entries)-1), browser.selected+1)
                elif key in (curses.KEY_UP, "k"):
                    browser.selected = max(0, browser.selected-1)
                elif key in (curses.KEY_NPAGE, curses.KEY_PPAGE):
                    page = max(1, self.screen.getmaxyx()[0]-7)
                    browser.selected = max(0, min(len(browser.entries)-1, browser.selected+(page if key == curses.KEY_NPAGE else -page)))
                elif key == curses.KEY_HOME:
                    browser.selected = 0
                elif key == curses.KEY_END:
                    browser.selected = max(0, len(browser.entries)-1)
                elif key in ("\n", "\r", curses.KEY_ENTER, curses.KEY_RIGHT):
                    entry = browser.current()
                    if entry:
                        browser.navigate(entry.path) if entry.is_dir else open_external(entry.path)
                elif key in (curses.KEY_LEFT, curses.KEY_BACKSPACE, "\x7f", "\b", "h"):
                    browser.navigate(browser.directory.parent)
                elif key in ("b", "]"):
                    browser.travel(forward=key == "]")
                elif key == "g":
                    target = self.prompt("Go to folder", str(browser.directory)+"/")
                    if target:
                        browser.navigate(target)
                elif key == "/":
                    query = self.prompt("Filter names (empty shows all)", browser.query)
                    if query is not None:
                        browser.query = query
                        browser.refresh()
                elif key == ".":
                    browser.hidden = not browser.hidden
                    browser.refresh()
                elif key == "s":
                    browser.sort = SORTS[(SORTS.index(browser.sort)+1) % len(SORTS)]
                    browser.refresh()
                elif key == "S":
                    browser.reverse = not browser.reverse
                    browser.refresh()
                elif key == "R":
                    browser.refresh()
                    browser.message = "Refreshed"
                elif key == "?":
                    self.viewer("Files · Keyboard guide", HELP)
                elif key in ("n", "p", " ", "o", "r", "c", "m", "t", curses.KEY_DC):
                    self.operate(key)
            except (OSError, ValueError, curses.error) as error:
                self.browser.message = f"Could not complete: {safe_text(error)}"


def human_size(size):
    amount = float(size)
    for unit in ("B", "K", "M", "G", "T"):
        if amount < 1024 or unit == "T":
            return f"{int(amount)}{unit}" if unit == "B" else f"{amount:.1f}{unit}"
        amount /= 1024


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("directory", nargs="?", default=".", help="folder to browse (default: current directory)")
    args = parser.parse_args(argv)
    if not sys.stdin.isatty() or not sys.stdout.isatty():
        parser.error("Files needs an interactive terminal")
    try:
        locale.setlocale(locale.LC_ALL, "")
        browser = Browser(args.directory)
        curses.wrapper(lambda screen: Interface(screen, browser).run())
        return 0
    except (OSError, ValueError, curses.error) as error:
        print(f"Files: {safe_text(error)}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
