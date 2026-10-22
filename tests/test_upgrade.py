#!/usr/bin/env python3
"""hardcore upgrade: main is fetched, compared by commit, built and swapped in hot.

Part 1 (Linux only, quick) runs the upgrade script hardcore.exe embeds against a
throwaway git repository standing in for GitHub, with a Makefile whose
hardcore.exe target only copies a file: current, newer, a branch build, an
unknown build and --force each give the answer they should.

Part 2 (Windows, run from WSL after ``make hardcore.exe``) is the whole thing
with a copy of hardcore.exe "installed" in a private folder
(HARDCORE_INSTALL_EXE) and a private config (HARDCORE_TEST_CONFIG, its own
pipe): a window of that copy runs a stand-in Claude, `hardcore upgrade --force`
builds the stand-in main and swaps it in, the window reloads in place, Claude
keeps running with its session, and after a crash the session comes back by id.
It never touches the installed hardcore, its windows, its config or its tabs.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/upgrade-test"
sys.path.insert(0, str(Path(__file__).resolve().parent))


def c_string(name):
    """The bytes of a C string constant in hardcore.c."""
    src = (ROOT / "hardcore.c").read_text()
    body = src[src.index(f"static const char {name}[] ="):]
    body = body[:body.index(";\n")]
    return "".join(bytes(m.group(1), "utf-8").decode("unicode_escape")
                   for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', body))


def git(repo, *args):
    return subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True, text=True).stdout.strip()


def commit(repo, message):
    (repo / "log.txt").open("a").write(message + "\n")
    git(repo, "add", "-A")
    git(repo, "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", message)
    return git(repo, "rev-parse", "HEAD")


def origin_repo(base, exe_source):
    """A repo like GitHub's: main A-B, a branch C off A, a later D on top of B (not main)."""
    origin = base / "origin"
    origin.mkdir()
    git(origin, "init", "-q", "-b", "main")
    (origin / "Makefile").write_text(
        ".PHONY: hardcore.exe\nhardcore.exe:\n\tcp '%s' hardcore.exe\n\tgit rev-parse HEAD > built-from.txt\n" % exe_source)
    a = commit(origin, "A")
    b = commit(origin, "B")
    git(origin, "checkout", "-q", "-b", "side", a)
    c = commit(origin, "C")
    git(origin, "checkout", "-q", "-b", "later", b)
    d = commit(origin, "D")
    git(origin, "checkout", "-q", "main")
    return origin, a, b, c, d


def script_tests():
    base = Path(tempfile.mkdtemp(prefix="hcup-"))
    try:
        fake_exe = base / "new.exe"
        fake_exe.write_bytes(b"MZ" + b"\0" * 200000)
        origin, a, b, c, d = origin_repo(base, fake_exe)
        script = base / "upgrade.sh"
        script.write_text(c_string("UPGRADE_SH"))
        src = base / "src"

        def run(have, force=False):
            env = dict(os.environ, HARDCORE_UPGRADE_SRC=str(src), HARDCORE_UPGRADE_URL=str(origin),
                       HARDCORE_UPGRADE_HAVE=have, HARDCORE_UPGRADE_FORCE="1" if force else "0")
            p = subprocess.run(["bash", str(script)], env=env, capture_output=True, text=True, timeout=120)
            tags = [l for l in p.stdout.splitlines() if l.startswith("@@")]
            return p.returncode, tags, p.stdout + p.stderr

        rc, tags, out = run(b)
        assert rc == 0 and tags[0].startswith(f"@@main {b} ") and tags[1:] == ["@@current"], out
        assert (src / ".git").is_dir(), "the first upgrade clones main"
        print("ok   main's own commit is current (the first upgrade clones main)")
        rc, tags, out = run(a)
        assert rc == 0 and tags[-1].startswith("@@built "), out
        assert (src / "built-from.txt").read_text().strip() == b, "built main's newest commit"
        print("ok   a build behind main builds main's newest commit")
        rc, tags, out = run(d)
        assert rc == 0 and tags[1:] == ["@@newer"], out
        print("ok   a build that has main and more is left alone")
        rc, tags, out = run(c)
        assert rc == 0 and tags[1:] == ["@@notmain"], out
        rc, tags, out = run("0123456789abcdef0123456789abcdef01234567")
        assert rc == 0 and tags[1:] == ["@@notmain"], out
        print("ok   a branch or local build (commits main lacks) is never replaced without --force")
        (src / "built-from.txt").unlink()
        rc, tags, out = run(c, force=True)
        assert rc == 0 and tags[-1].startswith("@@built ") and (src / "built-from.txt").read_text().strip() == b, out
        rc, tags, out = run("")
        assert rc == 0 and tags[-1].startswith("@@built "), out
        print("ok   --force, or a build whose commit is unknown, builds main")
        commit(origin, "E")                              # main moves on: the next upgrade fetches it
        e = git(origin, "rev-parse", "HEAD")
        rc, tags, out = run(b)
        assert tags[0].startswith(f"@@main {e} ") and (src / "built-from.txt").read_text().strip() == e, out
        print("ok   the next upgrade fetches main again")
    finally:
        shutil.rmtree(base, ignore_errors=True)


