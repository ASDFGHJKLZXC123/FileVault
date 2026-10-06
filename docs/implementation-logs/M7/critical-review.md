# M7 critical review

Status: final source review complete; all reported findings and follow-ups repaired.
No open source-review blocker. Runtime and cross-platform acceptance remain separate gates.

Reviewed `AGENTS.md`, the M7 milestone/orchestrator, Part 08 sections 28/31,
Part 09 section 32.7, and the current implementation decisions. Scope includes
the new CLI sources, core whole-file verification, narrow queries, restore final-hash
behavior, their integration tests, and the CLI acceptance suite. No implementation
or tests were authored by this reviewer; no builds or tests were run. Only this log
is owned by the reviewer. Root reported the native warning-strict build passing;
that report is not independent runtime verification.

Historical named model routes are unavailable. This is a fresh inherited-model
critical review, following the M6 precedent. A separate read-only supplemental
review of the three core changes found the nullable-hash issue below and no further
blockers in the narrow queries or restore flag change.

## Findings sent to the orchestrator

1. **P1 — interrupt owner ends before fatal diagnostics.** `src/cli/main.cpp:47`
   constructs the handler inside the `try` block. On cancellation it is destroyed
   and uninstalled before `Context::fail` writes the error. A full stderr pipe then
   stalls fatal output with no second-interrupt monitor alive. The new acceptance
   case `test_second_interrupt_forces_exit_during_blocked_diagnostics` exercises
   exactly this path. Keep the handler owner outside the try block while still
   installing it after parsing. This preserves forced exit 130 through diagnostics.
   Status: repaired and source-reviewed. `main` now owns the handler outside the
   try block through result/error rendering, while installing it only after parse.

2. **P2 — cancelled non-token queries can report success.**
   `src/cli/output.cpp:57` originally serialized results without checking the stop
   token. `stats`, `list`, `files`, and `init` use core operations without a token;
   an interrupt during one can therefore end with success instead of 130. Check
   cancellation before serialization/output. Root is adding checks before and
   after serialization. Status: repaired and source-reviewed; both stop checks
   precede the single stdout write.

3. **P2 — NULL file hash hides later independent failures.**
   `src/core/integrity_verifier.cpp:346` calls `column_text` for nullable
   `file_hash`; NULL throws `database_error`, which is rethrown and aborts the
   remaining whole-file pass. An early NULL hash hides a later forged hash even
   though its chunks are intact. Guard missing hashes per file, record a file
   issue, and continue. A regression should combine an early NULL hash with a
   later wrong hash and assert all files attempted, the later mismatch, and
   unchanged repository bytes. Status: repaired and source-reviewed. The query now
   supplies a text/lowercase-hex shape predicate; NULL/malformed hashes produce
   a per-file issue through a short-circuit check, then later files continue.
   `MissingWholeFileHashDoesNotAbortLaterFileHashChecks` covers the early NULL /
   later forged hash, all three attempted files, all 39 streamed bytes, no
   `invalid_database`, and unchanged bytes. Runtime validation is root-owned.

4. **P2 — GC preview can write while opening the repository.**
   `src/cli/commands_maintenance.cpp:232` opens the default read-write repository
   before requesting preview. `validate_layout` permits missing WAL/SHM sidecars;
   RW initialization and repository-info reads can recreate them before the core
   locked read-only preview begins. The core preview's existing byte-identical
   test starts after its repository was opened, so it does not cover CLI opening.
   Route preview through a non-writing core open/view under the exclusive lock,
   or fail without writes when such a view cannot be obtained. Do not put SQLite
   handling into the CLI. Status: repaired and source-reviewed. GC dry-run now opens
   `maintenance_read_only`, retains the repository lock through database close,
   and reuses a non-writing DB view; absent sidecars use immutable access, live WAL
   uses the private locked VFS, and incomplete pairs fail without writes. New tests
   cover closed sidecar-free preview, standalone live-WAL preview, execution/deletion
   refusal, byte equality, and busy opens before DB access. Runtime evidence is root-owned.

5. **P2 — JSON progress does not follow section 28.4.**
   `src/cli/output.cpp:28` originally enabled plain-text progress when `json` was
   true. The specification requires JSON progress to be disabled or structured
   on stderr. The smallest repair is to disable progress in JSON mode, or emit
   structured events. Status: repaired and source-reviewed. JSON mode emits phase,
   entry/byte counts, and optional path as structured stderr events. Snapshot
   interrupt readiness now matches the JSON phase field.

## Passing code-review observations

- POSIX and Windows handlers only update a lock-free atomic counter. Cooperative
  stop callbacks run outside the handlers. A separate monitor can force exit while
  synchronous stop callbacks or cleanup are stalled, provided its owner remains alive.
- Prompt input checks cancellation repeatedly for POSIX terminals/pipes and Windows
  console input, pipes, and regular files. Console UTF-16 is converted to UTF-8;
  scripted CRLF and non-newline final answers are supported; empty EOF is a clear error.
