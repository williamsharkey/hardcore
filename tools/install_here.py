#!/usr/bin/env python3
"""Install the portable terminal frontend, retaining a native GUI command."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

MARKER = b"# HARDCORE_TERMINAL_LAUNCHER_V1"
LAUNCHER_HEADER = b"#!/usr/bin/env python3\n" + MARKER + b"\n"
REQUIRED_FILES = ('here.py', 'here_frontend.py', 'here_settings.py', 'here_broker.py',
                  'here_terminal.py', 'here_web.py', 'files.py')


def owned_launcher(path):
    """Recognize our complete launcher header, never a marker in user text."""
    try:
        with Path(path).open("rb") as source:
            return source.read(len(LAUNCHER_HEADER)) == LAUNCHER_HEADER
    except OSError:
        return False


def atomic(path, data, mode=0o644):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".hardcore-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as out:
            out.write(data)
        os.chmod(temporary, mode)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def native_candidate():
    for candidate in (os.environ.get("HARDCORE_EXE"), shutil.which("hardcore.exe")):
        if candidate and Path(candidate).is_file():
            return str(Path(candidate).resolve())
    # The Windows installer changes the user PATH, not this WSL shell's PATH.
    if shutil.which("powershell.exe") and shutil.which("wslpath"):
        try:
            win = subprocess.check_output(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command",
                "[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false); "
                "[Console]::Write((Join-Path $env:LOCALAPPDATA 'hardcore/hardcore.exe'))"], timeout=10).decode("utf-8").strip()
            linux = subprocess.check_output(["wslpath", "-u", win], timeout=5).decode().strip()
            if Path(linux).is_file():
                return str(Path(linux).resolve())
        except (OSError, subprocess.SubprocessError, UnicodeError):
            pass
    return None


def install(bindir, native=None):
    bindir = Path(bindir).expanduser().resolve()
    target = bindir / "hardcore"
    data = bindir.parent / "share" / "hardcore-terminal"
    config = data / "launcher.json"
    if target.is_symlink():
        native = native or str(target.resolve())
    elif target.exists():
        if not owned_launcher(target):
            raise ValueError(f"{target} is an existing command; leave it intact and use python3 tools/here.py, or choose a different BINDIR")
        if native is None and config.exists():
            native = json.loads(config.read_text()).get("native")
    if native is None:
        native = native_candidate()
    if native is not None:
        native = str(Path(native).expanduser().resolve())
        if native == str(target) or not Path(native).is_file() or not os.access(native, os.X_OK):
            raise ValueError("native fallback must name a different, existing executable")
    source = Path(__file__).resolve().parent
    # Preflight the entire runtime before touching an existing installation.
    # The entry point now imports siblings, and Files is launched by path.
    missing = [name for name in REQUIRED_FILES if not (source / name).is_file()]
    if missing:
        raise ValueError("incomplete terminal runtime: missing " + ", ".join(missing))
    resources = {path.name:path.read_bytes() for path in sorted(source.glob('here*.py'))}
    resources['files.py'] = (source / 'files.py').read_bytes()
    for name, payload in resources.items():
        atomic(data / name, payload)
    atomic(config, (json.dumps({"native": native}) + "\n").encode())
    launcher = f'''#!/usr/bin/env python3
# HARDCORE_TERMINAL_LAUNCHER_V1
import json, os, sys
from pathlib import Path
root = Path({str(data)!r})
args = sys.argv[1:]
if args and args[0] == "here":
    os.execv(sys.executable, [sys.executable, "-B", str(root / "here.py"), *args[1:]])
native = json.loads((root / "launcher.json").read_text()).get("native")
if native:
    os.execv(native, [native, *args])
os.execv(sys.executable, [sys.executable, "-B", str(root / "here.py"), *args])
'''
    atomic(target, launcher.encode(), 0o755)
    return target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bindir", nargs="?", default=str(Path.home()/".local/bin"))
    parser.add_argument("--native", help="GUI executable to retain for commands other than here")
    args = parser.parse_args()
    try:
        target = install(args.bindir, args.native)
    except (OSError, ValueError) as exc:
        print(f"hardcore: {exc}", file=sys.stderr)
        return 1
    print(f"Installed terminal command: {target}\nRun: hardcore here")
    if str(target.parent) not in os.environ.get("PATH", "").split(os.pathsep):
        print(f"Add {target.parent} to your PATH, or run {target} here")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
