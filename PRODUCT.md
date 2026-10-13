# hardcore as a product

Who it's for, what it assumes, what's handled, and what sharing it would
still take. Written 2026-10-13.

## Who it's for

People on Windows who live in a Linux shell, and these days mostly in
Claude Code, and want the terminal to get out of the way:
- one look and one layout, no settings;
- tabs that know where you are;
- a tab column that tells you which Claude needs you.

That's a real gap. Windows Terminal is a jack of all trades, and the fast,
opinionated terminals (Ghostty, kitty, foot) are Linux and macOS first.

## What it assumed, and what happens now

| Assumption | Before | Now |
|---|---|---|
| WSL is installed with a distro | wsl.exe exits, the window vanishes | the registry is checked at startup (microseconds, no WSL boot). With nothing to run, a welcome screen offers **I** = `wsl --install`, **P** = PowerShell |
| The distro has bash | instant exit | detected (`sh -c 'command -v bash'`); tabs fall back to the default shell |
| Drives are under `/mnt/` | rc path, spare `cd`, dropped paths all break | the rc path goes through `WSLENV` `/p`, the spare converts with `wslpath`, and rc.bash reports a non-`/mnt` root |
| US keyboard | Ctrl+Alt+digit swallowed AltGr `{ [ ] }` | AltGr (right Alt) never triggers tab jumps |
| Any startup failure | window disappears | the tab stays, shows WSL's own error, **Enter** retries |
| WSL at all | required | `shell=powershell` gives the same terminal over PowerShell |

Still assumed, deliberately:
- **Dark only, Unifont only, integer scale only.** At 125% DPI the text is
  1×, small but sharp. That's the product, not a gap.
- **bash for folder tracking** (and Ctrl+Shift+↑/↓ prompt marks). zsh and fish
  users get bash in hardcore's tabs.

## Different kinds of WSL

| | Support |
|---|---|
| WSL 2 (default today), WSL 1 | same: `wsl.exe` hides the difference |
| Store WSL vs inbox WSL | same `wsl.exe` and same registry key |
| Several distros | `distro=` in config.ini. One per window, on purpose |
| Custom distros (imported tarballs) | fine, if they have bash; otherwise the fallback above |
| Root-only distros | fine |
| `[automount] root=/` | handled (see above) |
| `[interop] appendWindowsPath=false` | fine: nothing of ours runs inside Linux except bash |

## Not handled yet

1. **zsh and fish as the tab shell** (with folder tracking). Doable:
   `ZDOTDIR` for zsh, `--init-command` for fish. This is the most likely
   complaint from Linux people.
2. **Windows 10:** needs a test pass on a real machine.
3. **Kitty graphics.** Claude Code asks (`APC _G … a=q`) and ConPTY passes it
   through. Supporting it would put images inline. Medium-sized work.
4. **Find in scrollback.**

## What sharing takes

1. **A license.** Done: MIT. The bundled parts allow it: Unifont is
   OFL/GPL-with-font-exception, and ConPTY is MIT.
2. **Public repo and releases.** Done: the repo is public and `hardcore.exe`
   is a GitHub release. It's one file, about 6 MB, with nothing to install.
3. **SmartScreen.** An unsigned exe gets "Windows protected your PC" on
   download. The fix is code signing (Azure Trusted Signing is about
   $10/month); winget and Scoop help with reputation too.
4. **winget / Scoop manifests,** once there's a release URL.
5. **No auto-update, no telemetry.** That fits the product. Releases are
   enough.

## Without Windows: Debian and other Linux

The portable core is maybe 70% of the code:
- the VT parser and screen and scrollback model;
- the sidebar, throbber and Claude Code status;
- the immediate-mode renderer.

The rest is the Win32 platform layer: window, input, ConPTY, D3D11,
clipboard, drag and drop. A Linux port would replace it with
`forkpty` + Wayland/X11 (SDL3 or GLFW) + OpenGL/Vulkan. That's a few weeks of
focused work.

Recommendation: **not yet.** On Linux, hardcore would be one more
fast terminal among very good ones. On Windows, its edge is:
- WSL done right: the start folder, new tabs in the same folder, the
  paths;
- the Claude Code tab column, now driven by Claude's own OSC 7501 status.

Ship it on Windows, see who shows up, and port later if asked. Keeping the
parser and renderer free of Win32 (they nearly are) keeps that door open
cheaply.