def shell_function_tests():
    """rc.bash's `hardcore` in a tab: a folder (or none) opens a window there; any other
    first word goes to hardcore.exe, so commands added later work in old tabs' places too."""
    rc = c_string("RC_BASH")
    start = rc.index('if [ -n "$HARDCORE_EXE" ] && ! declare -F hardcore')
    func = rc[start:rc.index("\nfi\n", start) + 4]
    base = Path(tempfile.mkdtemp(prefix="hcfn-"))
    try:
        exe = base / "hardcore.exe"
        exe.write_text('#!/bin/sh\necho "$*" >> "$(dirname "$0")/args"\necho out-"$1"\n')
        exe.chmod(0o755)
        (base / "dir with space").mkdir()

        def run(line):
            (base / "args").unlink(missing_ok=True)
            p = subprocess.run(["bash", "-c", func + "\n" + line + "\nwait; sleep 0.3"], cwd=base,
                               env=dict(os.environ, HARDCORE_EXE=str(exe)), capture_output=True, text=True, timeout=30)
            return p.stdout, (base / "args").read_text().splitlines() if (base / "args").exists() else []
        out, args = run("hardcore upgrade --force")
        assert args == ["upgrade --force"] and "out-upgrade" in out, (out, args)
        out, args = run("hardcore tab -- ls")
        assert args == ["tab -- ls"], args
        out, args = run("hardcore some-future-command")
        assert args == ["some-future-command"] and "out-some-future-command" in out, (out, args)
        out, args = run("hardcore 'dir with space'")
        assert args == [f"--cd {base}/dir with space"], args
        out, args = run("hardcore")
        assert args == [f"--cd {base}"], args
        print("ok   hardcore in a tab: a folder opens a window, any other word is a command for hardcore.exe")
    finally:
        shutil.rmtree(base, ignore_errors=True)


def winpath(path):
    return subprocess.check_output(["wslpath", "-w", str(path)], text=True).strip()


