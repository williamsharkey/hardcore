#!/usr/bin/env python3
"""hardcore --ensure-tab NAME -- COMMAND: one NAME tab, never two.

Run from WSL after ``make hardcore.exe``. Each window is a private copy of
hardcore.exe with its own config (HARDCORE_TEST_CONFIG), its own session
folder and its own command pipe (the pipe's name follows the test config),
in a job object that ends with it. It never touches the installed hardcore,
its config, its saved tabs or its pipe. A stand-in command logs each start.

- a cold start with nothing saved: one NAME tab
- with the window running: the request goes to it; no second NAME tab
- a restart with the NAME tab saved: that tab runs COMMAND again; no second one
- a restart without it saved: one new NAME tab
"""
import os
from pathlib import Path
import re
import select
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/ensure-tab-windows"

DRIVER = r'''
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
static DWORD owned_pid;
static HWND owned_window;
static BOOL CALLBACK find_owned(HWND window, LPARAM unused) {
    DWORD pid; wchar_t cls[32]; (void)unused;
    GetWindowThreadProcessId(window, &pid);
    if (pid == owned_pid && GetClassNameW(window, cls, 32) && !wcscmp(cls, L"hardcore")) owned_window = window;
    return TRUE;
}
static int copydata(ULONG_PTR tag, const char *text) {
    COPYDATASTRUCT data = {tag, (DWORD)strlen(text) + 1, (void *)text};
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(owned_window, WM_COPYDATA, 0, (LPARAM)&data, SMTO_ABORTIFHUNG, 15000, &result) && result;
}
/* driver EXE CONFIG FAKE_LINUX_PATH [ARGS...]: ARGS go on hardcore's command line */
int wmain(int argc, wchar_t **argv) {
    PROCESS_INFORMATION process = {0}; STARTUPINFOW startup = {sizeof(startup)};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    wchar_t command[4096], wslenv[2048] = L""; char line[4096]; HANDLE job;
    if (argc < 4) return 2;
    SetEnvironmentVariableW(L"HARDCORE_TEST_CONFIG", argv[2]);
    SetEnvironmentVariableW(L"LOCALAPPDATA", argv[2]);
    SetEnvironmentVariableW(L"HARDCORE_SESSIONS", L"1");
    SetEnvironmentVariableW(L"HARDCORE_ADOPT", NULL);
    SetEnvironmentVariableW(L"HARDCORE_RUN", NULL);
    SetEnvironmentVariableW(L"HARDCORE_TEST_PATH", argv[3]);
    SetEnvironmentVariableW(L"HARDCORE_PIPE", L"1");          /* its own pipe (the test config names it) */
    GetEnvironmentVariableW(L"WSLENV", wslenv, 1900);
    wcscat(wslenv, wslenv[0] ? L":HARDCORE_TEST_PATH" : L"HARDCORE_TEST_PATH");
    SetEnvironmentVariableW(L"WSLENV", wslenv);
    job = CreateJobObjectW(NULL, NULL);
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return 3;
    _snwprintf(command, 4096, L"\"%ls\" --no-activate", argv[1]);
    for (int a = 4; a < argc; a++) { wcscat(command, L" \""); wcscat(command, argv[a]); wcscat(command, L"\""); }
    startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_SHOWNOACTIVATE;
    if (!CreateProcessW(argv[1], command, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &startup, &process)) return 4;
    if (!AssignProcessToJobObject(job, process.hProcess)) { TerminateProcess(process.hProcess, 1); return 5; }
    owned_pid = process.dwProcessId;
    ResumeThread(process.hThread); CloseHandle(process.hThread);
    for (int i = 0; i < 400 && !owned_window; i++) {
        if (WaitForSingleObject(process.hProcess, 0) != WAIT_TIMEOUT) break;
        EnumWindows(find_owned, 0); Sleep(50);
    }
    if (!owned_window) { TerminateJobObject(job, 1); return 6; }
    printf("OK %lu\n", owned_pid); fflush(stdout);
    while (fgets(line, sizeof(line), stdin)) {
        int ok = 0; line[strcspn(line, "\r\n")] = 0;
        if (!strcmp(line, "K")) {            /* a crash: hardcore and everything under it gone at once */
            TerminateJobObject(job, 1);
            WaitForSingleObject(process.hProcess, 5000);
            puts("OK"); fflush(stdout); return 0;
        }
        if (!strcmp(line, "Q")) {            /* the window's close button; then the job ends what's left */
            PostMessageW(owned_window, WM_CLOSE, 0, 0);
            ok = WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0;
            TerminateJobObject(job, 1);
            puts(ok ? "OK" : "FAIL"); fflush(stdout); return 0;
        }
        if (!strcmp(line, "W")) {            /* the window closed by itself? */
            ok = WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0;
            if (ok) TerminateJobObject(job, 1);
            puts(ok ? "OK" : "FAIL"); fflush(stdout); if (ok) return 0; continue;
        }
        if (!strcmp(line, "A")) ok = WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT;   /* still running */
        else if (line[0] == 'C' && line[1] == ' ') ok = copydata(0x48430002, line + 2);
        else if (line[0] == 'T' && line[1] == ' ' && strlen(line) + 1 < sizeof(line)) { strcat(line, "\n"); ok = copydata(0x48430001, line + 2); }
        puts(ok ? "OK" : "FAIL"); fflush(stdout);
    }
    TerminateJobObject(job, 1);
    return 0;
}
'''

FAKE_DASH = r'''#!/bin/bash
# a stand-in for a dashboard: says it started, then waits
here=$(cd "$(dirname "$0")" && pwd)
echo "$1" >> "$here/dash.log"
exec sleep 600
'''


