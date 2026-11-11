# WSL disk reclaim: give free Linux space back to C:

Status, 2026-11-11: the owner approved the choices in section 11. The
throwaway-distro test ran (section 12). The feature is built on branch
`feat/wsl-disk-reclaim` (not merged). The real compaction of Debian has not
run; section 13 has the steps.

## 1. The problem, from square one

Linux in WSL 2 keeps all its files in one Windows file, `ext4.vhdx`. That file
is a virtual hard disk. When Linux writes new data, the file grows on C:. When
Linux deletes data, the file does not get smaller by itself. So C: loses space
over time, even when Linux uses less.

When C: becomes full, Linux cannot write to its disk. Programs stop with "Bus
error" and the WSL virtual machine stops. Every hardcore tab stops with it (see
`issues/wsl-bus-errors.md`).

Example (this PC, 2026-11-11, measured read-only):

| Measurement | Value | How |
|---|---|---|
| Linux used space (`df /` in Debian) | 165 GiB | `df -h /` |
| vhdx length (Explorer "Size") | 268.1 GiB | `Get-Item` Length |
| vhdx clusters on C: (Explorer "Size on disk") | 200.9 GiB | `GetCompressedFileSizeW` |
| Sparse flag on the vhdx | set | `fsutil sparse queryflag` |
| Allocated ranges in the file | 48,571 | `fsutil sparse queryrange` |
| C: free | 15.2 GiB of 475.7 GiB | `Get-PSDrive C` |
| Other drives | none | `Get-PSDrive` |
| WSL | 2.7.3.0, kernel 6.6.114.1 | `wsl --version` |
| Root mount options | `rw,relatime,discard,errors=remount-ro` | `/proc/mounts` |
| Crash dumps (`%TEMP%\wsl-crashes`) | 0 GiB | `Get-ChildItem` |

So about 36 GiB on C: holds Linux blocks that Linux does not use. This is the
space to give back.

The goal: hardcore does this by itself. The user does not run scripts. hardcore
never stops work that runs without consent.

## 2. What each WSL setting really does (evidence)

### 2.1 Online discard (the `discard` mount option)

WSL already mounts each distro's ext4 with `discard`. The WSL source sets
`"discard,errors=remount-ro,data=ordered"` for the distro mount
(`src/windows/service/exe/WslCoreVm.cpp`, line 1213 on main, 2026-11-11). This
PC shows `discard` in `/proc/mounts`. So when Linux deletes a file, ext4 already
tells the virtual disk that the blocks are free. A guest mount option is not
the missing piece.

### 2.2 `fstrim`

`fstrim` sends discard requests for all free ext4 space. With online discard on,
it mostly sends again what was already sent. After a restart, ext4 forgets
which groups it trimmed, so the first `fstrim` reports the whole free space
(this PC: "830 GiB trimmed"). The number is free space in Linux. It is not
space returned to C:.

`fstrim` alone never returns space for a normal (not sparse) vhdx. The VHDX
driver records the blocks as unused in its block table. The file keeps its
size until a compaction moves blocks and cuts the file shorter.

### 2.3 Sparse vhdx (`wsl --manage <distro> --set-sparse true`)

What it does: WSL calls `FSCTL_SET_SPARSE` on the file. That is all
(`LxssUserSession.cpp`, `SetSparse`). The distro must be stopped; only that
distro, not the whole VM (the code checks `m_runningInstances` for that distro).

What it should do: when Linux discards a block, Windows should free the NTFS
clusters under it ("punch a hole"). The file length stays the same. Only "Size
on disk" goes down.

What happens in practice:

