#!/usr/bin/env python3
"""Install a macOS bundle and a stable CLI link without deleting a running app."""
import argparse
from datetime import datetime
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def install(source, destination, bin_dir):
    source = source.resolve()
    destination = destination.expanduser().absolute()
    bin_dir = bin_dir.expanduser().absolute()
    if source == destination:
        raise ValueError("source and installed app must be different paths")
    destination.parent.mkdir(parents=True, exist_ok=True)
    bin_dir.mkdir(parents=True, exist_ok=True)
    launcher = bin_dir / "hardcore"
    executable = destination / "Contents/MacOS/hardcore"
    # Refuse to silently replace a user's unrelated command.
    if os.path.lexists(launcher) and not (launcher.is_symlink() and launcher.resolve() == executable.resolve()):
        raise ValueError(f"{launcher} already exists and points elsewhere; installation unchanged")
    with tempfile.TemporaryDirectory(prefix=".hardcore-install-", dir=destination.parent) as tmp:
        staged = Path(tmp) / "hardcore.app"
        shutil.copytree(source, staged, symlinks=True)
        subprocess.run(["/usr/bin/codesign", "--verify", "--deep", "--strict", str(staged)], check=True)
        probe = subprocess.run([str(staged / "Contents/MacOS/hardcore"), "--mac-update-protocol"],
                               capture_output=True, text=True, timeout=15, check=True)
        if probe.stdout != "hardcore-mac-reload-1\n":
            raise ValueError("app does not support the current macOS update protocol")
        backup = Path(str(destination) + ".previous")
        if destination.exists():
            if backup.exists():
                stamp = datetime.now().strftime("%Y%m%d-%H%M%S-%f")
                backup.rename(Path(str(backup) + "-" + stamp))
            destination.rename(backup)
        try:
            staged.rename(destination)
        except OSError:
            if backup.exists() and not destination.exists():
                backup.rename(destination)
            raise
    if not os.path.lexists(launcher):
        launcher.symlink_to(executable)
    print(f"Installed {destination}\nCommand: {launcher}", flush=True)
    # Older builds have no listener: leave their windows running until the user
    # restarts them once. New builds hand off their PTYs without killing jobs.
    result = subprocess.run([str(executable), "--reload"], timeout=120)
    if result.returncode:
        print("Installed; some windows could not reload. Their current sessions remain open.")
    if str(bin_dir) not in os.environ.get("PATH", "").split(os.pathsep):
        print(f"Add {bin_dir} to your shell's PATH to run hardcore by name.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("bin_dir", type=Path)
    args = parser.parse_args()
    install(args.source, args.destination, args.bin_dir)
