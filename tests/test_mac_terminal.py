#!/usr/bin/env python3
"""Owned PTY metadata regressions; no Terminal UI or permissions by default.
Pass --live for the dedicated Terminal-tab integration (Automation + Accessibility).
"""
import fcntl
import json
import os
from pathlib import Path
import pty
import plistlib
import select
import shlex
import signal
import socket
import subprocess
import sys
import struct
import termios
import tempfile
import time
import unittest

from test_mac_import_resize import bounded_run

exe=str(Path('build/hardcore').resolve())

class LiveTerminalUnavailable(RuntimeError):
    pass

def apple(source):
    r=subprocess.run(['osascript','-e',source],capture_output=True,text=True,timeout=15)
    if r.returncode:
        if 'com.apple.hiservices-xpcservice' in r.stderr and 'Connection' in r.stderr:
            raise LiveTerminalUnavailable('macOS GUI/Apple Events service is unavailable from this runner; '\
                                          'Terminal automation did not execute.\n'+r.stderr.strip())
        if '-1743' in r.stderr or 'Not authorized to send Apple events' in r.stderr:
            raise LiveTerminalUnavailable('macOS denied Automation permission for Terminal.\n'+r.stderr.strip())
        raise RuntimeError(r.stderr)
    return r.stdout.strip()

def wait(fn, timeout=10):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        if fn(): return
        time.sleep(.05)
    raise AssertionError('timed out')


