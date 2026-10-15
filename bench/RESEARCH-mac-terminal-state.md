# Preserving Terminal.app presentation and existing shell jobs

Research date: 2026-10-15. This round used primary documentation, Darwin/zsh
source, and read-only inspection of the installed Terminal 2.15 (470) bundle.
It did not send Apple events, inspect Terminal through Accessibility, read the
user's clipboard or saved sessions, or operate Terminal's UI. The successful
user-operated Claude handoff is separate evidence; it does not establish any
of the presentation or shell-job transfers below.

## The most useful next presentation experiment

Terminal has a richer export path than its scripting dictionary: styled
copy. Apple documents ordinary Copy, Copy Without Background Color, Copy
Plain Text, Select All, and separate text-export commands in its
[keyboard-shortcut reference](https://support.apple.com/guide/terminal/keyboard-shortcuts-trmlshtcts/mac).
Read-only inspection of the shipping executable finds
`copySelectedAttributedText:withProfile:`, `selectedAttributedTextWithProfile:`,
`RTFFromRange:documentAttributes:`, `copyPlainText:`, and
`copyWithoutBackgroundColor:`. These are evidence of an attributed/RTF export
implementation, not proof that every terminal cell property survives it.

A small user-operated experiment can measure that coverage without automating
Terminal:

1. Print a disposable, uniquely identified fixture with known foreground and
   background colors, bold/italic/underline, Unicode combining characters,
   wide glyphs, hyperlinks, blank colored cells, and a long wrapped line.
   Include more lines than the visible viewport.
2. Ask the user to select all and use ordinary Copy in that Terminal window.
3. After the user explicitly requests capture, save the RTF and plain-text
   representations from `NSPasteboard` into a private new directory. Record
   available types and reject a pasteboard that lacks the fixture token.
4. Decode the RTF using `NSAttributedString`, compare text and attribute runs
   with the known fixture, and render a static archive in Hardcore. Preserve
   the original RTF alongside a normalized representation for diagnostics.
5. Repeat with changed width, truecolor, reverse video, alternate-screen
   content, and output around marks. Record missing information explicitly.

This can become a useful imported history archive even before the remaining
shell migration work succeeds. It must remain display data: never feed an
untrusted exported transcript back into a terminal parser as executable
escape sequences or send it to a PTY as input. The importer should decode
text/style runs into cells and retain source/capture metadata.

One user-operated capture now establishes the limited RTF coverage recorded
below. This is **not full terminal-state preservation**. In particular, a
formatted string does not automatically identify soft-wrap boundaries,
double-width continuation cells, empty cells with attributes, the current
cursor, parser partial input, tab stops, scrolling margins, alternate-screen
state, keyboard/mouse modes, or selection. Foreground/background colors may
be resolved RGB values rather than palette references. A copied hyperlink
may or may not survive as a link attribute. The importer should not claim
full emulator preservation or authorize source retirement from this archive.

### Implemented offline decoder and manual fixture

The experimental helper is now implemented in
[`tools/mac_terminal_archive.m`](../tools/mac_terminal_archive.m), with a
user-operated fixture in
[`tools/manual_mac_terminal_archive.py`](../tools/manual_mac_terminal_archive.py).
Run this manually in a fresh Terminal window:

```sh
python3 tools/manual_mac_terminal_archive.py
```

It prints the fixture, asks the user to press Command-A then ordinary Command-C,
and captures the pasteboard only after the user presses Return. All generated
artifacts go into a new mode-0700 directory under `/private/tmp`; archived files
are mode 0600. The fresh-window requirement matters because original RTF/text
include the surrounding text selected by the user. Normalized text/style runs
are restricted to the uniquely identified fixture region. The helper rejects
missing or repeated marker lines. It compares the independently located fixture
ranges literally, including their final newline, and records every difference.
Canonical composition differences may be reconciled through the separate
verified style mapping described below; other differences are refused.
Differences outside the fixture are recorded separately and do
not invalidate a fixture-only archive. No whole-history preservation is claimed.

Both representations come from the same identified `NSPasteboardItem`; multiple
matching items are refused. This avoids the different semantics of the board's
convenience APIs: string retrieval can concatenate items while data retrieval
returns the first matching item. Item index/count are recorded, and both values
are re-read to detect provider changes, in addition to checking `changeCount`.
[Apple NSPasteboard documentation](https://developer.apple.com/documentation/appkit/nspasteboard)

The decoder accepts RTF explicitly, with acceptance limits of eight MiB per
input, one million UTF-16 code units, 100,000 style runs, and 128 nesting levels.
Offline file reads are bounded before decoding. `NSPasteboard` materializes
clipboard data before the helper can check its size, so these acceptance limits
are not a hard bound on peak memory use during clipboard capture.
Objects, images, binary RTF payloads, and attachments are refused. Control
and directional formatting characters are converted to inert visible/replacement
characters without changing UTF-16 offsets. Link targets remain inactive
metadata. Colors are resolved to sRGB; AppKit color management can change
numeric values from an RTF calibrated color, so raw RTF remains available.
Unknown attributes and absent terminal-state fields are reported explicitly.

Offline verification:

```sh
python3 tests/test_mac_terminal_archive.py
python3 tools/manual_mac_terminal_archive.py --prepare-only
```

On this Mac, all 28 archive integration tests, 24 manual-workflow tests, and
22 native selftest checks passed,
including exact original retention, permissions, color attributes, Unicode,
control sanitization, mismatched/duplicate identity refusal, input size/nesting
limits, embedded-object refusal, and symlink/overwrite refusal. Review also
verified that escaped literal RTF-like text is accepted without being mistaken
for an embedded payload, and that a missing final newline fails exact fixture
comparison. The preparation
run at `/private/tmp/hc-terminal-archive-8sfpffc6` compiled the helper and saved
the fixture without reading the clipboard. These checks prove the decoder and
fixture handling, not Terminal's actual copy coverage.

### First manual archive capture: mismatch, not established data loss

The user ran the fixture in an existing Terminal window on 2026-10-15, retaining
the prior successful Claude handoff output. The run at
`/private/tmp/hc-terminal-archive-2j2cy25a` reached RTF decoding but refused with
`RTF text and plain text differ`. The old helper discarded the rejected copies;
only fixture inputs and the old compiled helper remain. Consequently neither
the mismatch location nor its cause can be determined from this run. Existing
window history, line/Unicode conventions and multi-item retrieval are possible
leads, not established explanations.

The revised helper preserves bounded, privately stored originals,
`decoded-rtf.txt`, and `comparison.json` whenever both decoded representations
have independently verified fixture identity. An unreconciled mismatch saves no
normalized archive and returns failure. The report includes whole-selection/fixture
equality, independent UTF-16 ranges, lengths, first mismatch and at most eight
code units from each fixture at that position. CRLF and canonical Unicode
equivalence never turn the literal comparison into an exact-match result.
Recognized line terminators at marker boundaries allow diagnosis without
silently changing their contents. Wrong, missing or duplicate identities still
save nothing.

`--recapture RUN_DIRECTORY` compiles the current helper in a new private attempt
directory and guides another user-operated copy of the existing fixture. It
validates the saved known fixture, never overwrites previous captures, and reads
the clipboard only after the user's Return. The Python report also requires the
captured fixture to equal its expected text; equality between two equally
truncated representations is insufficient.

### Fresh Terminal capture: canonical composition in the RTF exporter

The fresh-window run at `/private/tmp/hc-terminal-archive-bnqxoii3` retained both
representations. Its plain fixture exactly matches the expected 2,951 UTF-16
units. Its decoded RTF has 2,950 units, with one difference at fixture offset
165: plain `U+0065 U+0301` versus RTF `U+00E9`. Both strings are canonically
equivalent. The original RTF itself contains `\\'e9` for this second accented
character, so Terminal's export already composed it; the AppKit decoder did
not introduce this particular change. The capture contains one clipboard item,
and the two representations were paired from that item. An old window and
multi-item retrieval are not explanations of this fresh run's mismatch.

The decoder now keeps the exact plain-text representation and maps the RTF's
styles onto it. It requires canonical equivalence globally and for every
corresponding composed-character sequence. Changed graphemes must have one
uniform complete RTF attribute dictionary. Unchanged graphemes retain all
internal attribute boundaries. The mapped string is checked for literal UTF-16
equality with plain text before its output style ranges are enumerated. Newline
changes, compatibility-only normalization and ambiguous within-grapheme style
changes remain refusals. Unicode's default grapheme boundaries are stable under
canonical normalization. [Unicode UAX #29](https://unicode.org/reports/tr29/)

The raw comparison remains `exact:false`; `styleMapping` separately records
`canonical-grapheme`, the number of remapped clusters, `textSource:plain`, and
`styleSource:RTF`. This reconciles the available representations. It cannot
recover per-cell or within-grapheme styling already discarded by Terminal's
export, nor establish complete terminal-state preservation. The manual wrapper
also requires that the archived plain text equal the known fixture and that
mapping verification explicitly succeed.

Saved captures can now be processed without another copy or clipboard read:

```sh
python3 tools/manual_mac_terminal_archive.py --decode-capture CAPTURE_DIRECTORY
```

The offline path validates the private original fixture and capture files,
compiles the current helper, and writes a new attempt without replacing the
original evidence. Numeric `0`/`1` diagnostic flags from older helpers are also
recognized in summaries; those values previously hid the normalization clue.

The saved real capture was successfully decoded offline at
`/private/tmp/hc-terminal-archive-bnqxoii3/attempt-k1oln8aa/coverage.json`.
Verification preserved all 2,951 fixture UTF-16 units exactly, all 64 history
rows, the combining sequence, emoji and wide glyph. One grapheme was mapped;
the report retains `fixtureRepresentationsExact:false` while confirming
`fixtureTextExact:true` and `styleMappingVerified:true`. The exported attributes
include foreground/background, bold, italic, underline and colored blanks.
The truecolor sample resolves to sRGB `[0.06667, 0.6, 0.33333, 1]`, matching the
fixture's 17/153/85 channels to the exported precision. The wrapped test line's
text also matches, without establishing its soft-wrap cell flags.

The hyperlink label survives, but no link target is represented in this RTF.
`NSLigature` and `NSParagraphStyle` remain unrepresented attribute metadata.
Attribute presence is not an exhaustive proof of original visual fidelity.
No screen cursor, modes, active jobs, shell memory or window closure was tested
by this offline decode. No clipboard access or new user-operated capture was
needed, and original evidence was preserved.

### Additional verification without another copy operation

`tools/verify_mac_slurp.py --offline` runs the deterministic suites and a seeded
RTF robustness corpus, and replays a validated saved capture through a private
snapshot. The combined run at
`/private/tmp/hc-slurp-verification-visye5dd/report.json` passed 65 robustness
cases, including deliberate mutations of the saved fixture. No clipboard read
or new Terminal selection was involved. These are further parser and mapping
checks against the existing evidence, not fresh Terminal export coverage.

Without `--offline`, the same script also runs owned shell-state and native
Hardcore receiver probes without input prompts. It retains stage-specific logs,
timeouts, failures and explicit exclusions in a private report. The manual
archive wrapper also has bounded compile/selftest/decode waits and retains a
timeout diagnostic instead of accepting an unverified archive.

## What the other export routes actually offer

| Route | Evidence and practical use | Remaining limit |
| --- | --- | --- |
| Terminal scripting `history` / `contents` | Installed `Contents/Resources/Terminal.sdef` declares both as read-only `text`. `history` covers the scrolling buffer and `contents` the visible contents. Existing Hardcore bridge already uses this route. | No per-cell attributes or cursor position in the dictionary. Profile-wide colors and fonts do not recover individually styled cells. |
| Ordinary styled copy | The user-operated fixture exports text, colors, font traits and underline; canonical composition is reconciled while keeping exact plain text. | Hyperlink target is absent in the tested RTF; complete cell, cursor, mode and wrap state remain unproven. |
| Accessibility attributed range | Apple's [`kAXAttributedStringForRangeParameterizedAttribute`](https://developer.apple.com/documentation/applicationservices/kaxattributedstringforrangeparameterizedattribute?language=objc) returns attributed text when an accessibility object supports it. A future user-authorized product probe can first enumerate supported parameters. | Support is optional. Installed Terminal selectors show plain string/range handlers; this inspection did not establish attributed-range support. Do not assume it exists or silently downgrade to plain text. |
| Text export | Apple lists Export Text As and Export Selected Text As. Save the selected disposable fixture and inspect the actual format in a manual test. | The documented operation does not promise a terminal-state snapshot. A `.txt` export cannot establish colors, modes or cursor state. |
| `.terminal` profile / window group | Apple documents [profile export](https://support.apple.com/guide/terminal/import-and-export-terminal-profiles-trml4299c696/mac) and [window groups](https://support.apple.com/guide/terminal/use-window-groups-trml15652/mac). Groups retain window settings and can restore commands/connections. | These are configuration/layout documents, not documented process or memory snapshots. Re-running a saved command does not preserve the original process. Historical `.term` files need separate format evidence. |
| AppKit saved state | Installed Terminal contains `ShouldRestoreContent`, `ShouldLimitRestoreScrollback`, `RestoreScrollbackLines`, `RestoreScrollbackANSIColor`, and a diagnostic for truncated restored content. | Private storage/schema, capture timing and truncation must be established on a disposable user-provided sample. File existence or modification time cannot certify a complete current snapshot. |

Terminal also has a configurable [scrollback retention limit](https://support.apple.com/guide/terminal/limit-the-number-of-scrollback-rows-trml15233/mac).
Any exporter can promise at most the history still retained by the source;
data already evicted by that limit cannot be reconstructed by copying it.

Saved state deserves an offline parser experiment only after a user captures
an identified disposable fixture. Preserve the original bytes, use a bounded
data parser rather than instantiating arbitrary archived Objective-C classes,
and compare several controlled saves after text/style changes. It is a
recovery/archive lead, not currently a reliable automatic live handoff source.
Do not quit Terminal or alter restoration preferences merely to force a save.

## Why a cooperative same-PID shell change is harder than redirecting output

Darwin treats the controlling terminal, process session, process groups, open
descriptors, and a shell's internal job table as separate state. Redirecting
file descriptors 0/1/2 changes only those descriptors in the shell; already
running children retain their own descriptors and event registrations.

Current Apple kernel sources constrain the straightforward rebind route:

- [`tty.c`, `TIOCSCTTY`](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/tty.c)
  requires the caller to be a session leader and rejects acquisition if its
  session already has a controlling-terminal vnode or the new terminal
  belongs to another session. `TIOCSPGRP` requires a process group in the
  caller's same session.
- [`tty_tty.c`, `cttyioctl`](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/tty_tty.c)
  handles `TIOCNOTTY` on `/dev/tty` by clearing a nonleader's controlling-terminal
  flag. A session leader receives `EINVAL`; this is not a general detach
  operation for an existing interactive login shell.
- [`kern_prot.c`, `setsid_internal` and `setpgid`](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/kern_prot.c)
  rejects `setsid` for a process-group leader. `setpgid` cannot move processes
  between sessions or change a session leader, and a parent's ability to
  change a child's process group is restricted after that child has executed.

The existing [late zsh clone probe](RESEARCH-mac-zsh-clone.md) gets around the
session-leader constraint by forking a new process before acquiring a new
terminal. Upstream [`clone.c`](https://github.com/zsh-users/zsh/blob/master/Src/Modules/clone.c)
explicitly clears the copied job table and closes coprocess descriptors. Keeping
the copied job table instead would not make the old jobs children of the new
shell, move their session, redirect their I/O, or allow normal `wait` semantics.
That is why modifying only clone's job-table behavior is not a complete fix.

### A narrowly scoped remaining kernel experiment: revoke and reacquire

Apple's [`revoke` implementation](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/vfs/vfs_syscalls.c)
allows the device owner, or root, to invalidate references to a device. The
[special-device close path](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/miscfs/specfs/spec_vnops.c)
forces a device close on revocation, and `ttyclose` clears the session's terminal
association. Inference: an owned synthetic session leader might be able to
reacquire a fresh controlling terminal while retaining its PID/session after
revocation. This was **not executed or demonstrated** in this round.

This is unsuitable for a production handoff as currently understood. Revocation
invalidates all original device references rather than transferring them;
it can interrupt reads, flush pending I/O, revoke kqueue registrations, and
disconnect unrelated holders. Recovery to the exact prior state is not assured.
Each existing child still needs a cooperative rebind of every relevant descriptor
and event source. An external helper cannot invoke `TIOCSCTTY` on behalf of its
parent shell just because they share a session. A stock-shell in-process entry
point would still be needed. Only a disposable owned-PTY test with explicit
read/write, signal, child-wait and failure observations would justify pursuing
the idea further; never try it against the user's Terminal session.

## Late Claude adapter activation

A separate 2026-10-15 round checked the installed Claude Code 2.1.300 and an
owned disposable PTY, using private project settings and a private plugin
cache. It did not operate the user's Terminal session or send a model prompt.
Installing the observer plugin after startup did not load it during a five-second
observation period. With known composer focus and no existing stash, Claude's
Ctrl+S followed by `/reload-plugins` preserved the exact Unicode draft and its
middle cursor; the original frontend stayed alive. Private diagnostics were
saved at `/private/tmp/hc-claude-bootstrap-audit-x2pv4fw3/result.json`. The
repeatable broader probe is `python3 tests/probe_claude_bootstrap.py`.

This does not justify injecting that key sequence into an arbitrary session.
An existing stash, attachment, autocomplete, dialog or different focus can change
its meaning. The public prompt API cannot inspect all of that state before the
adapter loads. Hardcore can install the adapter and queue a request bound to the
exact live source; first activation still needs `/reload-plugins` in that source,
followed by a normally completed turn that establishes readiness. A previously
loaded adapter can accept the request without typing over the source draft.

Other bootstrap routes checked:

- The owned ordinary session exposed `/tmp/cc-socks/<pid>.sock`. Read-only
  inspection of the shipped message handler found peer messages and limited
  rename, idle and artifact controls, but no plugin-reload operation. Peer user
  messages explicitly set `skipSlashCommands: true`. Sending a reload string
  there would risk a model submission rather than perform the local command;
  no such message was sent. The separate `reload_plugins` control request found
  in the installed code belongs to hosted/SDK transports and is not evidence of
  an ordinary-session reload endpoint.
- Classic settings hooks hot-load, but their documented results cannot run an
  arbitrary local slash command. `SessionStart.reloadSkills` rescans skills and
  commands, while `initialUserMessage` applies to non-interactive startup; neither
  activates a late mod in an interactive session. See the
  [hook output reference](https://code.claude.com/docs/en/hooks#sessionstart-decision-control).
- JavaScript `modules` live in a plugin's `hooks/hooks.json`, not the normal
  settings hook schema. Loading that module still depends on plugin activation.
  See the [mod file reference](https://code.claude.com/docs/en/plugins/mods/reference#files).
- Claude's plugin panel applies its own pending changes when closed. Changes
  installed externally still need `/reload-plugins`; the documented
  `--plugin-dir` watch workflow requires that source to have been supplied to
  the running session. These routes do not solve arbitrary late bootstrap.
  See the [plugin command reference](https://code.claude.com/docs/en/plugins/cli-reference#reload-plugins).

The production boundary is therefore automatic discovery, project installation
and exact-source queuing, with honest pending-activation feedback. Plain-text
Terminal views require explicit `--bridge`; they are not a fallback reported as
a successful transfer. Claude conversation verification remains separate from
the parent-shell and Terminal-history proofs needed to close the source.

## Implementation status and remaining work

1. The manual styled-copy fixture and bounded archive decoder are implemented.
   A saved real Terminal capture passes exact fixture text and verified style
   mapping. Importing it into Hardcore as a read-only history archive remains
   separate work; it does not satisfy the full-state retirement proof.
2. The owned idle-zsh clone and native receiver pass exact draft, cursor,
   captured memory/history, and three-way PTY geometry checks in the
   [audited version-3 run](RESEARCH-mac-zsh-clone-state.md#version-3-live-receiver-verification).
   Advance this path with an exhaustive refusal audit for
   existing jobs, coprocesses, old-terminal descriptors, shared descriptor
   effects, and Apple session-history hooks. Verify a destination shell and
   draft before any source action.
3. Investigate same-PID rebind only in owned synthetic processes, including
   revocation failure and already-running cooperative children. There is no
   demonstrated general solution here for arbitrary existing jobs.
4. Preserve separate receipts for application state, shell state, visual
   history, and remaining source dependencies. Current Claude verification
   satisfies only the application portion; it cannot authorize full Terminal
   closure by itself.