def winpath(path):
    return subprocess.check_output(["wslpath", "-w", str(path)], text=True).strip()


def wait_for(check, description, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = check()
        if result:
            return result
        time.sleep(.1)
    raise AssertionError(f"timed out waiting for {description}")


class App:
    def __init__(self, helper, exe, config, fake, args=()):
        self.log = open(OUT / "driver.log", "a")
        self.proc = subprocess.Popen([str(helper), winpath(exe), winpath(config), str(fake), *args],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log)
        self.read(40)

    def read(self, timeout=30):
        ready = select.select([self.proc.stdout], [], [], timeout)[0]
        assert ready, "driver did not answer"
        answer = self.proc.stdout.readline().decode().strip()
        assert answer.startswith("OK"), f"driver: {answer!r}"
        return answer

    def send(self, command, timeout=30):
        self.proc.stdin.write((command + "\n").encode())
        self.proc.stdin.flush()
        return self.read(timeout)

    def end(self, how):
        try:
            self.send(how, 20)
            self.proc.wait(timeout=10)
        finally:
            if self.proc.poll() is None:
                self.proc.kill()
            self.log.close()


def saved_tabs(config):
    tabs = []
    folder = config / "session"
    for path in sorted(folder.glob("*.txt")) if folder.exists() else []:
        if "-tab" in path.name:
            continue
        for block in path.read_text().split("[tab]")[1:]:
            tabs.append(dict(re.findall(r"^(\w+)=(.*)$", block, re.M)))
    return tabs


def tags(config):
    return sorted(t["tag"] for t in saved_tabs(config) if t.get("tag"))


def starts(fake):
    log = fake / "dash.log"
    return log.read_text().splitlines() if log.exists() else []


def run(exe):
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "driver.log").write_text("")
    source, helper = OUT / "driver.c", OUT / "driver.exe"
    source.write_text(DRIVER)
    subprocess.run([os.environ.get("CC", "x86_64-w64-mingw32-gcc"), "-O2", "-Wall", "-municode", "-o", str(helper),
                    str(source), "-luser32"], check=True)
    work = Path(tempfile.mkdtemp(prefix="run-", dir=OUT))
    copied = work / "hardcore-ensure-test.exe"
    shutil.copy2(exe, copied)
    config = work / "config"
    config.mkdir()
    (config / "config.ini").write_text("shell=\nwidth=900\nheight=600\n")
    lin = Path(tempfile.mkdtemp(prefix="hcensure-"))
    fake = lin / "bin"
    fake.mkdir()
    (fake / "fakedash").write_text(FAKE_DASH)
    (fake / "fakedash").chmod(0o755)
    dash = str(fake / "fakedash")
    ensure = ["--ensure-tab", "dash", "--", dash, "dash"]
    env = dict(os.environ, HARDCORE_TEST_CONFIG=winpath(config))
    env["WSLENV"] = (env.get("WSLENV", "") + ":HARDCORE_TEST_CONFIG").lstrip(":")

    def forward(name):                                # the same command, with the window running
        return subprocess.run([str(copied), "--ensure-tab", name, "--", dash, name], env=env,
                              capture_output=True, text=True, timeout=60)
    try:
        # 1. a cold start with nothing saved: the session (none), then one dash tab
        app = App(helper, copied, config, fake, ensure)
        wait_for(lambda: starts(fake) == ["dash"], "the dash tab's command")
        wait_for(lambda: tags(config) == ["dash"], "the dash tab saved with its name")
        print("ok   a cold start with nothing saved opens one named tab")

        # 2. with the window running: it answers; no second dash tab, another name opens one
        r = forward("dash")
        assert r.returncode == 0 and "is open" in r.stdout, (r.returncode, r.stdout, r.stderr)
        r = forward("other")
        assert r.returncode == 0, (r.returncode, r.stdout, r.stderr)
        wait_for(lambda: tags(config) == ["dash", "other"], "the other tab saved")
        time.sleep(1)
        assert starts(fake) == ["dash", "other"], starts(fake)
        print("ok   with the window running the request goes to it: no second tab of a name")

        # 3. restart (after a crash) with both saved: each runs again, none doubles
        app.end("K")
        app = App(helper, copied, config, fake, ensure)
        wait_for(lambda: len(starts(fake)) >= 4, "the restored tabs' commands")
        time.sleep(2)
        assert sorted(starts(fake)[2:]) == ["dash", "other"], starts(fake)
        wait_for(lambda: tags(config) == ["dash", "other"], "the same named tabs saved")
        print("ok   a restored named tab runs its command again and counts: no second one")

        # 4. restart without the dash tab saved: one new dash tab
        app.end("K")
        for path in (config / "session").glob("*.txt"):
            if "-tab" in path.name:
                continue
            head, *blocks = path.read_text().split("[tab]")
            path.write_text(head + "".join("[tab]" + b for b in blocks if "tag=dash\n" not in b))
        before = len(starts(fake))
        app = App(helper, copied, config, fake, ensure)
        wait_for(lambda: len(starts(fake)) >= before + 2, "the other tab and a new dash tab")
        time.sleep(2)
        assert sorted(starts(fake)[before:]) == ["dash", "other"], starts(fake)
        wait_for(lambda: tags(config) == ["dash", "other"], "one dash tab again")
        app.end("K")
        print("ok   a restart without the named tab saved opens exactly one")
    finally:
        shutil.rmtree(lin, ignore_errors=True)


if __name__ == "__main__":
    run(ROOT / "hardcore.exe")