METADATA_HARNESS = r'''
#import <Cocoa/Cocoa.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <util.h>

/* METADATA */

static pid_t owned;
static void require(BOOL value, const char *message) {
    if (value) return;
    fprintf(stderr, "FAIL: %s\n", message);
    if (owned > 0) { kill(-owned, SIGKILL); kill(owned, SIGKILL); waitpid(owned, NULL, 0); }
    exit(1);
}
static void equal(id value, id expected, const char *message) {
    require([value isEqual:expected], message);
}
static void ready(int master) {
    char result[256] = {0}; size_t used = 0;
    for (int i = 0; i < 150; i++) {
        ssize_t n = read(master, result + used, sizeof(result) - used - 1);
        if (n > 0) used += (size_t)n;
        if (strstr(result, "READY")) return;
        usleep(20000);
    }
    require(NO, "owned worker did not become ready");
}
int main(int argc, const char **argv) {
    @autoreleasepool {
        require(argc >= 2, "test case required");
        if (!strcmp(argv[1], "--worker")) {
            signal(SIGTTOU, SIG_IGN);
            struct termios raw; require(!tcgetattr(0, &raw), "worker tty"); cfmakeraw(&raw);
            require(!tcsetattr(0, TCSANOW, &raw), "worker raw mode");
            require(!chdir(argv[2]), "worker cwd");
            write(1, "READY", 5);
            char command;
            while (read(0, &command, 1) == 1) {
                if (command == 'g') {
                    pid_t foreground = fork();
                    require(foreground >= 0, "foreground fork");
                    if (!foreground) {
                        setpgid(0, 0);
                        execl(argv[4], argv[4], "--worker", argv[3], argv[2], argv[4], NULL);
                        _exit(2);
                    }
                    setpgid(foreground, foreground);
                    require(!tcsetpgrp(0, foreground), "foreground job select");
                    int status; waitpid(foreground, &status, 0);
                    require(!tcsetpgrp(0, getpgrp()), "foreground shell restore");
                    write(1, "READY", 5);
                }
                if (command == 'c') { require(!chdir(argv[3]), "worker new cwd"); write(1, "READY", 5); }
                if (command == 'x') { execl(argv[4], argv[4], "--worker", argv[3], argv[2], argv[4], NULL); _exit(2); }
                if (command == 'q') return 0;
            }
            return 0;
        }
        if (!strcmp(argv[1], "names")) {
            equal(mac_terminal_program_name(@"node", @[@"node", @"/x/@openai/codex/bin/codex.js"]), @"codex", "node Codex");
            equal(mac_terminal_program_name(@"node", @[@"node", @"--require", @"/tmp/setup.js", @"/x/claude-code/cli.js"]), @"claude", "node Claude loader");
            equal(mac_terminal_program_name(@"2.1.300", @[@"/Users/test/.local/share/claude/versions/2.1.300"]), @"claude", "native Claude version");
            equal(mac_terminal_program_name(@"python3.14", @[@"python3.14", @"-m", @"ruffian.cli"]), @"ruffian", "Python module");
            equal(mac_terminal_program_name(@"Python", @[@"python3", @"-u", @"/project/ruffian.py"]), @"ruffian", "Python script");
            equal(mac_terminal_program_name(@"bun", @[@"bun", @"run", @"/project/agy"]), @"agy", "Bun agent");
            equal(mac_terminal_program_name(@"node", @[@"node", @"/project/grok-cli/main.js"]), @"grok", "Grok launcher");
            equal(mac_terminal_program_name(@"python3", @[@"python3", @"-c", @"print('claude')"]), @"python3", "inline code interpreted as name");
            equal(mac_terminal_program_name(@"node", @[@"node", @"--eval", @"claude()"]), @"node", "inline JS interpreted as name");
            equal(mac_terminal_program_name(@"vim", @[@"vim", @"/secret/claude"]), @"vim", "user argument interpreted as program");
            require(mac_terminal_is_shell(@"zsh") && !mac_terminal_is_shell(@"claude"), "shell classification");
            return 0;
        }
        if (!strcmp(argv[1], "validation")) {
            require(mac_terminal_valid_tty(@"/dev/ttys000"), "valid tty refused");
            for (id bad in @[@"/dev/ttys", @"/dev/tty", @"/dev/ttys3;foo", @"/tmp/ttys123", @42])
                require(!mac_terminal_valid_tty(bad), "invalid tty accepted");
            require(![mac_terminal_metadata(@"/dev/ttys999999999") count], "absent tty gets metadata");
            return 0;
        }
        if (!strcmp(argv[1], "inspect")) {
            require(argc == 3, "tty required");
            NSData *data = [NSJSONSerialization dataWithJSONObject:mac_terminal_metadata(@(argv[2])) options:0 error:nil];
            fwrite(data.bytes, 1, data.length, stdout); putchar('\n'); return 0;
        }
        require(!strcmp(argv[1], "kernel") && argc == 6, "kernel arguments");
        int master; char device[128] = {0};
        owned = forkpty(&master, device, NULL, NULL);
        require(owned >= 0, "owned forkpty");
        if (!owned) { execl(argv[2], argv[2], "--worker", argv[3], argv[4], argv[5], NULL); _exit(2); }
        fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
        ready(master);
        NSDictionary *first = mac_terminal_metadata(@(device));
        equal(first[@"cwd"], @(argv[3]), "source cwd differs from bridge parent cwd");
        equal(first[@"program"], @"claude", "foreground native Claude not identified");
        require([first[@"pid"] intValue] == owned, "source pid differs");
        require(write(master, "g", 1) == 1, "owned foreground command"); ready(master);
        NSDictionary *foreground = mac_terminal_metadata(@(device));
        equal(foreground[@"program"], @"ruffian", "foreground job changed but source name did not");
        equal(foreground[@"cwd"], @(argv[4]), "foreground job cwd ignored");
        require([foreground[@"pid"] intValue] != owned, "background controller was chosen over foreground job");
        require(write(master, "q", 1) == 1, "owned foreground exit"); ready(master);
        equal(mac_terminal_metadata(@(device))[@"program"], @"claude", "foreground group return was not refreshed");
        require(write(master, "c", 1) == 1, "owned cwd command"); ready(master);
        equal(mac_terminal_metadata(@(device))[@"cwd"], @(argv[4]), "cwd change was not refreshed");
        require(write(master, "x", 1) == 1, "owned exec command"); ready(master);
        NSDictionary *next = mac_terminal_metadata(@(device));
        equal(next[@"program"], @"ruffian", "same-PID foreground exec was not refreshed");
        equal(next[@"cwd"], @(argv[4]), "cwd after exec differs");
        require([next[@"pid"] intValue] == owned, "exec changed source identity unexpectedly");
        require(write(master, "q", 1) == 1, "owned exit command");
        int status; require(waitpid(owned, &status, 0) == owned, "worker reap"); owned = 0;
        require(WIFEXITED(status) && !WEXITSTATUS(status), "worker failed");
        require(![mac_terminal_metadata(@(device)) count], "closed job retained stale metadata");
        close(master);
        puts("PASS: owned source cwd, foreground name, cwd change, same-PID exec, exit");
    }
    return 0;
}
'''


class TerminalMetadataTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='hc-terminal-metadata-')
        cls.root = Path(cls.temp.name).resolve()
        source = Path('hardcore_mac_terminal.m').read_text()
        metadata = source[source.index('#include <libproc.h>'):source.index('static NSAppleEventDescriptor *mac_terminal_script')]
        harness = cls.root / 'metadata.m'
        harness.write_text(METADATA_HARNESS.replace('/* METADATA */', metadata))
        cls.binary = cls.root / 'metadata'
        compiled = bounded_run(['clang', '-Wall', '-Wextra', '-Werror', '-mmacosx-version-min=11.0',
                                str(harness), '-o', str(cls.binary), '-framework', 'Cocoa'], 30)
        if compiled.returncode:
            raise AssertionError(compiled.stdout + compiled.stderr)
        import shutil
        for name in ('claude', 'ruffian'):
            shutil.copy2(cls.binary, cls.root / name)
        for name in ('first', 'second'):
            (cls.root / name).mkdir()

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_case(self, *args):
        result = bounded_run([str(self.binary), *map(str, args)], 15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_launcher_names_without_titles_or_argument_leaks(self):
        self.run_case('names')

    def test_tty_validation(self):
        self.run_case('validation')

    def test_real_owned_foreground_metadata_changes(self):
        self.run_case('kernel', self.root / 'claude', self.root / 'first', self.root / 'second', self.root / 'ruffian')

    def test_discovery_exposes_source_identity_and_metadata(self):
        source = Path('hardcore_mac_terminal.m').read_text()
        self.assertIn('[session addEntriesFromDictionary:mac_terminal_metadata(tty)]', source)
        self.assertIn('@"terminal:%d:%d:%@", host, window, tty', source)

def run_live_integration():
    def info(path):
        with socket.socket(socket.AF_UNIX) as peer:
            peer.settimeout(3)
            peer.connect(str(path))
            request = plistlib.dumps({'op':'list'}, fmt=plistlib.FMT_BINARY)
            peer.sendall(struct.pack('!I', len(request)) + request)
            def exact(size):
                data = b''
                while len(data) < size:
                    chunk = peer.recv(size-len(data))
                    if not chunk:
                        raise EOFError('owned app endpoint closed')
                    data += chunk
                return data
            length = struct.unpack('!I', exact(4))[0]
            if length > 1024 * 1024:
                raise AssertionError('oversized owned app metadata')
            return plistlib.loads(exact(length))

    with tempfile.TemporaryDirectory(prefix='hc-terminal-',dir='/tmp') as temp:
        root=Path(temp).resolve()
        first_dir=root/'ruffian'; first_dir.mkdir()
        next_dir=root/'other-project'; next_dir.mkdir()
        worker=root/'claude.py'; next_worker=root/'grok.py'
        worker_source = r'''import os, select, sys, tty
from pathlib import Path
tty.setraw(0)
pid_file, input_file, request_file, first_dir, next_dir, next_worker = map(Path, sys.argv[1:])
first = not pid_file.exists()
if first:
    os.chdir(first_dir)
pid_file.write_text(str(os.getpid()))
if first:
    os.write(1, b"TERMINAL-BRIDGE-READY\r\n")
while True:
    if request_file.exists():
        command = request_file.read_text()
        request_file.unlink()
        if command == "cd":
            os.chdir(next_dir)
        elif command == "exec":
            os.execv(sys.executable, [sys.executable, str(next_worker), *sys.argv[1:]])
    readable, _, _ = select.select([0], [], [], .05)
    if not readable:
        continue
    data = os.read(0, 4096)
    if not data:
        break
    with input_file.open("ab") as out:
        out.write(data)
    os.write(1, b"ACK:" + data.hex().encode() + b"\r\n")
'''
        worker.write_text(worker_source)
        next_worker.write_text(worker_source)
        command='exec '+shlex.join([sys.executable,str(worker),str(root/'pid'),str(root/'input'),
                                   str(root/'request'),str(first_dir),str(next_dir),str(next_worker)])
        title="Hardcore bridge test " + root.name
        target=apple('tell application "Terminal"\nset t to do script '+json.dumps(command)+'\nset custom title of t to '+json.dumps(title)+'\nreturn tty of t\nend tell')
        window=apple('tell application "Terminal"\nrepeat with w in windows\nrepeat with t in tabs of w\nif custom title of t is '+json.dumps(title)+' then return id of w\nend repeat\nend repeat\nend tell')
        master,slave=pty.openpty()
        fcntl.ioctl(slave,termios.TIOCSWINSZ,struct.pack("HHHH",24,80,0,0))
        fcntl.fcntl(master,fcntl.F_SETFL,os.O_NONBLOCK)
        app=None
        output=bytearray()
        def drain():
            try:
                while True:
                    chunk=os.read(master,65536)
                    if not chunk: break
                    output.extend(chunk)
            except (BlockingIOError,OSError): pass
            return output
        try:
            wait(lambda:(root/'pid').exists())
            source_pid=int((root/'pid').read_text())
            app=subprocess.Popen([exe,'--terminal-bridge',target,window],stdin=slave,stdout=slave,stderr=slave)
            os.close(slave); slave=-1
            wait(lambda:b'TERMINAL-BRIDGE-READY' in drain() or app.poll() is not None)
            if app.poll() is not None:
                drain(); print(output.decode(errors='replace'))
                if b'permission' in output or b'automation failed' in output: sys.exit(77)
                raise AssertionError(f'bridge failed: {app.returncode}')
            os.write(master,b'hello\r\x03\x1b[A')
            wait(lambda:(root/'input').exists() and b'hello\r\x03' in (root/'input').read_bytes())
            got=(root/'input').read_bytes()
            assert b'\x1b[A' in got, got
            wait(lambda:b'ACK:' in drain())
            os.kill(source_pid,0)
            app.terminate();app.wait(timeout=5)
            os.kill(source_pid,0)
            # Exercise the public slurp command and reload its bridge while the
            # original Terminal job stays alive, using an isolated Hardcore window.
            runtime=root/'runtime'; ctl=root/'control'
            home=root/'home';(home/'Library/Application Support').mkdir(parents=True)
            env=dict(os.environ,HOME=str(home),HARDCORE_RUNTIME_DIR=str(runtime),HARDCORE_CTL=str(ctl))
            env.pop('HARDCORE_NO_ACTIVATE',None)
            r=subprocess.run([exe,'slurp','--terminal',target,'--bridge'],env=env,capture_output=True,text=True,timeout=25)
            assert r.returncode==0,(r.stdout,r.stderr)
            gui_pid=int(r.stdout.strip().split()[-1])
            try:
                wait(ctl.exists)
                def metadata_matches(folder, program):
                    tabs=[]
                    for endpoint in runtime.glob('*.sock'):
                        try:
                            tabs.extend(tab for tab in info(endpoint).get('tabs', [])
                                        if tab.get('source') == 'terminal:'+target)
                        except (OSError, EOFError):
                            continue
                    if len(tabs) != 1:
                        return False
                    tab=tabs[0]
                    return (tab.get('cwd') == str(folder) and tab.get('program') == program
                            and tab.get('label') == folder.name+':'+program+' [bridge]')
                wait(lambda:metadata_matches(first_dir,'claude'))
                def type_in_gui(text):
                    fd=os.open(ctl,os.O_WRONLY|os.O_NONBLOCK)
                    try:os.write(fd,('type '+text+'\\n\n').encode())
                    finally:os.close(fd)
                def request_worker(command):
                    pending=root/'request.tmp'
                    pending.write_text(command)
                    pending.replace(root/'request')
                type_in_gui('PUBLIC-SLURP')
                wait(lambda:b'PUBLIC-SLURP' in (root/'input').read_bytes())
                # Owned worker changes its cwd without emitting any output.
                # This proves periodic metadata refresh, not output/title parsing.
                request_worker('cd')
                wait(lambda:metadata_matches(next_dir,'claude'))
                r=subprocess.run([exe,'update','--local'],env=env,capture_output=True,text=True,timeout=25)
                assert r.returncode==0,(r.stdout,r.stderr)
                wait(lambda:metadata_matches(next_dir,'claude'))
                # Same source PID, different foreground launcher, again quiet.
                request_worker('exec')
                wait(lambda:metadata_matches(next_dir,'grok'))
                assert int((root/'pid').read_text()) == source_pid
                type_in_gui('BRIDGE-AFTER-UPDATE')
                wait(lambda:b'BRIDGE-AFTER-UPDATE' in (root/'input').read_bytes())
                os.kill(source_pid,0)
                snap=Path('build/terminal-slurp.bmp').resolve()
                snap.unlink(missing_ok=True)
                time.sleep(.2)
                fd=os.open(ctl,os.O_WRONLY|os.O_NONBLOCK)
                try:os.write(fd,('snap '+str(snap)+'\n').encode())
                finally:os.close(fd)
                wait(snap.exists)
            finally:
                for endpoint in runtime.glob('*.sock'):
                    try:os.kill(int(endpoint.stem),signal.SIGTERM)
                    except ProcessLookupError:pass
            os.kill(source_pid,0)
            print('PASS: owned Terminal screen/input, ruffian:claude label, quiet cwd/program refresh, live update, source PID survives detach')
        finally:
            Path('build/terminal-bridge-test.log').write_bytes(drain())
            if app and app.poll() is None:app.terminate();app.wait(timeout=5)
            if (root/'pid').exists():
                try:os.kill(int((root/'pid').read_text()),signal.SIGTERM)
                except ProcessLookupError:pass
            try:apple('tell application "Terminal"\nrepeat with w in windows\nrepeat with t in tabs of w\nif tty of t is '+json.dumps(target)+' and id of w is '+window+' then\nclose w\nreturn\nend if\nend repeat\nend repeat\nend tell')
            except Exception:pass
            os.close(master)
            if slave>=0:os.close(slave)


if __name__ == '__main__':
    if sys.argv[1:] == ['--live']:
        try:
            run_live_integration()
        except LiveTerminalUnavailable as error:
            print('UNAVAILABLE: '+str(error), file=sys.stderr)
            sys.exit(77)
    else:
        unittest.main()
