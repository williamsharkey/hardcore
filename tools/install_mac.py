"""Install a verified macOS app, retain its predecessor, and reload live windows."""
from datetime import datetime
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile


def install(source, appdir, bindir):
    source, appdir, bindir = (Path(p).expanduser().absolute() for p in (source, appdir, bindir))
    app = appdir / source.name
    command = bindir / "hardcore"
    executable = app / "Contents/MacOS/hardcore"
    resolved_source = source.resolve()
    if resolved_source == app.resolve():
        raise SystemExit("Install destination must differ from the source app.")
    if appdir.resolve().is_relative_to(resolved_source):
        raise SystemExit("Install destination must not be inside the source app.")
    if not (source / "Contents/MacOS/hardcore").is_file():
        raise SystemExit(f"Missing app executable in {source}")
    if os.path.lexists(command) and not (
        command.is_symlink() and command.resolve() == executable.resolve()
    ):
        raise SystemExit(f"Refusing to overwrite {command}; choose another BINDIR.")
    appdir.mkdir(parents=True, exist_ok=True)
    bindir.mkdir(parents=True, exist_ok=True)
    if app.is_symlink():
        raise SystemExit(f"Refusing to replace app symlink: {app}")
    # Finish copying and validation before touching the installed bundle.
    # Retain the previous app: an older running process may still need it.
    with tempfile.TemporaryDirectory(prefix=".hardcore-install-", dir=appdir) as temp:
        replacement = Path(temp) / source.name
        shutil.copytree(source, replacement, symlinks=True)
        subprocess.run(["/usr/bin/codesign", "--verify", "--deep", "--strict", str(replacement)],
                       stdin=subprocess.DEVNULL, timeout=30, check=True)
        probe = subprocess.run([str(replacement / "Contents/MacOS/hardcore"), "--mac-update-protocol"],
                               stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=15, check=True)
        if probe.stdout != "hardcore-mac-reload-1\n":
            raise ValueError("App does not support the current macOS update protocol")
        backup = app.with_name(app.name + ".previous")
        if app.exists():
            if backup.is_symlink():
                raise ValueError(f"Refusing to replace app backup symlink: {backup}")
            if backup.exists():
                stamp = datetime.now().strftime("%Y%m%d-%H%M%S-%f")
                os.rename(backup, backup.with_name(backup.name + "-" + stamp))
            os.rename(app, backup)
        try:
            os.rename(replacement, app)
        except BaseException:
            if backup.exists() and not app.exists():
                try:
                    os.rename(backup, app)
                except OSError:
                    print(f"Previous app retained for recovery: {backup}", file=sys.stderr)
                    raise
            raise
    if not command.is_symlink():
        command.symlink_to(executable)
    print(f"Installed {app}")
    print(f"Installed command: {command}")
    try:
        result = subprocess.run([str(executable), "--reload"], stdin=subprocess.DEVNULL, timeout=120)
        if result.returncode:
            print("Installed; some windows could not reload. Their current sessions remain open.")
    except (OSError, subprocess.SubprocessError) as error:
        print(f"Installed; could not finish reloading live windows: {error}")
    if str(bindir) in os.environ.get("PATH", "").split(os.pathsep):
        print("Run: hardcore  (or hardcore DIR to open a folder)")
        found = shutil.which("hardcore")
        if found and Path(found).resolve() != executable.resolve():
            print(f"Note: {found} takes precedence on PATH; use {shlex.quote(str(command))}.")
    else:
        export = f'export PATH={shlex.quote(str(bindir))}:"$PATH"'
        print("To run hardcore from your current zsh/bash shell, first run:")
        print(f"  {export}")
        print("For future shells, add that line to ~/.zshrc (zsh) or ~/.bash_profile (bash).")
    print(f"Launch from Finder, or run: open {shlex.quote(str(app))}")


if __name__ == "__main__":
    install(*sys.argv[1:])
