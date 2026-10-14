# WSL: "Bus error" a few minutes into a Claude Code session (open, 2026-10-13; low-disk warning shipped)

## Symptoms (Windows 11, hardcore → WSL 2 → Claude Code)

- A few minutes into a Claude Code session, processes start dying with
  `Bus error` (SIGBUS).
- It spreads: other programs in the distro crash too, and WSL stops working
  well until it's restarted. hardcore's tabs die along with it.

## Is hardcore causing it?

Probably not directly. hardcore is a Windows program. All it puts inside
Linux is `wsl.exe … bash --rcfile rc.bash -i` per tab, plus one idle spare
bash per window. A SIGBUS is raised by the Linux kernel inside the VM, when a
process touches a memory-mapped page that the backing store can't supply.
Nothing hardcore sends through ConPTY can cause that.

hardcore does make one of the likely causes more likely, though. New
windows start in `/mnt/c/Users/<you>`, on the 9p/DrvFs Windows mount, so
Claude Code sessions tend to run over Windows files (see cause 4).

## Likely causes, most likely first

Each claim below is from the linked issue, all checked on 2026-10-13.
Anything marked *inference* is a guess, not a confirmed report.

1. **The Windows drive fills up, so the distro's `ext4.vhdx` and swap stop
   accepting writes.** This matches "spreads to everything" best.
   - [microsoft/WSL#41134](https://github.com/microsoft/WSL/issues/41134): a VHDX that inherited NTFS compression failed writes, the filesystem went read-only, and unrelated processes got SIGBUS.
   - [microsoft/WSL#41314](https://github.com/microsoft/WSL/issues/41314) (closed "by design"): WSL's crash collector wrote 58–65 GiB dumps to `%TEMP%\wsl-crashes` and filled C:. See also [#41653](https://github.com/microsoft/WSL/issues/41653).
   - *Inference:* one crash produces a huge dump, the dump fills C:, and then every page-in from the VHDX or swap fails. That would look exactly like "a few minutes in, then everything".
2. **The VM runs out of memory.**
   [anthropics/claude-code#82188](https://github.com/anthropics/claude-code/issues/82188):
   a Claude Code background daemon leaked to about 11 GB, and the OOM kills
   took down the whole WSL 2 session. Those are SIGKILL, not SIGBUS. Memory
   pressure turns into SIGBUS only through a failed swap-in (back to cause 1)
   or a full `/dev/shm`.
3. **A broken Claude Code build or install.** This kills only `claude`, so
   it doesn't explain the spread, but it's the cheapest to rule out.
   - [anthropics/claude-code#93038](https://github.com/anthropics/claude-code/issues/93038): 2.1.266 crashes with "Bus error" on WSL 2; 2.1.265 works.
   - [anthropics/claude-code#84051](https://github.com/anthropics/claude-code/issues/84051): an interrupted auto-update can leave a truncated native binary.
4. **Memory-mapped files on `/mnt/c` that change under the reader.**
   - Any process that `mmap`s a file gets SIGBUS when the file shrinks underneath it. ripgrep, which Claude Code searches with, is the classic case: [BurntSushi/ripgrep#581](https://github.com/BurntSushi/ripgrep/issues/581).
   - *Inference:* on `/mnt/c/Users/<you>`, OneDrive (Files On-Demand placeholders), antivirus and Windows-side editors all rewrite files underneath WSL. That kills only the process doing the reading, but Claude Code restarts searches constantly.

## How to confirm (right after it happens, before `wsl --shutdown`)

PowerShell:

```powershell
Get-PSDrive C                                                    # free space on C:
gci $env:TEMP\wsl-crashes -ErrorAction SilentlyContinue | measure Length -Sum
type $env:USERPROFILE\.wslconfig
wsl --version
# the distro's disk: compressed or sparse is bad news
gci "$env:LOCALAPPDATA\Packages\*\LocalState\ext4.vhdx","$env:LOCALAPPDATA\wsl\*\ext4.vhdx" -ErrorAction SilentlyContinue |
  select FullName, Length, Attributes
```

In WSL (another tab, if it still opens):

```bash
sudo dmesg | grep -iE 'ext4|I/O error|read-only|blk_update|oom|bus error' | tail -40
df -h / /dev/shm /tmp
free -h
claude --version; ls -la ~/.local/share/claude/versions/
pwd     # is the session on /mnt/c?
```

## Fixes to try

1. `%USERPROFILE%\.wslconfig`:

   ```ini
   [wsl2]
   maxCrashDumpCount=-1     # no crash dumps filling C: (or set crashDumpFolder= on a roomy drive)
   memory=12GB              # pick about half your RAM; caps the VM instead of starving Windows
   swap=8GB
   ```

   Then run `wsl --shutdown`. Also free space on C:. If the `.vhdx` shows
   `Compressed`, run `compact /u` on it. Leave `sparseVhd` off.
2. Run Claude Code on the Linux filesystem (`~/src/...`), not `/mnt/c`. For
   hardcore, put `dir=/home/<you>` (or any Linux path) in
   `%LOCALAPPDATA%\hardcore\config.ini`, so new windows start there.
3. Update Claude Code, or pin 2.1.265 if you're on 2.1.266 (#93038). If the
   binary might be truncated, reinstall it (#84051).
4. If `dmesg` shows ext4 errors, run `e2fsck` on the distro's disk.

## Done in hardcore (main, 0921cb3)

The tab column now warns when the drive that holds WSL's disk runs low on
space. It shows amber under 20 GB free and red under 5 GB, checked every
minute. That's the most likely cause above, caught before it takes WSL down.

## Possible hardcore changes (not done; they need a Windows box to test)

- Default the start folder to the Linux home when the distro has one,
  instead of `%USERPROFILE%`. This is a product decision: "starts in
  `/mnt/c/Users/<you>`" is in the README as a feature.
- When a tab's shell dies with a signal (exit code 135 = 128 + SIGBUS),
  say so in the tab instead of closing it. Then "WSL is falling over" is
  visible instead of tabs silently vanishing.
