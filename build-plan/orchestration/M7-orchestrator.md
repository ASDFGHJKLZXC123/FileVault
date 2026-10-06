# M7 Orchestrator — Complete CLI

Spec: [../milestones/M7-complete-cli.md](../milestones/M7-complete-cli.md) · Loop: [00-COMMON.md](00-COMMON.md)
Reading set: Part 08 (§28, §31) · 09 (§32.7) · 11 (§42.10)

## Fix first (orchestrator)

- Exit codes and JSON schema freeze at release — orchestrator signs off both before command packets start.
- If any feature can't be a thin command over `localvault_core`, logic leaked — fix the core seam, don't work around it.
- User decision (2026-10-04): restore prompts accept terminal and scripted stdin; EOF fails clearly.

## Packets

| # | Packet | Scope | Order | Class |
|---|---|---|---|---|
| A | Skeleton | `main` = parse→dispatch→exception-to-exit-code map (§28.3/28.5); explicit `app.parse()` (§42.10 caveat); global options; output discipline §28.4 | first | standard |
| B | Commands 1 | `init`, `snapshot`, `list`, `show`, `files` + flags | after A, parallel | standard |
| C | Commands 2 | `diff`, `restore` (prompt via `ConflictResolver`), `verify`, `stats`, `delete`, `gc` | after A, parallel | standard |
| D | Cancellation | SIGINT / `SetConsoleCtrlHandler`: flag-only handler, cooperative stop, exit 130, second interrupt = immediate | after A | critical |
| E | e2e suite | `tests/cli/cli_e2e_test.py` (§32.7): full matrix, every exit code, strict JSON parse, scripted stdin prompts | parallel with B/C once A lands | standard |

## Watchpoints (put in briefs)

- Never `CLI11_PARSE` — the macro bypasses the exit-code table.
- JSON mode: the only stdout write is the single final document; e2e fails on any stray byte.
- Signal handler: set an atomic flag, nothing else — no locks, allocation, iostreams.
- Exit-7 e2e: harness holds `repository.lock` itself; assert fast failure, not a hang.
- Progress fancy only when stderr is a TTY; honors `--quiet`/`--no-color`.
- Windows Ctrl+C e2e is fiddly (`CTRL_BREAK_EVENT` + process group); if flaky, keep POSIX-only and log the VM check instead.

## Verify

- Fresh agent: checklist → `docs/implementation-logs/M7/`; FR-500/503, FR-303/307 mapped to e2e cases.
- e2e green on all three CI platforms (acceptance).
- Richard: native Windows session (once) — hands-on Ctrl+C; `localvault list --json | ConvertFrom-Json`.