def windows_test(exe):
    import test_session_resume_windows as srw
    srw.OUT = OUT
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "driver.log").write_text("")
    source, helper = OUT / "driver.c", OUT / "driver.exe"
    source.write_text(srw.DRIVER)
    subprocess.run([os.environ.get("CC", "x86_64-w64-mingw32-gcc"), "-O2", "-Wall", "-municode", "-o", str(helper),
                    str(source), "-luser32"], check=True)
    work = Path(tempfile.mkdtemp(prefix="run-", dir=OUT))      # Windows side: the "installed" copy, its config
    install = work / "install"
    install.mkdir()
    installed = install / "hardcore.exe"
    shutil.copy2(exe, installed)
    config = work / "config"
    config.mkdir()
    (config / "config.ini").write_text("shell=\nwidth=900\nheight=600\n")
    lin = Path(tempfile.mkdtemp(prefix="hcup-"))                # Linux side: Claude's folder, main's stand-in
    fake = lin / "bin"
    fake.mkdir()
    (fake / "claude").write_text(srw.FAKE_CLAUDE)
    (fake / "claude").chmod(0o755)
    w1 = lin / "w1"
    w1.mkdir()
    origin, a, b, c, d = origin_repo(lin, exe)                 # "main" builds to this same exe
    try:
        app = srw.App(helper, installed, config, fake)
        app.send(f"T cd {w1} && claude --model opus")
        srw.wait_for(lambda: len(srw.calls(fake)) == 1, "claude")
        sid = re.search(r"--session-id (\S+)", srw.calls(fake)[0]).group(1)
        srw.wait_for(lambda: (fake / f"pid-{sid}").exists(), "claude's pid")
        pid = (fake / f"pid-{sid}").read_text().strip()
        srw.wait_for(lambda: [t.get("claude") for t in srw.saved_tabs(config)] == [sid], "the session saved")

        env = dict(os.environ, HARDCORE_INSTALL_EXE=winpath(installed), HARDCORE_TEST_CONFIG=winpath(config),
                   HARDCORE_UPGRADE_SRC=str(lin / "src"), HARDCORE_UPGRADE_URL=str(origin))
        env["WSLENV"] = (env.get("WSLENV", "") + ":HARDCORE_INSTALL_EXE:HARDCORE_TEST_CONFIG:"
                         "HARDCORE_UPGRADE_SRC:HARDCORE_UPGRADE_URL").lstrip(":")

        def upgrade(*args):
            p = subprocess.run([str(installed), "upgrade", *args], env=env, capture_output=True, text=True, timeout=300)
            return p.returncode, p.stdout

        version = subprocess.run([str(installed), "--version"], capture_output=True, text=True).stdout
        assert re.match(r"hardcore \S+ \(build \S+, commit [0-9a-f]{12}, \d{4}-\d\d-\d\d\)", version), version
        print("ok   hardcore --version names the build, its commit and date")

        rc, out = upgrade()                                     # this build's commit isn't on that main
        assert rc == 0 and "has commits main doesn't" in out and "--force" in out, out
        assert not list(install.glob("hardcore.old-*.exe")), "nothing changed"
        print("ok   hardcore upgrade leaves a build main doesn't have alone, and says --force")

        rc, out = upgrade("--force")
        assert rc == 0 and "installed" in out and "1 window now reloads in place" in out, out
        assert len(list(install.glob("hardcore.old-*.exe"))) == 1, "the old exe moved aside"
        os.kill(int(pid), 0)                                    # Claude kept running through it
        assert len(srw.calls(fake)) == 1, "Claude was not started again"
        time.sleep(3)
        assert [t.get("claude") for t in srw.saved_tabs(config)] == [sid], srw.saved_tabs(config)
        print("ok   upgrade --force builds main, swaps it in, and the window reloads in place: Claude runs on")

        app.end("K")                                            # then a crash: the session comes back by id
        app = srw.App(helper, installed, config, fake)
        srw.wait_for(lambda: len(srw.calls(fake)) > 1, "the resumed claude")
        assert srw.calls(fake)[1:] == [f"{w1}|--model opus --resume {sid}"], srw.calls(fake)
        print("ok   after the upgrade a crash still resumes Claude by its id")

        # make win-upgrade: a build of a checkout installs itself (--self), hot, the same way
        srw.wait_for(lambda: (fake / f"pid-{sid}").read_text().strip() != pid, "the resumed claude's pid")
        pid2 = (fake / f"pid-{sid}").read_text().strip()
        p = subprocess.run([str(exe), "upgrade", "--self"], env=env, capture_output=True, text=True, timeout=120)
        assert p.returncode == 0 and "installed" in p.stdout and "1 window now reloads in place" in p.stdout, p.stdout
        os.kill(int(pid2), 0)
        assert len(srw.calls(fake)) == 2, "Claude was not started again"
        p = subprocess.run([str(installed), "upgrade", "--self"], env=env, capture_output=True, text=True, timeout=60)
        assert "the installed hardcore already" in p.stdout, p.stdout
        app.end("K")
        print("ok   upgrade --self (make win-upgrade) installs a checkout's build hot; Claude runs on")

        # run in a tab of the window it reloads, as you would: every line whole, in order,
        # and all of it before the next prompt
        for f in (config / "session").glob("*"):
            f.unlink()
        app = srw.App(helper, installed, config, fake)
        exports = " ".join(f"{k}='{env[k]}'" for k in ("HARDCORE_INSTALL_EXE", "HARDCORE_TEST_CONFIG")) + \
            " WSLENV=HARDCORE_INSTALL_EXE:HARDCORE_TEST_CONFIG"
        app.send(f"T clear; export {exports}; '{exe}' upgrade --self | cat; echo END-$((6*7))")

        def tab_text():
            texts = sorted((config / "session").glob("*-tab*.txt"))
            return texts[0].read_text(errors="replace") if texts else ""
        srw.wait_for(lambda: "END-42" in tab_text(), "the command's end in the tab", 60)
        time.sleep(3)
        text = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", tab_text())
        rows = [r.rstrip() for r in text.replace("\r", "").split("\n")]
        at = {key: next((i for i, r in enumerate(rows) if r.startswith(key)), -1)
              for key in ("hardcore 0", "hardcore: installed", "hardcore: done.", "END-42")}
        assert -1 not in at.values() and at["hardcore 0"] < at["hardcore: installed"] < at["hardcore: done."] < at["END-42"], (at, rows)
        assert not any("hardcore:" in r for r in rows[at["END-42"]:]), rows
        app.end("K")
        print("ok   in a tab of the window it reloads: whole lines, in order, all before the prompt")
    finally:
        shutil.rmtree(lin, ignore_errors=True)


if __name__ == "__main__":
    shell_function_tests()
    script_tests()
    if len(sys.argv) < 2 or sys.argv[1] != "--script-only":
        windows_test(ROOT / "hardcore.exe")
