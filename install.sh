#!/bin/sh
# hardcore installer, from WSL (or macOS):
#   curl -fsSL https://raw.githubusercontent.com/williamsharkey/hardcore/main/install.sh | sh
# In WSL it runs install.ps1 on the Windows side (hardcore is a Windows app).
set -e
URL=https://raw.githubusercontent.com/williamsharkey/hardcore/main/install.ps1
case "$(uname -s)" in
  Linux)
    if grep -qi microsoft /proc/version 2>/dev/null; then
      PS=$(command -v powershell.exe || echo /mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe)
      cd /mnt/c 2>/dev/null || true            # a Windows-side working folder for powershell.exe
      exec "$PS" -NoProfile -ExecutionPolicy Bypass -Command "irm $URL | iex"
    fi
    echo "hardcore is a terminal for WSL: run this inside WSL on Windows." >&2; exit 1 ;;
  Darwin)
    echo "hardcore for macOS isn't published as a download yet. Build it:" >&2
    echo "  git clone https://github.com/williamsharkey/hardcore && cd hardcore && make hardcore.app" >&2
    exit 1 ;;
  *) echo "hardcore runs on Windows (with WSL) and macOS." >&2; exit 1 ;;
esac
