#!/usr/bin/env python3
"""A bounded text-only web reader for a Hardcore here shell tab.

Visible HTML text and numbered links; no JavaScript, cookies, login sharing,
form submission, or remote control strings. Press o to use the system browser.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
from html.parser import HTMLParser
import json
import multiprocessing
import os
import re
import select
import signal
import sys
import time
import unicodedata
from urllib.error import HTTPError, URLError
from urllib.parse import quote, urljoin, urlsplit, urlunsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener
import webbrowser

MAX_RESPONSE = 2 * 1024 * 1024
MAX_TEXT = 160000
MAX_LINKS = 300
MAX_URL = 4096
MAX_REDIRECTS = 5
FETCH_SECONDS = 15


def safe_text(text):
    return ''.join(c for c in text if c in '\n\t' or unicodedata.category(c) not in ('Cc', 'Cs', 'Cf'))


def normalize_url(value, base=None):
    if not isinstance(value, str) or len(value) > MAX_URL:
        raise ValueError('URL is too long')
    if any(ord(c) < 32 or ord(c) == 127 for c in value):
        raise ValueError('Control characters are not allowed in URLs')
    value = value.strip()
    if base:
        value = urljoin(base, value)
    elif '://' not in value and not re.match(r'^[a-zA-Z][a-zA-Z0-9+.-]*:', value):
        value = 'https://' + value
    try:
        parts = urlsplit(value)
        if parts.scheme.lower() not in ('http', 'https') or not parts.hostname:
            raise ValueError('Only HTTP and HTTPS pages are supported')
        if parts.username is not None or parts.password is not None:
            raise ValueError('Credentials in URLs are not supported')
        host = parts.hostname.encode('idna').decode('ascii')
        port = parts.port
        if ':' in host:
            host = '[' + host + ']'
        netloc = host + (':' + str(port) if port is not None else '')
        result = urlunsplit((parts.scheme.lower(), netloc, quote(parts.path or '/', safe="/%:@!$&'()*+,;=-._~"),
                            quote(parts.query, safe="%/:?@!$&'()*+,;=-._~"), quote(parts.fragment, safe='%-._~')))
    except (UnicodeError, ValueError) as exc:
        raise ValueError(str(exc) or 'Invalid URL') from exc
    if len(result) > MAX_URL:
        raise ValueError('URL is too long')
    return result


@dataclass
class Page:
    url: str
    title: str
    text: str
    links: list


class TextHTML(HTMLParser):
    BLOCK = frozenset(('address', 'article', 'aside', 'blockquote', 'br', 'dd', 'div', 'dl', 'dt',
                       'footer', 'h1', 'h2', 'h3', 'h4', 'h5', 'h6', 'header', 'hr', 'li', 'main',
                       'nav', 'ol', 'p', 'pre', 'section', 'table', 'tr', 'ul'))
    HIDDEN = frozenset(('head', 'title', 'script', 'style', 'template', 'noscript', 'svg'))
    VOID = frozenset(('area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input', 'link', 'meta', 'param', 'source', 'track', 'wbr'))

    def __init__(self, url):
        super().__init__(convert_charrefs=True)
        self.url = self.base = url
        self.stack = []
        self.chunks = []
        self.length = 0
        self.truncated = False
        self.links = []
        self.link_numbers = {}
        self.title = []

    def append(self, text):
        text = safe_text(text)
        remaining = MAX_TEXT - self.length
        if len(text) > remaining:
            text = text[:max(0, remaining)]
            self.truncated = True
        self.chunks.append(text)
        self.length += len(text)

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        hidden = (self.stack[-1][1] if self.stack else False) or tag in self.HIDDEN or \
            'hidden' in attrs or attrs.get('aria-hidden', '').lower() == 'true' or \
            bool(re.search(r'(?:display\s*:\s*none|visibility\s*:\s*hidden)', attrs.get('style', ''), re.I))
        link = None
        if tag == 'base' and attrs.get('href'):
            try:
                self.base = normalize_url(attrs['href'], self.url)
            except ValueError:
                pass
        if not hidden:
            if tag in self.BLOCK:
                self.append('\n' if tag in ('br', 'li', 'tr') else '\n\n')
            if tag == 'li':
                self.append('• ')
            if tag == 'img' and attrs.get('alt'):
                self.append('[Image: ' + attrs['alt'][:200] + '] ')
            if tag == 'td':
                self.append('  ')
            if tag == 'a' and attrs.get('href') and len(self.links) < MAX_LINKS and not self.truncated:
                try:
                    target = normalize_url(attrs['href'], self.base)
                    if target not in self.link_numbers:
                        self.links.append(target)
                        self.link_numbers[target] = len(self.links)
                    link = self.link_numbers[target]
                except ValueError:
                    pass
        if tag not in self.VOID:
            if len(self.stack) >= 256:
                raise ValueError('HTML nesting limit exceeded')
            self.stack.append((tag, hidden, link))

    def handle_endtag(self, tag):
        found = next((i for i in range(len(self.stack)-1, -1, -1) if self.stack[i][0] == tag), None)
        if found is None:
            return
        _, hidden, link = self.stack[found]
        del self.stack[found:]
        if not hidden:
            if link:
                self.append(f' [{link}]')
            if tag in self.BLOCK:
                self.append('\n' if tag in ('li', 'tr') else '\n\n')

    def handle_data(self, data):
        if any(entry[0] == 'title' for entry in self.stack):
            self.title.append(safe_text(data)[:512])
        if self.stack and self.stack[-1][1]:
            return
        if not any(entry[0] == 'pre' for entry in self.stack):
            data = re.sub(r'\s+', ' ', data)
            if not self.chunks or self.chunks[-1].endswith('\n'):
                data = data.lstrip()
        self.append(data)

    def page(self):
        text = ''.join(self.chunks)
        text = re.sub(r'\n{3,}', '\n\n', text).strip()
        if self.truncated:
            text += '\n\n[Page truncated at text limit]'
        title = re.sub(r'\s+', ' ', ''.join(self.title)).strip()[:200]
        return Page(self.url, title or urlsplit(self.url).hostname or 'Web', text, self.links)


class SafeRedirects(HTTPRedirectHandler):
    def __init__(self):
        self.count = 0

    def redirect_request(self, request, fp, code, message, headers, newurl):
        self.count += 1
        if self.count > MAX_REDIRECTS:
            raise ValueError('Too many redirects')
        target = normalize_url(newurl, request.full_url)
        return super().redirect_request(request, fp, code, message, headers, target)


def fetch_page(url, opener=None):
    url = normalize_url(url)
    opener = opener or build_opener(SafeRedirects())
    request = Request(url, headers={'User-Agent': 'Hardcore-text-reader/1.0',
                                   'Accept': 'text/html,application/xhtml+xml,text/plain;q=0.9',
                                   'Accept-Encoding': 'identity'})
    deadline = time.monotonic() + FETCH_SECONDS
    try:
        response = opener.open(request, timeout=5)
    except HTTPError as exc:
        exc.close()
        raise ValueError(f'HTTP {exc.code}: {exc.reason}') from exc
    with response:
        final_url = normalize_url(response.geturl())
        content_type = response.headers.get_content_type()
        if content_type not in ('text/html', 'application/xhtml+xml', 'text/plain'):
            raise ValueError(f'{content_type} needs a full browser; press o to open it')
        if response.headers.get('Content-Encoding', '').lower() not in ('', 'identity'):
            raise ValueError('Compressed response is unsupported by this text reader')
        declared = response.headers.get('Content-Length')
        if declared and declared.isdigit() and int(declared) > MAX_RESPONSE:
            raise ValueError('Page exceeds the 2 MiB download limit')
        chunks, size = [], 0
        read = getattr(response, 'read1', response.read)
        while True:
            if time.monotonic() > deadline:
                raise ValueError('Page download timed out')
            part = read(min(65536, MAX_RESPONSE + 1 - size))
            if not part:
                break
            size += len(part)
            if size > MAX_RESPONSE:
                raise ValueError('Page exceeds the 2 MiB download limit')
            chunks.append(part)
        data = b''.join(chunks)
        encoding = response.headers.get_content_charset() or 'utf-8'
        try:
            text = data.decode(encoding, errors='replace')
        except LookupError:
            text = data.decode('utf-8', errors='replace')
    if content_type == 'text/plain':
        return Page(final_url, urlsplit(final_url).hostname or 'Web', safe_text(text[:MAX_TEXT]), [])
    parser = TextHTML(final_url)
    parser.feed(text)
    parser.close()
    return parser.page()


def _fetch_worker(url, connection):
    try:
        page = fetch_page(url)
        connection.send({'page': page})
    except Exception as exc:
        connection.send({'error': safe_text(str(exc))[:300]})
    finally:
        connection.close()


def wrap_text(text, columns):
    from here_terminal import char_width
    columns = max(1, columns)
    result = []
    for source in text.expandtabs(4).splitlines() or ['']:
        line, width = '', 0
        for char in source:
            cell_width = char_width(char)
            if width + cell_width > columns:
                # Prefer a nearby word boundary without losing indentation or
                # truncating long URLs and program identifiers.
                split = line.rfind(' ')
                if split > len(line)//2 and line[:split].strip():
                    result.append(line[:split])
                    line = line[split+1:]
                    width = sum(char_width(c) for c in line)
                else:
                    result.append(line)
                    line, width = '', 0
            if cell_width <= columns:
                line += char
                width += cell_width
        result.append(line)
    return result


class WebReader:
    """Network work runs in an owned process, with a hard whole-fetch deadline."""
    def __init__(self, url, host=None):
        if host is None:
            from here import HostTerminal
            host = HostTerminal()
        self.host = host
        self.url = normalize_url(url)
        self.page = None
        self.history = []
        self.offset = 0
        self.lines = []
        self.cols, self.rows = 80, 24
        self.worker = self.connection = None
        self.deadline = 0
        self.message = ''
        self.prompt = None
        self.input = ''
        self.pending = bytearray()
        self.pasting = False
        self.stop = self.suspended = False
        self.dirty = True

    def cancel_fetch(self):
        if self.connection:
            self.connection.close()
            self.connection = None
        if self.worker:
            if self.worker.is_alive():
                self.worker.terminate()
            self.worker.join(.3)
            if self.worker.is_alive():
                self.worker.kill()
                self.worker.join(.3)
            self.worker.close()
            self.worker = None

    def navigate(self, url, remember=True):
        try:
            url = normalize_url(url)
        except ValueError as exc:
            self.message = str(exc)
            self.dirty = True
            return
        if self.page and remember:
            self.history.append((self.page, self.offset))
            self.history = self.history[-32:]
        self.cancel_fetch()
        self.url = url
        self.host.write('\x1b]2;Web: ' + safe_text(urlsplit(url).hostname or 'Web') + '\x1b\\')
        self.page = None
        self.lines = []
        self.offset = 0
        context = multiprocessing.get_context('spawn')
        self.connection, sender = context.Pipe(duplex=False)
        self.worker = context.Process(target=_fetch_worker, args=(url, sender), daemon=True)
        self.worker.start()
        sender.close()
        self.deadline = time.monotonic() + FETCH_SECONDS
        self.message = 'Loading page…'
        self.dirty = True

    def poll(self):
        if not self.worker:
            return
        if self.connection.poll():
            try:
                result = self.connection.recv()
            except (EOFError, OSError):
                result = {'error': 'Page request ended without a response'}
            self.cancel_fetch()
            if 'page' in result:
                self.page = result['page']
                self.url = self.page.url
                self.rewrap()
                self.message = f'{len(self.page.links)} links · text view; JavaScript and forms are not supported'
            else:
                self.message = result['error']
            self.dirty = True
        elif time.monotonic() >= self.deadline:
            self.cancel_fetch()
            self.message = 'Page request timed out; press u for another URL or o for your browser'
            self.dirty = True

    def rewrap(self):
        self.lines = wrap_text(self.page.text, self.cols) if self.page else []
        self.offset = min(self.offset, max(0, len(self.lines)-self.body_rows))

    @property
    def body_rows(self):
        return max(1, self.rows - 4)

    def resize(self):
        size = os.get_terminal_size(self.host.output_fd)
        columns, rows = max(2, min(500, size.columns)), max(4, min(200, size.lines))
        if (columns, rows) != (self.cols, self.rows):
            self.cols, self.rows = columns, rows
            self.rewrap()
        self.dirty = True

    def scroll(self, rows):
        self.offset = max(0, min(max(0, len(self.lines)-self.body_rows), self.offset+rows))
        self.dirty = True

    def key(self, key):
        self.dirty = True
        if key == '\x03':
            self.stop = True
            return
        if self.prompt:
            if key == '\x1b':
                self.prompt, self.input = None, ''
            elif key in ('\r', '\n'):
                action, value = self.prompt, self.input.strip()
                self.prompt, self.input = None, ''
                if action == 'URL':
                    if value:
                        self.navigate(value)
                elif value.isdigit() and self.page and 1 <= int(value) <= len(self.page.links):
                    self.navigate(self.page.links[int(value)-1])
                else:
                    self.message = 'Enter a link number from this page'
            elif key in ('\x7f', '\b'):
                self.input = self.input[:-1]
            elif key == '\x15':
                self.input = ''
            elif len(self.input) < MAX_URL and key.isprintable():
                self.input += key
            return
        if key == 'q':
            self.stop = True
        elif key in ('j', '\x1b[B'):
            self.scroll(1)
        elif key in ('k', '\x1b[A'):
            self.scroll(-1)
        elif key in (' ', '\x1b[6~'):
            self.scroll(self.body_rows)
        elif key in ('\x1b[5~', '\x02'):
            self.scroll(-self.body_rows)
        elif key in ('g', '\x1b[H', '\x1b[1~'):
            self.scroll(-len(self.lines))
        elif key in ('G', '\x1b[F', '\x1b[4~'):
            self.scroll(len(self.lines))
        elif key == 'u':
            self.prompt, self.input = 'URL', ''
        elif key == 'b':
            if self.history:
                self.cancel_fetch()
                self.page, self.offset = self.history.pop()
                self.url = self.page.url
                self.rewrap()
                self.message = 'Previous page'
        elif key == 'r':
            self.navigate(self.url, remember=False)
        elif key == 'o':
            # Explicit user action only. Browser runs normally outside this
            # terminal and keeps its own authentication/password-manager state.
            try:
                opened = webbrowser.open(self.url, new=2)
                self.message = 'Opened in system browser' if opened else 'Could not open a system browser'
            except Exception:
                self.message = 'Could not open a system browser'
        elif key.isascii() and key.isdigit():
            self.prompt, self.input = 'Link', key

    def input_bytes(self, data):
        self.pending.extend(data)
        # No bytes from terminal responses, mouse reports, or bracketed-paste
        # framing are interpreted as commands. Pasted commands never execute.
        while self.pending:
            if self.pending.startswith(b'\x1b[200~'):
                del self.pending[:6]
                self.pasting = True
                continue
            if self.pending.startswith(b'\x1b[201~'):
                del self.pending[:6]
                self.pasting = False
                continue
            if self.pending[0] == 27:
                if len(self.pending) == 1:
                    break
                if self.pending[1] == ord('['):
                    match = re.match(br'\x1b\[[0-?]*[ -/]*[@-~]', self.pending)
                    if not match:
                        if len(self.pending) > 128:
                            self.pending.clear()
                        break
                    sequence = match[0]
                    del self.pending[:len(sequence)]
                    mouse = re.fullmatch(br'\x1b\[<(\d+);(\d+);(\d+)[Mm]', sequence)
                    if mouse:
                        button = int(mouse[1])
                        if button in (64, 65):
                            self.scroll(-3 if button == 64 else 3)
                    elif not self.pasting:
                        self.key(sequence.decode('ascii'))
                    continue
                del self.pending[0]
                if not self.pasting:
                    self.key('\x1b')
                continue
            first = self.pending[0]
            count = 1 if first < 128 else 2 if 0xc2 <= first <= 0xdf else 3 if 0xe0 <= first <= 0xef else 4 if 0xf0 <= first <= 0xf4 else 1
            if len(self.pending) < count:
                break
            char = bytes(self.pending[:count]).decode('utf-8', 'replace')
            del self.pending[:count]
            if self.pasting:
                if self.prompt == 'URL' and char.isprintable() and len(self.input) < MAX_URL:
                    self.input += char
                    self.dirty = True
            else:
                self.key(char)

    def paint(self):
        from here_terminal import char_width, clean_text
        title = self.page.title if self.page else 'Web'
        lines = [('\x1b[1;7m', 'Hardcore web · ' + title), ('\x1b[0;36m', self.url)]
        lines += [('\x1b[0m', self.lines[i] if i < len(self.lines) else '')
                  for i in range(self.offset, self.offset+self.body_rows)]
        status = self.message
        if self.page:
            status = f'{self.offset+1}/{max(1,len(self.lines))} · ' + status
        lines.append(('\x1b[0;2m', status))
        prompt = f'{self.prompt}: {self.input}' if self.prompt else 'j/k scroll · Space page · number↵ link · b back · u URL · o browser · q close'
        lines.append(('\x1b[0;7m', prompt))
        output = ['\x1b[?25l']
        for row, (style, text) in enumerate(lines[:self.rows]):
            text = clean_text(safe_text(text), self.cols)
            padding = ' ' * (self.cols-sum(char_width(c) for c in text))
            output.append(f'\x1b[{row+1};1H{style}{text}{padding}')
        output.append('\x1b[0m')
        self.host.write(''.join(output))
        self.dirty = False

    def run(self):
        old_signals = {}
        try:
            for signum in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
                old_signals[signum] = signal.signal(signum, lambda *_: setattr(self, 'stop', True))
            old_signals[signal.SIGWINCH] = signal.signal(signal.SIGWINCH, lambda *_: setattr(self, 'dirty', True))
            old_signals[signal.SIGTSTP] = signal.signal(signal.SIGTSTP, lambda *_: setattr(self, 'suspended', True))
            self.host.enter()
            self.resize()
            self.navigate(self.url)
            while not self.stop:
                if self.suspended:
                    self.host.restore()
                    os.kill(os.getpid(), signal.SIGSTOP)
                    self.host.enter()
                    self.suspended = False
                    self.dirty = True
                self.poll()
                if self.dirty:
                    self.resize()
                    self.paint()
                ready, _, _ = select.select([self.host.input_fd], [], [], .08)
                if ready:
                    data = os.read(self.host.input_fd, 4096)
                    if not data:
                        break
                    self.input_bytes(data)
                elif self.pending == b'\x1b':
                    self.pending.clear()
                    self.key('\x1b')
        finally:
            self.host.restore()
            self.cancel_fetch()
            for signum, handler in old_signals.items():
                signal.signal(signum, handler)
        return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('url', nargs='?', default='https://www.wikipedia.org/')
    args = parser.parse_args(argv)
    if not os.isatty(0) or not os.isatty(1):
        parser.exit(2, 'Hardcore web needs an interactive terminal.\n')
    try:
        return WebReader(args.url).run()
    except (ValueError, OSError) as exc:
        print(f'Hardcore web: {safe_text(str(exc))}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
