# Hardcore Claude handoff adapter

The adapter requires a Hardcore build that includes Claude handoff and Claude
Code 2.1.300 or later. Earlier Claude versions cannot supply the rich conversation
snapshot. From the existing conversation's project directory:

```sh
hardcore claude install
hardcore claude status
```

Installation enables **both** `hardcore-slurp` and `hardcore-slurp-driver` for
that project. In the existing Claude session, run `/reload-plugins`, then
`/hardcore-slurp wait`. A newly installed plugin does not load into an existing
session until it is reloaded. Keep the source shell and Terminal window open;
the adapter moves the conversation, not the parent shell or Terminal history.

`/hardcore-slurp` prepares a Hardcore receiver and moves a verified settled
conversation using Claude's background-session mechanism. `/hardcore-slurp wait`
waits for an ordinary turn to finish; it does not submit a model prompt or
interrupt work. `/hardcore-slurp status` explains what is still needed, and
`/hardcore-slurp cancel` cancels a pending request.

After a late load, the adapter deliberately waits to observe a normal main-turn
completion and a Stop event with explicitly empty background-task and scheduled
task lists. A quiet prompt alone does not prove there is no work. Running,
pending or unrecognized agent states, permission requests and changes during
preparation stop the handoff. Text drafts and their UTF-16 cursor positions are
saved privately, restored in the receiving composer, and read back exactly.
For a cursor in the middle, a request-specific `prompt.edit` hook restores its
position after one cursor-only Left key from the verified receiving PTY. No
source draft is submitted or cleared. Source editing, history recall, undo and
stash changes during preparation cancel the saved snapshot, including an edit
changed back to its original text.
Visible `[Image #…]` and `[Pasted text #…]` labels refuse handoff: Claude's public
composer API exposes the label but not its hidden image or collapsed-text
payload. A literal label also refuses because the API cannot distinguish it.

Once the adapter is loaded, Hardcore can queue a request through the native
helper. The mod polls for a request bound to its exact session and working
directory; no slash command is typed over the draft. A newly installed adapter
still requires `/reload-plugins` in an already running Claude session.

```sh
claude agents --json --all --cwd "$PWD" # find the interactive source sessionId
hardcore claude move SESSION_ID         # optional: --project /path/to/project
hardcore claude moves
hardcore claude inspect REQUEST_ID
hardcore claude recover REQUEST_ID --export-draft saved-draft.json
```

`move` waits for the same readiness checks as the slash command; it does not
interrupt active work. `inspect` checks the saved request and current source,
destination, and receiver. `recover` writes the saved draft and cursor to a new
private JSON file without submitting text or starting another conversation.
If the receiving client fails after backgrounding, use `claude agents` to find
the background session and `claude attach ID` to recover it.

Two plugins are necessary because Claude skips a timer's own plugin when it
dispatches a command. The separate driver makes the final background command
pass through the guard again, using a one-use request identifier. The guard
checks state before and after the receiver arms and catches errors without
running the background command. The driver declares the guard as a plugin
dependency, so Claude refuses to disable the guard alone and does not load the
driver without it. Keep both enabled during a handoff; this depends on Claude's
plugin loader and does not protect against manual file removal or a runtime
failure. See [Claude's dependency behavior](https://code.claude.com/docs/en/plugins/install#plugins-with-dependencies).

The `executable` plugin setting accepts the installed `hardcore` command or an
absolute executable path. The helper receives session identifiers and the
working directory, the draft text, and a hash of Claude's current conversation in
Messages API form. This includes image, document, thinking and tool blocks. The
private handoff directory and records are restricted to the current
user. The handoff temporarily
renames the session to a unique marker; the destination restores its title.
Only after that local rename command completes does the destination acknowledge
readiness. The receiver waits for this acknowledgement before attaching, because
Claude's public session list can show a new process before it accepts attachment.
After restoring the draft, the mod checks the source conversation snapshot,
emits a unique terminal receipt, and asks the native receiver to verify actual
attachment and rendering. It refuses to overwrite a different receiving draft.
The receiving tab holds human text input while restoration runs. The mod checks
that the exact request's input guard remains active before filling, retries
attachment barriers for a bounded time, and releases the guard after success or
failure. Cursor movement is one-use and requires its own rendered request marker.

The public prompt API does not expose unsent image attachments or a hidden prompt
stash. Detected attachment/paste labels are blocked, but this cannot certify the
absence of every hidden composer state. The rich message API returns the current normalized context, capped at
4096 messages, with the last compaction summary replacing older messages; it
excludes the system prompt and tool definitions. These checks cannot certify
every saved transcript byte, image draft, shell variable, shell history entry,
or terminal scrollback. They do not authorize closing Terminal.

This is cooperative conversation handoff, not migration of Claude's own process
or arbitrary active tools. User-installed classic hooks may be suppressed in
managed Claude installations; the adapter then cannot establish settled state
and refuses to move. The hooks API is early access and needs validation when
Claude changes.

Run local deterministic checks with:

```sh
node --test plugins/hardcore-slurp/tests/*.test.mjs
claude plugin validate plugins/hardcore-slurp
claude plugin validate plugins/hardcore-slurp-driver
```

The opt-in `python3 tests/probe_claude_bootstrap.py --cursor` probe checks a real
Claude composer on a disposable PTY without sending a model prompt. Without
`--cursor`, it checks Claude's own Ctrl+S stash around late plugin reload, with
known composer focus and no existing stash. This is research evidence, not blind
Terminal keystroke automation.