- It works on new disks for small tests. A user installed Debian, set sparse,
  wrote 1 GB, deleted it, and saw the space come back in seconds
  ([WSL#12103, 2026-04-19](https://github.com/microsoft/WSL/issues/12103)).
- It does not work well on disks that were already in use. Users report no
  change after `set-sparse` + `fstrim`
  ([WSL#12103](https://github.com/microsoft/WSL/issues/12103),
  [WSL#10703](https://github.com/microsoft/WSL/issues/10703),
  [Microsoft Q&A 1526083](https://learn.microsoft.com/en-za/answers/questions/1526083/in-wsl2-with-sparse-vhd-the-storage-usage-does-not)).
  One user found that blocks written before a WSL restart and deleted after it
  did not come back.
- Microsoft has not explained this. A WSL program manager wrote in 2025: "we
  are still seeing quality reports and issues with sparse VHDs and so have made
  it an opt in experimental feature"
  ([WSL#13241](https://github.com/microsoft/WSL/issues/13241)).
- Corruption reports exist from 2023 and 2024
  ([WSL#10609](https://github.com/microsoft/WSL/issues/10609)). WSL 2.5 to 2.9
  refused `set-sparse true` without `--allow-unsafe`
  ([WSL#13075](https://github.com/microsoft/WSL/issues/13075)). WSL 3.0.2
  removed that gate and shows an "experimental" warning instead
  ([PR #41726](https://github.com/microsoft/WSL/pull/41726)).

### 2.4 `.wslconfig` `[experimental] sparseVhd=true`

Microsoft docs: "When set to true, any newly created VHD will be set to sparse
automatically" ([wsl-config](https://learn.microsoft.com/en-us/windows/wsl/wsl-config)).
It does not change an existing disk. Before WSL 3.0.2 the setting was ignored
for new disks too (source of PR #41726: "honor `experimental.sparseVhd` when
creating new distribution VHDs").

### 2.5 `.wslconfig` `[experimental] autoMemoryReclaim`

This setting returns **memory** (RAM cache) to Windows. It does nothing for
disk space ([wsl-config](https://learn.microsoft.com/en-us/windows/wsl/wsl-config)).

### 2.6 Compaction

Compaction is the only reliable way. It moves used blocks to the front of the
vhdx and cuts the file shorter. Windows has one API for it:
`CompactVirtualDisk` ([docs](https://learn.microsoft.com/en-us/windows/win32/api/virtdisk/nf-virtdisk-compactvirtualdisk)).
`Optimize-VHD` and `diskpart compact vdisk` call it too.

Facts from the docs:

- Without attaching the disk, compaction is "file-system-agnostic". It frees
  blocks that the block table marks unused (trimmed) and blocks that are all
  zeros. For ext4 that is the right mode. Windows cannot read ext4, so the
  "file-system-aware" mode (attach read-only, needs admin) adds nothing.
- `Optimize-VHD -Mode Full` on a VHDX that is not attached read-only falls
  back to `Prezeroed` mode
  ([Optimize-VHD](https://learn.microsoft.com/en-us/powershell/module/hyper-v/optimize-vhd)).
  So the usual "Optimize-VHD -Mode Full" recipe is the agnostic mode anyway.
- "A compaction operation on a virtual disk can be safely interrupted and
  re-run later."
- "The CompactVirtualDisk function runs on the virtual disk in the same
  security context as the caller."

Facts measured on this PC today (throwaway 64 MB VHDX files in `%TEMP%`, both
deleted after):

| Test | Result |
|---|---|
| Normal user (`runas /trustlevel:0x20000`, no admin token): `CreateVirtualDisk`, `OpenVirtualDisk(GET_INFO\|METAOPS)`, `CompactVirtualDisk` on a normal VHDX | all succeed |
| Same, sparse VHDX, open flags `0` | open fails `0xC03A001A` `STATUS_VIRTUAL_DISK_LIMITATION`: "Virtual hard disk files must be uncompressed and unencrypted and must not be sparse." This is the `Optimize-VHD` error. |
| Same, sparse VHDX, open flag `OPEN_VIRTUAL_DISK_FLAG_SUPPORT_SPARSE_FILES_ANY_FS` (`0x400`) | open and compact succeed; the file stays sparse |

So:

- **Compaction does not need admin.** It does not need the Hyper-V module, so
  it also works on Windows Home.
- **A sparse vhdx can be compacted without clearing the sparse flag.** The
  `Optimize-VHD` cmdlet only fails because it does not pass flag `0x400`.

Measured later on a disk with real ext4 data (section 12): a compaction of a
sparse copy freed all of the trimmed space, kept the sparse flag, took 3.4 s
for 3 GB, and used no extra drive space while it ran.

### 2.7 `wsl --manage <distro> --compact` (WSL 3.0.1 and later)

WSL added this command in August 2026
([PR #40481](https://github.com/microsoft/WSL/pull/40481), in releases 3.0.1 to
3.0.5; not in 2.7.3, which this PC has). From the source:

- It **terminates the distro by itself** (`_ConversionBegin` calls
  `_TerminateInstanceInternal`). It does not ask. It kills running work.
- It runs an offline `e2fsck -f -y -E discard` on the disk first. This
  discards free blocks again, then compacts with `CompactVirtualDisk`.
- It opens the disk without flag `0x400`, so it most likely fails on a sparse
  vhdx with the same error as `Optimize-VHD` (inferred from source; not run).
- It does not need admin (it impersonates the user).

### 2.8 Clearing the sparse flag is dangerous on a nearly full C:

`set-sparse false` calls `FSCTL_SET_SPARSE` with `FALSE`. NTFS then allocates
every hole in the file. Measured today on a 64 MB test file: 0 MB allocated
when sparse, 64 MB allocated right after clearing the flag.

On this PC that means: 268.1 GiB length - 200.9 GiB allocated = **about 67 GiB
of new allocation, with 15.2 GiB free on C:**. The step "set-sparse false,
then Optimize-VHD, then set-sparse true" would fill C: before Optimize-VHD
starts. **Do not run `set-sparse false` on this PC now.** hardcore must never
clear the sparse flag unless C: free is more than (length - allocated) plus a
margin.

## 3. Why sparse + fstrim did not free space here

Short answer, in order of weight:

1. **"Size" is the wrong number.** A sparse file keeps its length. The 268.1
   GiB is the length. "Size on disk" is 200.9 GiB now. So holes of about 67
   GiB exist in the file. Check "Size on disk", not "Size", and check C: free.
2. **The blocks were already marked free before the file became sparse.**
   Online discard and the first `fstrim` ran while the file was not sparse.
   The VHDX driver marked those blocks unused in its block table, but NTFS
   kept the clusters, because a non-sparse file cannot have holes. The second
   `fstrim`, after `set-sparse true`, sent discards for blocks that the table
   already shows as unused. The driver had no state change to act on, so it
   punched no new holes. This is an inference. It agrees with the user reports
   in section 2.3 (works on new sparse disks, fails on converted ones) and with
   the 36 GiB that is still allocated but unused. Microsoft has not published
   how the driver handles this.
3. **Block granularity.** WSL creates the vhdx with 1 MB blocks
   (`WslCoreFilesystem.cpp`, `BlockSizeInBytes = _1MB`). ext4 uses 4 KB blocks.
   One live 4 KB block keeps its whole 1 MB block. The 48,571 allocated ranges
   show a fragmented file, so some of the 36 GiB is in partly used blocks.
   Compaction cannot free those either. Only a full copy of the files
   (`wsl --export` and `--import`) can.

What is **not** the cause:

- A missing guest option. `discard` is on (section 2.1).
- A missing WSL restart. `set-sparse` itself needs the distro stopped, so the
  distro restarted after it.
- A minimum WSL version for sparse. 2.7.3 sets the flag correctly. The flag is
  set (`fsutil` says so).

The fix for the 36 GiB: stop the distro, then compact the sparse file with
`CompactVirtualDisk` and flag `0x400`, with no admin and no sparse-flag change.
Section 8 says how to prove this first on a throwaway disk.

## 4. Detection

All of these run in hardcore.exe. They read files and the registry. They do
not start WSL.

### 4.1 Find each vhdx

`HKCU\Software\Microsoft\Windows\CurrentVersion\Lxss\{GUID}` has, per distro:

- `DistributionName` (for example `Debian`)
- `BasePath` (for example `C:\Users\willi\AppData\Local\wsl\{466db190-...}`)
- `VhdFileName` (`ext4.vhdx`; use `ext4.vhdx` when the value is missing)
- `Version` (2 = WSL 2; skip 1, which has no vhdx)
- `Flags` (bit `0x8` = WSL 2 VM mode, as `LXSS_DISTRO_FLAGS_VM_MODE`)

This is the Microsoft-documented way
([disk-space](https://learn.microsoft.com/en-us/windows/wsl/disk-space)).
`wsl_distros()` in `hardcore.c` already opens this key. It must also keep
`BasePath`, `VhdFileName` and `Flags`.

Also watch, but never compact: `%TEMP%\*\swap.vhdx` (or `.wslconfig`
`swapFile=`), and `%TEMP%\wsl-crashes` (or `crashDumpFolder=`). Docker
Desktop's disks are not in this key. hardcore does not touch them.

### 4.2 Sizes

| Value | API | Note |
|---|---|---|
| length L | `GetFileAttributesExW` | Explorer "Size" |
| allocated A | `GetCompressedFileSizeW` | Explorer "Size on disk"; correct for sparse files |
| sparse | `FILE_ATTRIBUTE_SPARSE_FILE` | |
| compressed or encrypted | `FILE_ATTRIBUTE_COMPRESSED`, `_ENCRYPTED` | bad state; warn only (see WSL#41134 in the bus-error issue) |
| volume free F | `GetDiskFreeSpaceExW(BasePath)` | the drive of the vhdx, not `%LOCALAPPDATA%` |
| Linux used U | `df -B1 --output=used /` through `wsl.exe -d <distro> -e` | only when a hardcore WSL tab of that distro runs; cache the last value with its time |

Today `check_disk()` measures the drive of `%LOCALAPPDATA%\wsl`. That is wrong
when a distro lives on another drive (`wsl --manage --move`, or
`distributionInstallPath`). It must measure the drive of each `BasePath`.

### 4.3 Slack and thresholds

Slack S = A - U. This is the space that compaction can give back at most.

| State | Rule (defaults) | What hardcore does |
|---|---|---|
| fine | F >= 50 GB, or S < 10 GB | nothing visible |
| worth it | S >= max(10 GB, 10% of U) and F < 50 GB | quiet line in the tab column; offer when idle |
| low | F < 20 GB (existing `DISK_WARN_GB`) | amber card, offer now |
| danger | F < 5 GB (existing `DISK_BAD_GB`) | red card, offer now, also before each new tab |

The thresholds are `#define`s now. Make them config keys only if the owner
wants (section 7).

## 5. Tiers of action

### Tier A: no admin, no stop (automatic)

1. **`fstrim`.** Run `wsl.exe -d <distro> -u root -e fstrim /` with no
   window (the `claude_resync_thread` pattern). Run it at most once per day,
   and only while a hardcore WSL tab of that distro runs, so it never starts
   WSL. Run it once more just before each compaction. It is cheap insurance:
   with `discard` on, it usually changes little.
2. **Crash dumps.** If `%TEMP%\wsl-crashes` holds more than 1 GB, show it in
   the card. One key deletes them. They are Windows files, so WSL keeps
   running. Never delete them without the key.
3. **Measure.** Sizes from 4.2, every 60 s (the existing `TIMER_DISK`).

### Tier B: stop the distro, no admin (one key)

This is the step that gives space back.

1. Save the session (`session_save`). Tabs come back after a stopped WSL
   already (README, "Close it, open it, and your tabs come back").
2. Run `fstrim` (tier A).
3. Stop **only that distro**: `wsl --terminate <distro>`. Never
   `wsl --shutdown`.
4. Wait until hardcore can open the vhdx itself. The open fails with
   `ERROR_SHARING_VIOLATION` while WSL holds it. **Measured (section 12):
   WSL 2.7.3 keeps a stopped distro's vhdx attached to the Linux VM until the
   VM stops.** The VM stops `vmIdleTimeout` (default 60 s) after its last
   distro stops. So hardcore refuses when another distro (or Docker's) runs,
   and waits `vmIdleTimeout` + 120 s (180 s by default).
5. Open with `OpenVirtualDisk(VIRTUAL_DISK_ACCESS_GET_INFO |
   VIRTUAL_DISK_ACCESS_METAOPS, OPEN_VIRTUAL_DISK_FLAG_SUPPORT_SPARSE_FILES_ANY_FS
   when sparse)`. Keep the handle open for the whole step. While hardcore holds
   it, WSL cannot start that distro, so nothing writes into the disk during
   compaction.
6. Call `CompactVirtualDisk` with an `OVERLAPPED`. Poll
   `GetVirtualDiskOperationProgress` every second. Show the percent in the
   card.
7. Watch C: free during the run. If it falls under 2 GB, cancel
   (`CancelIoEx`). Microsoft says an interrupted compaction is safe and can run
   again.
8. Close the handle. Measure L, A and F again. Write one log line.
9. Restart the tabs of that distro (the same path as "Enter retries" after a
   failed start), or leave them stopped with a line that says why, by choice.

hardcore links `virtdisk.dll` at run time with `LoadLibrary`, like it loads
ConPTY. mingw also has `libvirtdisk.a` and `virtdisk.h` with the needed names.

hardcore does not use `wsl --manage --compact` (2.7 lacks it, it kills the
distro by itself, and it likely fails on sparse disks). If a later WSL fixes
the sparse case, hardcore can call it in place of steps 3 to 7, after its own
busy check. The offline `e2fsck -E discard` in it is useful, so watch for that.

### Tier C: admin (not needed)

Compaction does not need admin (section 2.6). So hardcore asks for no UAC
prompt in the normal path. Admin is only for things hardcore does not do by
itself:

- `Optimize-VHD` (Hyper-V module; refuses sparse files).
- `diskpart` `attach vdisk readonly` + `compact vdisk` (file-system-aware
  mode; no gain for ext4).

If a later step needs admin, use the existing pattern in `relaunch_elevated()`:
`ShellExecuteExW` with `runas` on `hardcore.exe` and a narrow subcommand (for
example `hardcore disk compact --vhd PATH --status FILE`). One UAC prompt. The
helper must take the vhdx path as an argument, because an elevated process of a
different admin account reads a different `HKCU`. On this PC `elevated=1` is
set in `config.ini`, so hardcore already runs elevated here; the design must not
depend on that.

### What never happens automatically

- `wsl --shutdown`.
- `set-sparse false` or `set-sparse true`.
- Any write to `.wslconfig`.
- `wsl --export`, `--import`, `--unregister`, `--move`, `--resize`.

## 6. Busy check and consent

A stop kills every process in that distro. Claude Code sessions resume after a
restart, but a turn in progress, a build, a server or an agent job does not.

hardcore calls a distro **busy** when any of these is true:

1. A hardcore tab of that distro runs a command. rc.bash reports `cmd=` with a
   command, and an empty `cmd=` at the prompt (`Term.cmd` in `hardcore.c`).
2. A hardcore tab of that distro runs Claude (`Term.claude` is set), or its
   throbber runs (`tb->busy`).
3. Another hardcore window has such a tab. Ask each window over the existing
   pipe `\\.\pipe\hardcore-<SID>` (a new message, for example
   `HC_PIPE_BUSY`).
4. A `wsl.exe` or `wslhost.exe` process runs that is not a child of a hardcore
   process (Toolhelp32 snapshot). Examples: Windows Terminal, VS Code Remote.
5. Inside Linux, a process runs that is not a hardcore shell, not systemd and
   not a kernel thread: one hidden `ps -eo pid,ppid,tty,stat,etimes,comm` while
   the distro already runs. Load average over 0.5 also counts.

The card always shows what will stop, in plain words, for example:
"This stops Linux for about 5 minutes. 2 tabs are running programs: `make`,
`claude`. They will stop."

Consent modes (config key `reclaim=`, section 7):

- `ask` (default): hardcore never stops Linux by itself. The card offers a key.
  If something is busy, the key asks again with the list.
- `idle`: hardcore may stop Linux by itself, only when all of these are true:
  nothing is busy (all 5 checks), no keyboard or mouse input for 30 minutes
  (`GetLastInputInfo`), the PC is on AC power or above 50% battery, and the
  rule in 4.3 says "worth it" or worse. It shows a 60-second countdown that any
  key cancels.
- `warn`: only the card; no stop key.
- `off`: no checks at all, except the existing low-disk line.

## 7. What the user sees

The existing warning sits at the bottom of the tab column. It grows into a
small card. Plain words, no file names.

Low (amber):

```
C: 15.2 GB free
Linux uses 165 GB. Its disk
file holds 201 GB on C:.
R  give back about 36 GB
   (stops Linux ~5 min)
L  later, when idle
```

While it runs:

```
Giving space back to C: 42%
Linux is stopped. Tabs come
back when this ends.
```

After:

```
Gave back 31 GB. C: 46 GB free.
```

Failure:

```
Could not give space back.
Linux is running again.
Details: hardcore disk log
```

Danger (red, under 5 GB): the same card, plus a line before each new tab:
"C: is almost full. Linux stops when C: fills."

Keys work when the mouse is on the card or when the tab column has focus. A
click on a line does the same as its key. `N` hides the offer for 7 days for
that disk.

Command line (for the owner and for tests):

```
hardcore disk                 # sizes, slack, state, last result, per distro
hardcore disk trim            # tier A now
hardcore disk compact [--distro NAME] [--yes] [--dry-run]
hardcore disk log             # the last 50 log lines
```

`--dry-run` prints each step and its checks, and changes nothing.
Built as well: `hardcore disk cap [--size GB]` and `hardcore disk dumps
[--delete]`.

Config keys in `config.ini` (hidden, like `elevated=`; PRODUCT.md promises
"no settings beyond a font and a color scheme" in the panel):

```
# reclaim=ask: offer to give WSL disk space back to Windows; idle = also do it
# when the PC is idle and nothing runs; warn = only show it; off = never
reclaim=
```

Environment `HARDCORE_RECLAIM=off` overrides it (tests and emergencies).

macOS and Linux builds: no vhdx, so the feature is not built
(`#ifdef _WIN32`).

## 8. Safety

Rules:

1. hardcore never writes inside the vhdx with its own code. It uses only the
   Windows virtual disk API, the same one that WSL and `Optimize-VHD` use.
2. hardcore compacts only when it holds the vhdx open itself. So WSL cannot
   run that distro during the step.
3. hardcore never clears the sparse flag. A future manual command may do it
   only when C: free > (L - A) + 10 GB.
4. hardcore never stops Linux without a key, except in `idle` mode with all
   the checks in section 6.
5. hardcore checks before it starts: F >= 2 GB, the file is not compressed or
   encrypted, the distro is WSL 2, the file is `.vhdx` (not `.vhd`).
6. One step at a time, per PC: a lock file `%LOCALAPPDATA%\hardcore\disk.lock`
   with the PID, so two windows do not both compact.
7. State file `%LOCALAPPDATA%\hardcore\disk-state.json`: last sizes, last
   trim, last compaction, "running since". If hardcore starts and finds
   "running since" without an end, it shows: "The last disk cleanup stopped
   early. Linux files are safe. Run it again?" (the compaction is
   restartable per Microsoft).
8. On any error: close the handle, release the lock, restart the tabs, show
   the failure card, log the Win32 error. Do not retry for 7 days, except on a
   key.
9. Log `%LOCALAPPDATA%\hardcore\disk-reclaim.log`: one line per action with
   time, distro, L, A, F, U, slack, duration, result. Keep 1 MB, then rotate
   once.

Backup: there is no room on this PC for a copy (one drive, 15 GB free). The
design relies on the documented safe interruption of `CompactVirtualDisk`. If
another drive has room for A, the card can offer `wsl --export` first as an
option, not by default.

## 9. Where it goes in the code

| Part | Place |
|---|---|
| vhdx list per distro (BasePath, VhdFileName, Flags) | extend `wsl_distros()` in `hardcore.c` |
| sizes, slack, rules, state file, log, lock | new `hardcore_win_disk.h`, included in the `_WIN32` part of `hardcore.c`, like `hardcore_win_menu.h` |
| measure per vhdx drive | `check_disk()` (`TIMER_DISK`, 60 s) calls into `hardcore_win_disk.h` |
| hidden `wsl.exe` runs (`df`, `fstrim`, `ps`) | factor `claude_resync_thread()` into a general "run hidden, capture, timeout" helper |
| busy check | tabs: `Term.cmd`, `Term.claude`, `tb->busy`; other windows: new pipe message beside `HC_PIPE_SPAWN`; other WSL clients: Toolhelp32 |
| compaction thread | `hardcore_win_disk.h`; posts `WM_APP_DISK` progress to the window |
| card and keys | `render_disk_warning()`; key and click handling next to the sidebar hit tests |
| tab restart after | the `HELP_FAILED` / "Enter retries" path |
| CLI `hardcore disk ...` | `wWinMain` argument loop, next to `update` and `upgrade` |
| config `reclaim=` | `config_load()` / `config_save()` |
| docs | README "Warns before the disk fills"; update `issues/wsl-bus-errors.md` |

## 10. Tests that never touch the real vhdx

1. **Rules (pure C).** `tests/test_disk_policy.c`, like `tests/test_reflow.c`.
   Input: L, A, U, F, busy flags, mode, idle time, power. Output: state and
   action. Covers every row of 4.3 and section 6, and the sparse-clear guard
   (C: free 15 GB, holes 67 GB: refuse).
2. **Fake sizes.** `HARDCORE_DISK_FAKE=FILE` (JSON with L, A, U, F per
   distro) replaces the measurements. With `HARDCORE_TEST_CONFIG`, a test
   window draws each card. Screenshot checks like the existing chrome tests.
3. **Fake vhdx, no admin.** `tests/test_disk_compact.py` makes a 64 MB VHDX
   in a temp folder with `CreateVirtualDisk` (sparse and not), then runs
   `hardcore disk compact --vhd PATH --yes`. Checks: exit 0, sparse flag
   unchanged, log line written. Also: an open handle held by the test makes
   hardcore report "in use" and change nothing. A read-only file makes it fail
   cleanly. A kill of hardcore during the run leaves a state file that the
   next start reports.
4. **Dry run.** `hardcore disk compact --dry-run` against the real Debian
   entry prints the steps and changes nothing (check L, A and mtime before and
   after).
5. **Real data, throwaway distro (manual lane, needs owner OK).** Import a
   small rootfs as `hardcore-disk-test` into a temp folder (`wsl --import`
   needs no admin). Set sparse. Write 2 GB, delete it, `fstrim`. Terminate it.
   Run `hardcore disk compact --distro hardcore-disk-test`. Measure A and peak
   C: use. Repeat with "write, restart, delete". Then `wsl --unregister
   hardcore-disk-test`. This answers the two open measurements: real gain on a
   sparse disk, and extra space during the run.

## 11. The owner's choices (2026-11-11)

1. **Default mode:** `ask`. hardcore never stops Linux without a keypress.
2. **Sparse:** hardcore never changes the sparse flag.
3. **Tab restart after a compaction:** the tabs start again by themselves.
4. **Cap growth:** the card offers `C`, which runs
   `wsl --manage <distro> --resize <N>GB` after the same stop. N = A + F - 5 GB,
   in whole GB, and only when Linux keeps at least 10 GB of room over U.
5. **Crash dumps:** the card offers deletion only (`D`). hardcore never writes
   `.wslconfig`.
6. **Thresholds:** fixed in code (`hardcore_disk_policy.h`).
7. **The throwaway-distro test:** approved and done (section 12).

## 12. The throwaway-distro test (2026-11-11, WSL 2.7.3)

Setup: Alpine minirootfs 3.20.3 imported as `hc-reclaim-test` into
`C:\Users\willi\wsl-test\`. Debian ran the whole time. Sizes from
`hardcore disk compact --vhd PATH --dry-run` (length L, allocation A). After
the test: both test distros unregistered, the folder deleted.

| Step | L | A on C: |
|---|---|---|
| Fresh import (not sparse) | 0.07 GB | 0.07 GB |
| Write 2 GB (random data) | 2.07 GB | 2.07 GB |
| Delete it (online discard) | 2.07 GB | 2.07 GB |
| `fstrim` | 2.07 GB | 2.07 GB |
| Write files a, b, c (1 GB each), delete a and c, `fstrim` (Linux uses 1.0 GB) | 3.07 GB | 3.07 GB |

Compaction (hardcore's own code: `CompactVirtualDisk`, no admin), on copies of
that 3.07 GB file, because WSL still held the file itself (see below):

| Copy | Before | After | Freed | Time | Extra C: space during the run |
|---|---|---|---|---|---|
| Not sparse | 3.07 GB | 1.07 GB | 2.01 GB | 3.4 s | 0.00 GB |
| Sparse (`fsutil sparse setflag`, no admin; the Debian case: trimmed before sparse) | 3.07 GB | 1.07 GB | 2.01 GB | 3.4 s | 0.00 GB |
| Not sparse, all data deleted (earlier copy) | 2.07 GB | 0.07 GB | 2.01 GB | 0.6 s | 0.00 GB |

- C: free was sampled 4 times a second during each run. It never fell below
  its value at the start. Compaction moves blocks inside the file and then
  cuts the file shorter, so it needs no extra space.
- The sparse copy stayed sparse, with one allocated range after the run. Both
  L and A went down.
- Data check: the compacted sparse copy, imported with `--import-in-place`,
  had the same `md5sum` for the kept 1 GB file as the original.
- Online discard on a sparse disk works on this PC: on the compacted sparse
  copy, writing 1 GB added 1.0 GB on C:, and `rm` gave 0.97 GB back within
  3 s, with no compaction. So after one compaction, Debian (sparse) gives
  deleted space back by itself. This agrees with section 3, point 2.
- `wsl --manage --set-sparse true` on WSL 2.7.3 refuses without
  `--allow-unsafe`.

**The important finding: `wsl --terminate` does not release the disk file.**
After `wsl --terminate hc-reclaim-test`, `wsl -l -v` said "Stopped" and the
kernel log said "EXT4-fs (sde): unmounting filesystem", but the vhdx stayed
held by `System` (PID 4, Restart Manager query) for more than 2 minutes. In
that state:

- `OpenVirtualDisk` fails with error 32 (`ERROR_SHARING_VIOLATION`).
- `wsl --manage hc-reclaim-test --set-sparse true` fails with
  `ERROR_SHARING_VIOLATION` and says to run `wsl --shutdown`.
- `wsl --manage hc-reclaim-test --resize 8GB` fails with
  `WSL_E_DISTRO_NOT_STOPPED`.
- `wsl --unregister` releases and deletes it.

So WSL keeps every distro's disk attached to the one Linux VM until the VM
stops. The VM stops only when no distro runs (after `vmIdleTimeout`). The
full `hardcore disk compact --distro hc-reclaim-test --yes` run (with Debian
running) did fstrim, terminate, waited 15 s, then failed cleanly with "The
disk file is in use" and changed nothing (16.4 s). hardcore now refuses up
front when another distro runs, and says why.

For Debian this is not a problem when Debian is the only distro: when Debian
stops, the VM stops about a minute later, and the file is free.

## 13. Compacting Debian later (no agents running)

hardcore must run from Windows, not from inside Debian: a step that stops
Debian stops the shell that runs it, and `hardcore disk compact` refuses
from inside Debian. All Claude agents and builds stop with Debian.

1. Stop every agent and long job. Save work. Close other WSL windows
   (VS Code Remote, Windows Terminal tabs on Debian).
2. Check that Debian is the only distro: `wsl -l -v` (no other "Running").
3. From PowerShell (not a WSL tab), dry run first:
   `& "$env:LOCALAPPDATA\hardcore\hardcore.exe" disk compact --distro Debian --dry-run`
   (use the branch build's `hardcore.exe` until the branch is merged and
   installed). It prints the steps and what will stop, and changes nothing.
4. Run it: `... disk compact --distro Debian --yes`. It runs fstrim, stops
   Debian (`wsl --terminate Debian`), waits up to 180 s for the VM to stop and
   WSL to let go of the file, compacts with open flag 0x400 (the sparse flag
   stays), and prints the space it gave back and the lowest C: free during the
   run. Expected: about 36 GB back (A 201 GB to about 165 GB), a few minutes,
   no extra C: space. If C: falls under 2 GB free, it stops the run by itself
   (safe; run it again later).
5. Or, from a hardcore window after the branch is installed: the card shows
   "R give back about 36 GB". R shows what stops, R again runs it, and the
   tabs start again when it ends.
6. Then: `hardcore disk` shows the new sizes; `hardcore disk log` shows the
   run. Never run `set-sparse false` on Debian (section 2.8).

## 14. What is built (branch feat/wsl-disk-reclaim)

- `hardcore_disk_policy.h`: the rules in pure C (states, actions per mode,
  preflight, sparse-clear guard, cap size). Tested by
  `tests/test_disk_policy.c` (54 checks, `make reflow-test` and
  `make disk-test`).
- `hardcore_win_disk.h`: the distro list with `BasePath`, `VhdFileName`,
  `Flags` (used by `wsl_distros()`); sizes per disk file and per drive;
  `HARDCORE_DISK_FAKE`; the lock (`disk.lock`, removed when its process
  ends), `disk-state.json`, `disk-reclaim.log` (1 MB, one rotation); hidden
  `wsl.exe` runs (df, load and programs on a terminal, fstrim, terminate); the
  WSL client check (Toolhelp + command lines: which `wsl.exe` opens which
  distro, which ones are hardcore tabs, and whether this process runs inside
  the distro); crash dumps; `CompactVirtualDisk` with progress, a 2 GB floor
  and cancel; the `hardcore disk` command (status, trim, compact, cap, dumps,
  log).
- `hardcore.c`: `check_disk()` per disk-file drive, the card in
  `render_disk_warning()` (keys R, L, C, D, N, Esc, and clicks), the job
  thread, tab hold and restart (also in other hardcore windows), no spare
  shell during a step, `reclaim=` in `config.ini`, `HARDCORE_RECLAIM`.
- `tests/test_disk_reclaim_windows.py` (49 checks): small VHDX files made
  without admin, plain and sparse, without `--yes`, held open, read-only,
  killed in the middle; the dry run against Debian (size and mtime unchanged);
  the card in an off-screen window with fake sizes, which never stops the
  real Linux.

Not built from the design: asking other windows over the pipe (other windows
get `WM_APP_DISK_HOLD` and check the lock file instead; the Linux process
list covers their programs), and the busy check does not look at processes
without a terminal (the load average stands in for them).

## Sources

- Microsoft, [Advanced settings configuration in WSL](https://learn.microsoft.com/en-us/windows/wsl/wsl-config) (sparseVhd, autoMemoryReclaim, defaultVhdSize, crash dumps)
- Microsoft, [How to manage WSL disk space](https://learn.microsoft.com/en-us/windows/wsl/disk-space) (vhdx location from the Lxss key)
- Microsoft, [CompactVirtualDisk](https://learn.microsoft.com/en-us/windows/win32/api/virtdisk/nf-virtdisk-compactvirtualdisk)
- Microsoft, [Optimize-VHD](https://learn.microsoft.com/en-us/powershell/module/hyper-v/optimize-vhd)
- Microsoft, [FSCTL_SET_SPARSE](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_set_sparse)
- Microsoft Q&A, [sparse vhd does not shrink](https://learn.microsoft.com/en-za/answers/questions/1526083/in-wsl2-with-sparse-vhd-the-storage-usage-does-not)
- microsoft/WSL issues [#4699](https://github.com/microsoft/WSL/issues/4699), [#10609](https://github.com/microsoft/WSL/issues/10609), [#10703](https://github.com/microsoft/WSL/issues/10703), [#12103](https://github.com/microsoft/WSL/issues/12103), [#13075](https://github.com/microsoft/WSL/issues/13075), [#13241](https://github.com/microsoft/WSL/issues/13241)
- microsoft/WSL pull requests [#40481](https://github.com/microsoft/WSL/pull/40481) (`--compact`), [#41726](https://github.com/microsoft/WSL/pull/41726) (sparse re-enabled), release notes 3.0.1 to 3.0.5
- microsoft/WSL source on main, 2026-11-11 (commit 6e43d80): `WslCoreVm.cpp` (mount options), `LxssUserSession.cpp` (`SetSparse`, `CompactDistribution`), `WslCoreFilesystem.cpp` (`CreateVhd`, `OpenVhd`, `CompactVhd`), `src/linux/init/main.cpp` (offline `e2fsck -E discard`)