- Command options remain alive through shared ownership captured by both registration
  callbacks and the dispatcher. Parsing selects one command; CLI sources use public
  core services rather than SQLite or repository storage operations.
- Results/errors use one schema-v1 stdout envelope. Prompts, diagnostics, warning
  text, and progress use stderr. UTF-8 arguments use CLI11 conversion and filesystem
  paths are explicitly encoded/decoded.
- Whole-file hashing processes one verified chunk buffer at a time and performs no
  restore writes. Ordinary missing/corrupt objects are recorded per file and later
  files continue; the NULL-hash exception above is the uncovered break in that rule.
- Explicit `--no-final-hash` skips only the final comparison. Hash metadata shape,
  paths, contiguous chunk layout, chunk size/content verification, and output size
  validation remain enforced by core restore.
- Warning queries are scoped and paged with stable ordering and existing bounds.
  Snapshot statistics require complete snapshots, count distinct referenced chunks,
  use one transaction, and retain checked unsigned aggregates and savings formulas.

## Follow-up and runtime limits

Root also identified the CLI diff's accumulation of all changes despite streaming
core output. The completed `finish_array` repair stages output with OS-owned temporary
deletion, serializes one entry at a time, checks producer/write/flush/rewind failures
before stdout, and copies with a 64KiB buffer. This closes the accumulation concern
in source review. Follow-up output repairs are also source-reviewed: the Windows
descriptor is read/write, spool read errors and failed stdout copying return 4 with
stderr-only diagnostics after stdout commitment, and result output is flushed before
a requested stop returns 130. No second JSON envelope is appended after commitment.
Runtime evidence remains root-owned.

Follow-up reviewed the retained-lock design for `maintenance_read_only`: retain the
exclusive repository OS lock in `Repository::Impl` until after its database closes;
use immutable read-only access for absent sidecars and a private locked WAL view
for live WAL; preview/verification reuse that view without reacquiring the lock.
The current VFS preserves native SHARED locks/accounting, refuses RW/CREATE for
DB/WAL opens, and denies writes/truncate/sync/delete. Database initialization
disables close checkpoints. Inspection of bundled SQLite `sqlite3WalClose` confirms
normal last-close checkpoint/deletion requires native EXCLUSIVE and private close
checkpointing is skipped with that configuration. Under the retained repository
lock, a second normal connection is no longer necessary; old comments requiring
one have been updated. The implementation and regressions have now been source-reviewed;
runtime evidence remains root-owned. Resource parameters use rvalue references so
an exception during canonical-root calculation leaves `open`'s local DB/lock owners
with the same close-before-unlock destruction order as the `Impl` fields.
Standalone regression advice: close the writer before opening the private view,
read a WAL-only row, compare files before/after view close, and probe the native DB
lock while the view is live. Avoid opening/closing the main DB via ordinary file
streams during a POSIX lock probe because that close releases process fcntl locks.

The temporary ordinary-reader concurrency regression is repaired and source-reviewed:
paired WAL readers retain ordinary read-only opens while the closed sidecar-free
fallback retains an immutable maintenance lock. The busy e2e now asserts six ordinary
query routes succeed while the writer lock is held and verification/GC preview fail
busy. Ordinary-query data equality excludes SQLite coordination sidecars; strict
maintenance byte equality remains intact.

The reader-open architecture follow-up is repaired and source-reviewed:
`Repository::open_for_query` now owns normalization and no-follow sidecar inspection
in the core. Ordinary CLI queries only pass `context.repository_path()` to that
helper. Either sidecar present selects existing read-only behavior, so incomplete
pairs still fail closed; both absent select the non-writing locked fallback. Existing
`read_only` semantics remain unchanged. Core regressions cover helper concurrency
while a writer lock is held, absent-sidecar byte equality/exclusion, and incomplete
pair rejection.

The inherited section-31.1 lock-access limitation is repaired and source-reviewed:
no-diagnostics acquisition opens the existing lock with O_RDONLY / GENERIC_READ,
retaining exclusive nonblocking locking; the writer path still requests write/create
access. New tests verify actual denied RW access, maintenance exclusion/release,
unchanged data, and refusal to create a missing lock file. This review did not execute
platform tests or certify physically read-only storage runtime behavior.

Final source closure was sent to root after reviewing all integrated repairs. Root
reported a native e2e run with 13/14 cases passing; the warning-pagination case had
a fixture ordering error (three-digit padding across 10,005 warning names). Root
changed it to five digits to match binary path order and is rerunning after the full
rebuild. This is attributed runtime information, not reviewer-executed validation or
a passing-suite claim.

The acceptance suite now covers the documented command/flag matrix, strict JSON,
scripted prompts, EOF, Unicode paths, corrupt objects, file hashes, no-final-hash,
busy/partial/usage errors, snapshot recovery, silent prompt cancellation, and a
second interrupt during blocked diagnostics. Review of test source is not a claim
that these cases pass. POSIX/macOS/Windows CI and the human Windows Ctrl+C / PowerShell
JSON session remain acceptance gates.
