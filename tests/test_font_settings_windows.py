#!/usr/bin/env python3
"""Exercise grouped font controls and native D3D pixels in a disposable Windows app.

Run from WSL after ``make hardcore.exe``. The helper owns one process tree,
uses a private config, never moves the pointer or activates any window, and
addresses only its own HWND. Artifacts remain in build/font-settings-windows/.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import select
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/font-settings-windows"

DRIVER = r'''
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
static DWORD owned_pid;
static HWND owned_window;
static BOOL CALLBACK find_owned(HWND window, LPARAM unused) {
    DWORD pid; wchar_t cls[32]; (void)unused;
    GetWindowThreadProcessId(window, &pid);
    if (pid == owned_pid && GetClassNameW(window, cls, 32) && !wcscmp(cls, L"hardcore"))
        owned_window = window;
    return TRUE;
}
static int copydata(ULONG_PTR tag, const char *text) {
    COPYDATASTRUCT data = {tag, (DWORD)strlen(text) + 1, (void *)text};
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(owned_window, WM_COPYDATA, 0, (LPARAM)&data,
                              SMTO_ABORTIFHUNG, 15000, &result) && result;
}
int wmain(int argc, wchar_t **argv) {
    PROCESS_INFORMATION process = {0}; STARTUPINFOW startup = {sizeof(startup)};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    wchar_t command[4096]; char line[4096]; HANDLE job;
    typedef BOOL (WINAPI *DpiContext)(HANDLE);
    DpiContext dpi = (DpiContext)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (dpi) dpi((HANDLE)(INT_PTR)-4);
    if (argc != 3) return 2;
    SetEnvironmentVariableW(L"HARDCORE_TEST_CONFIG", argv[2]);
    SetEnvironmentVariableW(L"APPDATA", argv[2]);
    SetEnvironmentVariableW(L"LOCALAPPDATA", argv[2]);
    SetEnvironmentVariableW(L"USERPROFILE", argv[2]);
    SetEnvironmentVariableW(L"HARDCORE_SESSIONS", NULL);
    SetEnvironmentVariableW(L"HARDCORE_ADOPT", NULL);
    SetEnvironmentVariableW(L"HARDCORE_RUN", NULL);
    SetEnvironmentVariableW(L"HARDCORE_RELOAD_EXE", NULL);
    job = CreateJobObjectW(NULL, NULL);
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return 3;
    _snwprintf(command, 4096, L"\"%ls\" --no-activate", argv[1]);
    startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_SHOWNOACTIVATE;
    if (!CreateProcessW(argv[1], command, NULL, NULL, FALSE, CREATE_SUSPENDED,
                        NULL, NULL, &startup, &process)) { CloseHandle(job); return 4; }
    if (!AssignProcessToJobObject(job, process.hProcess)) {
        TerminateProcess(process.hProcess, 1); CloseHandle(process.hThread);
        CloseHandle(process.hProcess); CloseHandle(job); return 5;
    }
    owned_pid = process.dwProcessId;
    ResumeThread(process.hThread); CloseHandle(process.hThread);
    for (int i = 0; i < 300 && !owned_window; i++) {
        if (WaitForSingleObject(process.hProcess, 0) != WAIT_TIMEOUT) break;
        EnumWindows(find_owned, 0); Sleep(50);
    }
    if (!owned_window) { CloseHandle(job); CloseHandle(process.hProcess); return 6; }
    printf("OK %lu\n", owned_pid); fflush(stdout);
    while (fgets(line, sizeof(line), stdin)) {
        int ok = 0; line[strcspn(line, "\r\n")] = 0;
        if (!strcmp(line, "Q")) { puts("OK"); fflush(stdout); break; }
        if (line[0] == 'C' && line[1] == ' ') ok = copydata(0x48430002, line + 2);
        else if (line[0] == 'T' && line[1] == ' ' && strlen(line) + 1 < sizeof(line)) {
            strcat(line, "\n"); ok = copydata(0x48430001, line + 2);
        }
        else if (line[0] == 'S' && line[1] == ' ') ok = copydata(0x48430003, line + 2);
        else if (line[0] == 'R' && line[1] == ' ') {
            int width, height;
            if (sscanf(line + 2, "%d,%d", &width, &height) == 2)
                ok = SetWindowPos(owned_window, NULL, 0, 0, width, height,
                                  SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        puts(ok ? "OK" : "FAIL"); fflush(stdout);
    }
    PostMessageW(owned_window, WM_CLOSE, 0, 0);
    WaitForSingleObject(process.hProcess, 1000);
    CloseHandle(job); CloseHandle(process.hProcess);
    return 0;
}
'''


def winpath(path):
    return subprocess.check_output(["wslpath", "-w", str(path)], text=True).strip()


def wait_for(check, description, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = check()
        if result:
            return result
        time.sleep(.04)
    raise AssertionError(f"timed out waiting for {description}")


def bitmap_class():
    spec = importlib.util.spec_from_file_location("chrome_bitmap", ROOT / "tests/test_chrome_mac.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.Bitmap


class NativeApp:
    def __init__(self, root, exe, helper):
        self.root = root
        self.sequence = 0
        self.config = root / "config"
        self.config.mkdir()
        (self.config / "config.ini").write_text(
            "font=t\nshell=powershell\ntab_underlay=0\nwidth=1100\nheight=800\n")
        copied = root / "font-settings.exe"
        shutil.copy2(exe, copied)
        self.log = open(root / "driver.log", "w+")
        self.proc = subprocess.Popen([str(helper), winpath(copied), winpath(self.config)],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log)
        try:
            self.read()
        except BaseException:
            self.close()
            raise

    def read(self):
        ready = select.select([self.proc.stdout], [], [], 25)[0]
        assert ready, "native helper did not respond"
        answer = self.proc.stdout.readline().decode().strip()
        assert answer.startswith("OK"), f"native helper: {answer!r}"
        return answer

    def send(self, command):
        assert "\n" not in command
        self.proc.stdin.write((command + "\n").encode())
        self.proc.stdin.flush()
        return self.read()

    def state(self):
        self.sequence += 1
        path = self.root / f"state-{self.sequence}.json"
        self.send("C fontmenu " + winpath(path))
        return json.loads(path.read_text())

    def click(self, state, col, row):
        x = (state["dock"]["x"] + col) * state["cell"]["width"] + state["cell"]["width"] // 2
        y = row * state["cell"]["height"] + state["cell"]["height"] // 2
        self.send(f"C click {x},{y}")
        return self.state()

    def page(self, state, direction):
        page = state["pages"]["settings"]
        if direction < 0:
            col = 2
        else:
            counter = f'{page["offset"] // page["capacity"] + 1}/{(page["total"] + page["capacity"] - 1) // page["capacity"]}'
            col = state["dock"]["width"] - 1 - len(counter) - 3
        return self.click(state, col, page["row"])

    def find(self, key):
        state = self.state()
        while state["pages"]["settings"]["offset"]:
            state = self.page(state, -1)
        for _ in range(len(state["fonts"]) + 1):
            for button in state["font_buttons"]:
                if button["fontkey"] == key:
                    return state, button
            page = state["pages"]["settings"]
            assert page["offset"] + page["capacity"] < page["total"], f"font {key} unreachable"
            before = page["offset"]
            state = self.page(state, 1)
            assert state["pages"]["settings"]["offset"] > before
        raise AssertionError(f"font {key} paging did not terminate")

    def snapshot(self, label):
        path = OUT / f"{label}.bmp"
        path.unlink(missing_ok=True)
        self.send("S " + winpath(path))
        wait_for(lambda: path.exists() and path.stat().st_size > 54, "native D3D snapshot")
        bitmap = bitmap_class()(path)
        bitmap.png(path.with_suffix(".png"))
        return bitmap

    def close(self):
        try:
            if self.proc.poll() is None:
                self.send("Q")
                self.proc.wait(timeout=8)
        finally:
            if self.proc.poll() is None:
                self.proc.kill()
                self.proc.wait(timeout=5)
            self.proc.stdin.close()
            self.proc.stdout.close()
            self.log.close()


def run(exe):
    OUT.mkdir(parents=True, exist_ok=True)
    source = OUT / "driver.c"
    helper = OUT / "driver.exe"
    source.write_text(DRIVER)
    subprocess.run([os.environ.get("CC", "x86_64-w64-mingw32-gcc"), "-O2", "-Wall", "-Wextra",
                    "-Wno-missing-field-initializers", "-municode", "-o", str(helper), str(source), "-luser32"], check=True)
    result = {"scope": "isolated native Windows app; physical D3D snapshots; no user preferences or input changed", "fonts": []}
    # Retain the private config and response files for inspecting failures.
    root = Path(tempfile.mkdtemp(prefix="run-", dir=OUT))
    app = None
    try:
        app = NativeApp(root, exe, helper)
        ready = root / "fixture-ready.txt"
        ready_path = winpath(ready).replace("'", "''")
        app.send("T Clear-Host; Write-Host 'Hardcore bitmap font check'; Write-Host '│ build complete  21 fonts │'; "
                 "Write-Host '┌──────────┬──────────────┐'; Write-Host '│ terminal │ I l 1 ! |   │'; "
                 "Write-Host '├──────────┼──────────────┤'; Write-Host '│ status   │ all sizes    │'; "
                 "Write-Host '└──────────┴──────────────┘'; "
                 f"Set-Content -LiteralPath '{ready_path}' -Value 'ready'")
        wait_for(ready.exists, "disposable shell fixture", timeout=30)
        state = app.state()
        state = app.click(state, 3, state["pages"]["settings_row"])
        assert state["settings_open"] and state["settings_tab"] == 0
        result["scale"] = state["scale"]
        fonts = state["fonts"]
        assert len({font["fontkey"] for font in fonts}) == len(fonts)
        for font in fonts:
            state, button = app.find(font["fontkey"])
            assert button["left"] < button["right"] <= state["dock"]["width"] - 1
            state = app.click(state, button["left"], button["row"])
            assert state["fontkey"] == font["fontkey"], (font, state)
            assert state["cell"] == {"width": font["width"], "height": font["height"]}, (font, state)
            persisted = (app.config / "config.ini").read_text()
            assert f'font={font["fontkey"]}\n' in persisted
            app.send("C hover -1,-1")
            # Windows file names are case-insensitive; font keys are not.
            artifact = f'font-{ord(font["fontkey"]):02x}-{font["width"]}x{font["height"]}'
            bitmap = app.snapshot(artifact)
            assert bitmap.width > 100 and bitmap.height > state["cell"]["height"] * 4
            # The native frame must actually contain glyphs, not an empty clear.
            assert len(set(bitmap.crop(0, 0, bitmap.width, bitmap.height)[0::4])) > 2
            result["fonts"].append({**font, "selected": True, "persisted": True,
                                     "screenshot": f'{artifact}.png'})
        # Tiny heights force continuation rows and page controls to coexist.
        app.send("R 1000,220")
        state = app.state()
        seen = set()
        while state["pages"]["settings"]["offset"]:
            state = app.page(state, -1)
        original = state["fontkey"]
        capacity = state["pages"]["settings"]["capacity"]
        assert 0 < capacity < state["pages"]["settings"]["total"]
        for _ in range(len(fonts) + 1):
            page = state["pages"]["settings"]
            assert page["capacity"] == capacity
            assert state["fontkey"] == original, "page navigation selected a font"
            seen.update(button["fontkey"] for button in state["font_buttons"])
            if page["offset"] + capacity >= page["total"]:
                break
            state = app.page(state, 1)
        assert seen == {font["fontkey"] for font in fonts}, (seen, fonts)
        last = state["pages"]["settings"]["offset"]
        state = app.page(state, 1)
        assert state["pages"]["settings"]["offset"] == last
        state = app.page(state, -1)
        assert state["pages"]["settings"]["offset"] == max(0, last - capacity)
        app.snapshot("small-window-pages")
        result["pagination"] = {"fonts_reached": len(seen), "capacity": capacity, "last_offset": last}
        result["ok"] = True
    finally:
        if app:
            app.close()
        (OUT / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f'PASS: {len(result["fonts"])} font sizes selected, persisted, and rendered in native Windows screenshots; all reachable in a short window')


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=ROOT / "hardcore.exe")
    args = parser.parse_args()
    run(args.exe.resolve())
