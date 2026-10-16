"""Fetch a verified macOS release and atomically replace the installed app.

Live PTY handoff is performed by the native CLI after this helper succeeds.
Only the canonical hardcore-macos.zip asset with GitHub's SHA-256 is accepted.
"""
import argparse
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

RELEASE_URL = "https://api.github.com/repos/williamsharkey/hardcore/releases/latest"
MAX_DOWNLOAD = 256 * 1024 * 1024


def fetch(url, limit=MAX_DOWNLOAD):
    if not url.startswith("https://"):
        raise ValueError("Update downloads must use HTTPS")
    request = urllib.request.Request(url, headers={"User-Agent": "hardcore-update", "Accept": "application/vnd.github+json"})
    with urllib.request.urlopen(request, timeout=30) as response:
        if not response.url.startswith("https://"):
            raise ValueError("Update redirected outside HTTPS")
        data = response.read(limit + 1)
    if len(data) > limit:
        raise ValueError("Update exceeds the download size limit")
    return data


def version(value):
    match = re.fullmatch(r"v?(\d+)\.(\d+)(?:\.(\d+))?", value)
    if not match:
        raise ValueError(f"Unrecognized release version: {value}")
    return tuple(int(x or 0) for x in match.groups())


def unpack(archive, destination):
    with zipfile.ZipFile(archive) as bundle:
        total = 0
        for item in bundle.infolist():
            path = PurePosixPath(item.filename)
            mode = item.external_attr >> 16
            if path.is_absolute() or ".." in path.parts or not path.parts or path.parts[0] != "hardcore.app":
                raise ValueError("Release contains a file outside hardcore.app")
            if stat.S_ISLNK(mode) or (stat.S_IFMT(mode) not in (0, stat.S_IFREG, stat.S_IFDIR)):
                raise ValueError("Release contains an unsupported file type")
            total += item.file_size
            if total > MAX_DOWNLOAD:
                raise ValueError("Expanded update exceeds the size limit")
        bundle.extractall(destination)
    app = destination / "hardcore.app"
    exe = app / "Contents/MacOS/hardcore"
    if not exe.is_file() or not (app / "Contents/Info.plist").is_file():
        raise ValueError("Release is missing its app executable or Info.plist")
    exe.chmod(0o755)
    subprocess.run(["/usr/bin/codesign", "--verify", "--deep", "--strict", str(app)],
                   stdin=subprocess.DEVNULL, timeout=30, check=True)
    # A valid app must implement our receiving protocol before replacing anything.
    probe = subprocess.run([str(exe), "--version"], stdin=subprocess.DEVNULL,
                           capture_output=True, text=True, timeout=10, check=True)
    if "macOS handoff protocol 1" not in probe.stdout:
        raise ValueError("Release does not support this live-handoff protocol")
    return app


def update(executable, current, force=False):
    executable = Path(executable).resolve()
    if executable.parent.name != "MacOS" or executable.parent.parent.name != "Contents" or executable.parent.parent.parent.suffix != ".app":
        raise ValueError("Release updates require an installed .app; use make install, then hardcore update --local")
    target = executable.parent.parent.parent
    print("hardcore: checking GitHub for a macOS release...", flush=True)
    release = json.loads(fetch(RELEASE_URL, 4 * 1024 * 1024))
    tag = release["tag_name"]
    asset = next((a for a in release.get("assets", []) if a.get("name") == "hardcore-macos.zip"), None)
    if not asset:
        raise ValueError(f"Release {tag} has no hardcore-macos.zip. For a local build, run make install then hardcore update --local.")
    if not force and version(tag) <= version(current):
        print(f"hardcore: up to date ({current}; latest release {tag})")
        return
    digest = asset.get("digest", "")
    if not re.fullmatch(r"sha256:[a-fA-F0-9]{64}", digest):
        raise ValueError("Release has no SHA-256 digest; refusing to install it")
    data = fetch(asset["browser_download_url"])
    if hashlib.sha256(data).hexdigest() != digest[7:].lower():
        raise ValueError("Release SHA-256 does not match; installed app is unchanged")
    # Same-volume staging makes the final renames atomic. Keep the backup
    # outside automatic staging cleanup, including if rollback itself fails.
    with tempfile.TemporaryDirectory(prefix=".hardcore-update-", dir=target.parent) as temp:
        stage = Path(temp)
        archive = stage / "release.zip"
        archive.write_bytes(data)
        replacement = unpack(archive, stage / "unpacked")
        backup = target.with_name(target.name + ".previous")
        if backup.is_symlink():
            raise ValueError(f"Refusing to replace app backup symlink: {backup}")
        if backup.exists():
            stamp = datetime.now().strftime("%Y%m%d-%H%M%S-%f")
            os.rename(backup, backup.with_name(backup.name + "-" + stamp))
        os.rename(target, backup)
        try:
            os.rename(replacement, target)
        except BaseException:
            try:
                os.rename(backup, target)
            except OSError:
                print(f"hardcore: previous app retained for recovery: {backup}", file=sys.stderr)
                raise
            raise
    print(f"hardcore: installed {tag}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    try:
        update(args.executable, args.version, args.force)
    except (OSError, ValueError, KeyError, subprocess.SubprocessError, zipfile.BadZipFile) as error:
        print(f"hardcore: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
