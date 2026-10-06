# Milestone 7 — Complete CLI

> Part of the LocalVault build plan ([index](../00-INDEX.md)). Spec source: guide §37.
> **Have these open — the complete reading set for this milestone:**
> [Part 08](../08-interfaces-cli-gui-config.md) (§28: commands, exit codes, output rules · §31: error context and partial success) ·
> [Part 09](../09-quality-testing-ci-packaging.md) (§32.7: e2e suite requirements) ·
> [Part 11](../11-appendix-skeletons.md) (§42.10: CLI bootstrap and the CLI11_PARSE caveat).
> Nothing else is required; no other milestone file is ever needed.

## Role in the overall project

M7 wraps the finished engine in its first complete user interface: every command from §28, stable exit codes, `--json` output, progress rendering, and Ctrl+C cancellation. Equally important, it delivers the black-box e2e suite (§32.7) — the tests that exercise LocalVault the way a user does, which then guard every later change.

## Why it matters

- CLI-first ordering is an architecture enforcement device: if any feature *can't* be exposed as a thin command over `localvault_core`, business logic leaked into an interface somewhere — better to discover that before the GUI repeats the mistake.
- Stable exit codes and JSON are the automation contract (scripts, cron jobs, future tooling). Once published they're frozen — this milestone is when to get them right.
- The e2e suite is the closest thing the project has to a user: it catches integration failures (argument parsing → engine → output) that unit tests structurally cannot.

## How it works

1. One options-struct + `run_*_command` function per command (§28.5); `main` only parses, installs signal handling, dispatches, and maps exceptions to exit codes (§28.3).
2. Output discipline (§28.4): results on stdout, progress/errors on stderr; `--json` emits exactly one JSON document on stdout with a schema version.
3. Cancellation: SIGINT (POSIX) / `SetConsoleCtrlHandler` (Windows) → `stop_source.request_stop()`; second interrupt = immediate exit 130.
4. `tests/cli/cli_e2e_test.py` drives the built binary through the full command matrix, checking exit codes, JSON validity, and restored bytes.

## Specification (verbatim from §37)

Implement all commands, exit codes, JSON output, progress, signal cancellation (POSIX signals and `SetConsoleCtrlHandler`), and clear errors.

Acceptance:

All core functions can be exercised without the GUI, and the CLI end-to-end suite (Section 32.7) passes on all three platforms.

## Likely problems and confusions — with answers

1. **"CLI11_PARSE returns its own exit codes and skips my mapping."** Known behavior (§42.10 note): the macro calls `app.exit()` internally. Use the explicit `app.parse()` + `catch (const CLI::ParseError&)` form so *your* table (usage error → 2) is the single authority.
2. **"My --json output breaks a parser."** Any stray `std::cout` — a progress line, a debug print, a color code — corrupts the document. Rule: in JSON mode the *only* stdout write is the final document (§28.4). The e2e suite must parse stdout strictly and fail on prefix/suffix garbage.
3. **"What's safe inside the SIGINT handler?"** Almost nothing — no locks, no allocation, no iostreams. Set a `volatile sig_atomic_t`/atomic flag (or use `signalfd`-style self-pipe); the main thread polls it and calls `request_stop()`. On Windows the `SetConsoleCtrlHandler` callback runs on its own thread — same rule: flag only.
4. **"How does the e2e test send Ctrl+C?"** POSIX: `proc.send_signal(SIGINT)`. Windows: `CTRL_BREAK_EVENT` to a process group created with `CREATE_NEW_PROCESS_GROUP` — genuinely fiddly; if it stays flaky, keep the signal e2e test POSIX-only and cover Windows cancellation interactively in the VM once, noting it in the verification log.
5. **"How do I test `--overwrite prompt` non-interactively?"** Feed scripted answers via stdin in the e2e harness. User decision (2026-10-04): accept both terminal and scripted input; EOF without an answer fails with a clear error. Prompt waits remain cancellable.
6. **"Repository-busy (exit 7) e2e test."** The Python harness grabs the lock itself: open `repository.lock` and `flock`/`msvcrt.locking` it (or run a helper subcommand that holds it), then assert the CLI returns 7 quickly, not a hang — `busy_timeout` applies to SQLite, not your lock.
7. **"Progress bar garbage in CI logs."** Emit fancy progress only when stderr is a TTY; plain line-per-phase otherwise (also honors `--quiet`/`--no-color`).
8. **Development-host amendment (2026-10-04):** develop and run the whole e2e suite on native Windows. CI still runs it on all three platforms (§32.7). Retain a human Windows session for hands-on Ctrl+C and `--json | ConvertFrom-Json` sanity checks; a VM is unnecessary on the native Windows host.

## Completion checklist

M7 is complete only when **every** box is checked. Copy this checklist into the verification log and check items there with evidence.

**Implementation**

- [ ] Every §28.2 command implemented: `init`, `snapshot`, `list`, `show`, `files`, `diff`, `restore`, `verify`, `stats`, `delete`, `gc` — with all documented flags (including `--skip-hidden`, `--one-file-system`, `--allow-risky-filesystem`, `--all` required for full restore).
- [ ] Global options work on every command: `--repo`, `--json`, `--verbose`, `--quiet`, `--no-color`, `--help`, `--version`.
- [ ] Exit codes exactly per the §28.3 table; explicit CLI11 parse handling maps usage errors to `2` (the `CLI11_PARSE` macro caveat, §42.10).
- [ ] Output rules (§28.4): results → stdout, progress/errors → stderr; `--json` emits exactly one valid JSON document with `schema_version`; progress is TTY-aware and honors `--quiet`/`--no-color`.
- [ ] Cancellation: SIGINT (POSIX) and `SetConsoleCtrlHandler` (Windows) set a flag only (async-signal-safe); cooperative stop; exit `130`; a second interrupt forces immediate exit.
- [ ] `--overwrite prompt` wired through the `ConflictResolver` with apply-to-all; behavior without a TTY decided, documented, and (if adopted) amended into §28 in the plan.
- [ ] `main` contains no storage logic: parse → dispatch → map exceptions (§28.5).

**Tests (all green)**

- [ ] e2e happy path (§32.7): `init → snapshot → list → show → files → diff → restore → verify → delete → gc` with restored bytes compared to source.
- [ ] Every exit code exercised: 0, 2 (bad flag), 3 (missing repo), 4, 5 (corrupted fixture), 6 (partial success via a permission-denied file), 7 (harness holds the lock), 130 (interrupt).
- [ ] `--json` output parses strictly for every supporting command — no stray bytes before/after the document.
- [ ] Prompt mode driven non-interactively via scripted stdin (skip / replace / apply-to-all paths).
- [ ] Cancellation e2e: interrupt mid-snapshot → 130, repository recovers on next open (POSIX in CI; Windows via CI e2e if stable, otherwise VM + logged).

**Platform & CI**

- [ ] e2e suite green on all three CI platforms (acceptance).
- [ ] Human Windows session (native host or VM, once): hands-on Ctrl+C behaves, and `localvault list --json | ConvertFrom-Json` works in PowerShell. Recorded in the verification log.

**Process**

- [ ] Implementation + verification logs under `docs/implementation-logs/M7/`, including this checklist's state.
- [ ] Log records FR-500, FR-503, and the restore/overwrite FRs (FR-303, FR-307) as fully exposed, with proving e2e cases.
