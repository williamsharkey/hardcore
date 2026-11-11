#!/usr/bin/env python3
"""WSL disk reclaim (design/WSL-DISK-RECLAIM.md, section 10), with no real vhdx.

Run from WSL after ``make hardcore.exe``. Everything happens in a private
config folder under build/disk-reclaim-windows/:

- hardcore disk compact on small VHDX files that hardcore makes itself
  (no admin): plain and sparse, without --yes, held open by another
  process, read-only, and killed in the middle;
- the dry run against your default distro, which must change nothing;
- the tab-column card with fake sizes (HARDCORE_DISK_FAKE) in a window that
  opens off-screen, never takes focus, and never stops the real Linux (the
  fake mode refuses to).
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_font_settings_windows as native   # the isolated window driver   # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/disk-reclaim-windows"
EXE = ROOT / "hardcore.exe"
checks = 0


def check(cond, what):
    global checks
    checks += 1
    if not cond:
        raise AssertionError(what)


def winpath(path):
    return subprocess.check_output(["wslpath", "-w", str(path)], text=True).strip()


def hc(cfg, *args, env=None, timeout=120, background=False):
    e = dict(os.environ)
    e["HARDCORE_TEST_CONFIG"] = winpath(cfg)
    names = ["HARDCORE_TEST_CONFIG"]
    for k, v in (env or {}).items():
        e[k] = v
        names.append(k)
    e["WSLENV"] = ":".join([e.get("WSLENV", "")] + names).strip(":")
    cmd = [str(EXE), "disk", *args]
    if background:
        return subprocess.Popen(cmd, env=e, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    r = subprocess.run(cmd, env=e, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=timeout)
    return r.returncode, r.stdout


def sparse(path):
    out = subprocess.run(["fsutil.exe", "sparse", "queryflag", winpath(path)], capture_output=True, text=True).stdout
    return "NOT" not in out


def fresh(name, sparse_flag):
    p = OUT / name
    p.unlink(missing_ok=True)
    rc, out = hc(CFG, "make-test-vhd", winpath(p), "64", *(["sparse"] if sparse_flag else []))
    check(rc == 0 and p.exists(), f"make-test-vhd {name}: {out}")
    return p


def log_text():
    p = CFG / "disk-reclaim.log"
    return p.read_text() if p.exists() else ""


def cli_tests():
    # plain and sparse: compact works without admin, and the sparse flag stays as it was
    for name, sp in (("plain.vhdx", False), ("sparse.vhdx", True)):
        p = fresh(name, sp)
        check(sparse(p) == sp, f"{name}: sparse flag after create")
        before = p.stat().st_mtime
        rc, out = hc(CFG, "compact", "--vhd", winpath(p))
        check(rc == 3 and "--yes" in out and p.stat().st_mtime == before, f"{name}: no --yes changes nothing: {out}")
        rc, out = hc(CFG, "compact", "--vhd", winpath(p), "--dry-run")
        check(rc == 0 and "Dry run" in out and p.stat().st_mtime == before, f"{name}: dry run: {out}")
        if sp:
            check("0x400" in out, "sparse: the plan names open flag 0x400")
        rc, out = hc(CFG, "compact", "--vhd", winpath(p), "--yes")
        check(rc == 0 and "Gave back" in out, f"{name}: compact: {out}")
        check(sparse(p) == sp, f"{name}: the sparse flag did not change")
        check("compact ok" in log_text(), f"{name}: log line")
    check(not (CFG / "disk.lock").exists(), "the lock goes with the run")

    # held open by another process: "in use", nothing changes
    p = fresh("held.vhdx", True)
    holder = hc(CFG, "hold-test-vhd", winpath(p), "8", background=True)
    try:
        line = holder.stdout.readline()
        check("holding" in line, f"holder started: {line!r}")
        before = (p.stat().st_size, p.stat().st_mtime)
        t0 = time.monotonic()
        rc, out = hc(CFG, "compact", "--vhd", winpath(p), "--yes", "--wait", "2")
        check(rc == 1 and "in use" in out, f"held file: {out}")
        check(time.monotonic() - t0 < 7, "the wait ends after --wait seconds")
        check((p.stat().st_size, p.stat().st_mtime) == before, "held file unchanged")
        check("err=32" in log_text(), "held file: sharing violation logged")
    finally:
        holder.wait(timeout=20)

    # read-only: a clean failure, the lock and the run marker cleared
    p = fresh("readonly.vhdx", False)
    subprocess.run(["attrib.exe", "+R", winpath(p)], check=True, capture_output=True)
    try:
        rc, out = hc(CFG, "compact", "--vhd", winpath(p), "--yes", "--wait", "1")
        check(rc == 1 and "Could not give space back" in out, f"read-only: {out}")
        check(not (CFG / "disk.lock").exists(), "read-only: lock released")
        state = json.loads((CFG / "disk-state.json").read_text())
        check(state.get("running_since", "") == "", "read-only: no run left open")
    finally:
        subprocess.run(["attrib.exe", "-R", winpath(p)], capture_output=True)

    # killed in the middle: the next start says the run stopped early
    p = fresh("killed.vhdx", True)
    run = hc(CFG, "compact", "--vhd", winpath(p), "--yes", env={"HARDCORE_DISK_TEST_PAUSE": "15000"}, background=True)
    try:
        native.wait_for(lambda: (CFG / "disk.lock").exists() and (CFG / "disk.lock").read_text().strip(), "the lock file")
        pid = (CFG / "disk.lock").read_text().strip()
        subprocess.run(["taskkill.exe", "/F", "/PID", pid], capture_output=True)   # our own child, by its Windows PID
        run.wait(timeout=20)
    finally:
        if run.poll() is None:
            run.kill()
    native.wait_for(lambda: not (CFG / "disk.lock").exists(), "the lock goes when its process dies")
    rc, out = hc(CFG, "status", env={"HARDCORE_RECLAIM": "off"})
    check("stopped early" in out, f"killed run reported: {out}")
    check("mode: off" in out, "HARDCORE_RECLAIM=off shows")
    rc, out = hc(CFG, "compact", "--vhd", winpath(p), "--yes")
    check(rc == 0, f"the run again works: {out}")
    rc, out = hc(CFG, "status", env={"HARDCORE_RECLAIM": "off"})
    check("stopped early" not in out, "a finished run clears the notice")

    rc, out = hc(CFG, "log")
    check(rc == 0 and out.count("\n") >= 5, "hardcore disk log prints lines")


def dry_run_real():
    """The dry run against the default distro: prints the steps, changes nothing."""
    rc, out = hc(CFG, "compact", "--dry-run", timeout=180)
    first = out.splitlines()[0] if out else ""
    if ":" not in first or "\\" not in first:
        print("dry run: no WSL 2 distro here; skipped")
        return
    vhd = first.split(": ", 1)[1].strip()
    path = Path(subprocess.check_output(["wslpath", "-u", vhd], text=True).strip())
    before = (path.stat().st_size, path.stat().st_mtime)
    rc, out = hc(CFG, "compact", "--dry-run", timeout=180)
    check(rc in (0, 2) and "Dry run: nothing changed" in out and "Steps:" in out, f"dry run: {out}")
    check((path.stat().st_size, path.stat().st_mtime) == before, "dry run: the real disk file is unchanged")


def gui_tests():
    fake = OUT / "fake.json"
    app_root = OUT / "app"
    shutil.rmtree(app_root, ignore_errors=True)
    app_root.mkdir()
    os.environ["HARDCORE_DISK_FAKE"] = winpath(fake)
    os.environ["WSLENV"] = (os.environ.get("WSLENV", "") + ":HARDCORE_DISK_FAKE").strip(":")

    def put(**d):
        fake.write_text(json.dumps({"Debian": d}))

    put(L=268.1, A=200.9, U=165, V=1007, F=15.2)
    helper = OUT / "driver.exe"
    src = OUT / "driver.c"
    # the shared driver, but the window opens off-screen
    src.write_text(native.DRIVER.replace(
        "startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_SHOWNOACTIVATE;",
        "startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USEPOSITION; startup.wShowWindow = SW_SHOWNOACTIVATE;"
        " startup.dwX = (DWORD)-30000; startup.dwY = (DWORD)-30000;"))
    subprocess.run([os.environ.get("CC", "x86_64-w64-mingw32-gcc"), "-O2", "-municode", "-o", str(helper), str(src), "-luser32"], check=True)
    native.OUT = OUT
    app = native.NativeApp(app_root, EXE, helper)
    try:
        n = [0]

        def state():
            n[0] += 1
            p = app_root / f"disk-{n[0]}.json"
            app.send("C diskstate " + winpath(p))
            return json.loads(p.read_text())

        def key(k):
            app.send("C diskkey " + k) if k != "?" else None

        s = state()
        check(s["state"] == "low" and s["view"] == 2 and s["offer"] == 1 and not s["red"], f"low card: {s}")
        check(abs(s["slack"] - 35.9) < 0.2 and s["cap"] == 211, f"slack and cap: {s}")
        check(any(r["key"] == ord("R") for r in s["rows"]) and any(r["key"] == ord("C") for r in s["rows"]), "R and C lines")
        bmp = app.snapshot("low-card")
        check(s["top"] > 0, "the card has a place")
        key("R")
        s = state()
        check(s["phase"] == 2, f"R asks first, with what stops: {s}")
        app.snapshot("confirm")
        key("R")
        s = state()
        check(s["phase"] == 6, f"R again goes on (fake: done at once, real Linux untouched): {s}")
        app.snapshot("done")
        app.send("C diskkey esc")
        check(state()["phase"] == 0, "Esc closes the result")

        put(L=268.1, A=200.9, U=165, V=1007, F=3.5)
        s = state()
        check(s["state"] == "danger" and s["red"] == 1 and s["offer"] == 1, f"danger: {s}")
        app.snapshot("danger")

        put(L=100, A=100, U=80, V=256, F=30)
        s = state()
        check(s["state"] == "worth it" and s["view"] == 1 and s["offer"] == 1, f"worth it: a quiet line: {s}")
        app.snapshot("worth-line")
        key("N")
        s = state()
        check(s["view"] == 0, f"N hides the quiet line for 7 days: {s}")

        put(L=100, A=100, U=99, V=120, F=80)
        s = state()
        check(s["view"] == 0 and s["top"] < 0, f"fine: nothing shows: {s}")

        put(L=268.1, A=200.9, U=165, V=1007, F=15.2, phase="running", pct=42)
        s = state()
        check(s["phase"] == 5, "running card")
        app.snapshot("running")
        put(L=268.1, A=200.9, U=165, V=1007, F=15.2, phase="failed")
        check(state()["phase"] == 7, "failed card")
        app.snapshot("failed")
        put(L=268.1, A=200.9, U=165, V=1007, F=15.2, phase="early")
        check(state()["phase"] == 8, "stopped-early card")
        app.snapshot("early")
        put(L=268.1, A=200.9, U=165, V=1007, F=15.2, dumps=2.5, phase="")
        app.send("C diskkey esc")
        s = state()
        check(any(r["key"] == ord("D") for r in s["rows"]), f"crash dumps over 1 GB: a D line: {s}")
        del bmp
    finally:
        app.close()
        os.environ.pop("HARDCORE_DISK_FAKE", None)


if __name__ == "__main__":
    shutil.rmtree(OUT, ignore_errors=True)
    OUT.mkdir(parents=True)
    CFG = OUT / "cfg"
    CFG.mkdir()
    cli_tests()
    dry_run_real()
    gui_tests()
    print(f"disk reclaim (Windows): {checks} checks passed; pictures in {OUT}")
