# M7 independent verification

Status: **complete — all 16 M7 checklist items satisfied**. Native/human acceptance
and all five CI jobs pass. Final CI evidence and its independent audit were appended
by root; the original source-verification authorship is preserved below.

This is a fresh verification pass by an agent that did not implement the CLI or
perform the critical review. Read `AGENTS.md`, the M7 milestone, Part 08 sections
28/31, Part 09 section 32.7, Part 11 section 42.10, and the current implementation
and critical-review logs. Only this verification file is owned by this pass.
Source and test assertions were inspected independently; builds and tests are
root-owned and were not executed by this verifier. Historical named model routes
are unavailable; this inherited-model routing deviation matches the implementation
record. No future milestone work is included.

Checked implementation boxes below mean the current source implements the item.
Test boxes require completed runtime evidence. CI configuration is not evidence of
a CI pass, and an automated console signal is not a physical human Ctrl+C session.

## Source and test evidence

- **S1 — command coverage and thin adapters.** `src/cli/commands_basic.cpp:15`
  contains options and `run_*` functions for init/snapshot/list/show/files;
  registration starts at line 133. `src/cli/commands_maintenance.cpp:23` contains
  the other six option structures and functions; registration starts at line 252.
  All documented section-28.2 options are registered and forwarded to public core
  services. `restore` rejects both implicit full restore and combining paths with
  `--all` (line 137). First release's 4MiB-only chunk rule is enforced.
- **S2 — globals, parsing, exits.** `src/cli/main.cpp:10` configures every global
  option, calls explicit `app.parse(argc, app.ensure_utf8(argv))` at line 41,
  installs interrupts after parsing, and dispatches. Each subcommand uses
  `fallthrough()`. Help/version are successful paths; parse errors return 2 at
  line 64. `src/cli/output.cpp:191` maps argument/unsafe-path errors to 2,
  repository errors to 3, missing/corrupt objects to 5, partial success to 6,
  busy to 7, cancellation to 130, and other operation errors to 4. Snapshot and
  restore warning results return 6; verification failures return 5. `main`
  contains no repository/database/storage operation.
- **S3 — output discipline.** `src/cli/output.cpp:68` sends progress to stderr,
  suppresses it for quiet mode, detects stderr's terminal state, and emits plain
  phases for redirected human output or structured events for JSON. Verbose
  includes current paths. No color escape sequences are generated, so no-color
  is honored. `finish` (line 109) writes one schema-v1 result envelope with stop
  checks before/after serialization and a checked stdout flush. `fail` (line 173)
  writes actionable stderr diagnostics and one schema-v1 error envelope with
  code/message/path. Warnings/prompts/issues use stderr. `finish_array` (line 130)
  stages diff entries in an OS-deleted temporary file, checks writes/rewind, and
  copies with a 64KiB buffer; an output-copy error reports 4 on stderr without
  appending a second JSON document after stdout commitment.
- **S4 — cancellation.** `src/cli/console.cpp:28` asserts the signal counter's
  atomic type is always lock-free. POSIX SIGINT and Windows Ctrl+C/Ctrl+Break
  callbacks only increment that counter (lines 31/39). Separate ordinary threads
  request cooperative stop and call `_Exit(130)` after a second interrupt (lines
  155/168). `main` owns the handler outside the try block through diagnostics.
  `read_answer` (line 220) repeatedly checks stop for pipes/files/POSIX input;
  Windows console input uses bounded waits and UTF-16-to-UTF-8 conversion.
- **S5 — restore prompts and hash behavior.** `commands_maintenance.cpp:71`
  installs a `ConflictResolver`, supports skip/replace/skip-all/replace-all/cancel,
  and remembers apply-to-all choices. `read_answer` accepts CRLF and a final
  non-newline answer; empty EOF raises an invalid-argument error. The decision is
  recorded in Part 08 section 28.2 and the milestone. The restore adapter forwards
  `--output`, selected paths, overwrite policy, and `--no-final-hash` (line 137).
  Core restore still validates hash shape, paths, contiguous chunk layout,
  verified chunk bytes, and restored size; the flag only skips final whole-file
  hashing/comparison (`src/core/restore_engine.cpp:239`, `:567`, `:642`, `:652`).
- **S6 — core additions and maintenance safety.** Whole-file verification
  (`src/core/integrity_verifier.cpp:273`) streams one verified chunk at a time,
  records file issues, checks nullable/malformed hashes per file, and continues
  independent checks; it makes no destination files. Narrow warnings/statistics
  queries are in `QueryService`. Verify and GC preview open
  `maintenance_read_only`; `Repository::open` retains an exclusive repository
  lock through DB close (`src/core/repository.cpp:515`, `:752`). Absent sidecars
  select immutable access; a complete live pair uses a private locked read-only
  DB view; incomplete pairs fail closed. `open_for_query` (line 766) owns no-follow
  sidecar routing in core and preserves concurrent ordinary WAL readers. The
  no-diagnostics lock open requires read access only and cannot create a lock file.
- **S7 — black-box coverage and registration.**
  `tests/cli/cli_e2e_test.py:30` uses strict `json.loads` (rejecting non-JSON
  constants, prefix/suffix garbage, and multiple documents), requires schema 1,
  result/error exclusivity, error context, UTF-8 decoding, and no escape color.
  All 14 test methods were inspected. `tests/CMakeLists.txt:113` registers
  `CliEndToEnd` against the built binary with a 180-second timeout. The existing
  workflow runs CTest in Linux/macOS/Windows jobs (`.github/workflows/build-test.yml:15`,
  `:150`, `:189`); sanitizer jobs are also configured. This establishes intended
  coverage/CI routing only.

## Runtime evidence ledger

- **N1 — final native warning-strict rebuild passed**, root-reported and log
  inspected: `build/m5-junction/m7-build.log`, completed 2026-10-05 06:16:31 UTC.
  The log builds the current core/CLI/tests; the inspected cache records
  `LOCALVAULT_WARNINGS_AS_ERRORS:BOOL=ON`. Root also reports the changed C++ files
  passed the format dry-run/error check; this verifier did not run that command.
- **N2 — final native e2e passed: 14/14, zero skips, 20.705 seconds.** Inspected
  `build/m5-junction/m7-e2e-native.log` and the corresponding `CliEndToEnd` section
  at line 7200 of `build/m5-junction/cmake/Testing/Temporary/LastTest.log`.
  The CTest-launched Python 3.12 run targets the rebuilt native Debug CLI and ends
  2026-10-04 23:17 PDT (2026-10-05 06:17 UTC). All three actual process-group
  Windows Ctrl+Break cases pass, as do denied source, busy lock, strict JSON,
  restored bytes, overwrite/EOF, and corrected 10,005-warning pagination.
  This supersedes the older 13/14 run whose failure was the warning fixture's
  three-digit binary ordering; its corrected source uses five digits.
- **N3 — full native CTest passed: 265 registered, 254 passed, 11 skipped,
  zero failures; 34.78 seconds.** Inspected `build/m5-junction/m7-ctest-native.log`.
  Ten skips are symlink-dependent platform tests and one is the unsupplied M3
  external-dataset test; no M7 CLI case skipped. Nullable whole-file hash,
  no-final-hash integrity, narrow warning/statistics queries, concurrent query
  readers, closed/standalone live-WAL maintenance, busy-before-DB-access, and
  actual Windows read-only-denied lock access/exclusion/preservation regressions
  all pass. These are root-executed results verified from their saved logs.
- **C1 — final CI passed on all five jobs**, implementation revision
  `8c473b7ddc5a1b2cc4a6245f8646e16655128fc5`,
  [run 37422902785](https://github.com/ASDFGHJKLZXC123/FileVault/actions/runs/37422902785).
  Linux/macOS/Windows and ASan/UBSan each execute all 14 CLI cases with zero skips,
  including all three signal cases; TSan passes its 19-test concurrency filter.
  Detailed counts, skips, repairs, and the independent CI audit follow below.
  N1–N3 are the earlier native evidence; final portability repairs are validated by C1.
- **H1 — human native Windows session passed.** The user physically ran
  `scripts/windows-m7-acceptance.ps1` and supplied the prompt/cancellation output
  to root. Independently inspected
  `build/m7-human-aceb3abe0fdd4ab3822a463227eb4867/evidence.json`: status
  `passed by human`, timestamp `2026-10-05T17:15:31.4167440+00:00`, snapshot 1,
  current default binary `build/m5-junction/cmake/src/cli/Debug/localvault.exe`,
  physical Ctrl+C observed, exit 130, destination unchanged, PowerShell JSON
  pipeline successful, and subsequent whole-file verification successful.
  Independently read the fixture's destination file `测试.txt` (U+6D4B/U+8BD5):
  its 32 bytes still decode as `existing destination must remain` and match
  Base64 `ZXhpc3RpbmcgZGVzdGluYXRpb24gbXVzdCByZW1haW4=`. No verifier-run
  build/test/restore was needed. The earlier execution-policy block was overcome
  on the human retry. The actual filesystem name and preserved destination bytes
  are correct. Root's separate display diagnosis reproduced decoding UTF-8 as CP936;
  the wrapper now temporarily selects UTF-8 output and restores it in `finally`.
  PowerShell 5.1/7.6 syntax checks passed; no CLI/core change or human rerun occurred.
  H1 certifies the user-executed cancellation and JSON check preceding that wrapper repair.

## Completion checklist

Copied in full from `build-plan/milestones/M7-complete-cli.md`; evidence follows
each item. M7 is complete only when every box is checked.

**Implementation**

- [x] Every §28.2 command implemented: `init`, `snapshot`, `list`, `show`, `files`, `diff`, `restore`, `verify`, `stats`, `delete`, `gc` — with all documented flags (including `--skip-hidden`, `--one-file-system`, `--allow-risky-filesystem`, `--all` required for full restore).
  Evidence: S1; e2e command/flag assertions in `test_full_happy_path_bytes_queries_and_history_retention`, `test_json_help_version_globals_and_usage`, `test_snapshot_ignore_hidden_and_reuse`.
- [x] Global options work on every command: `--repo`, `--json`, `--verbose`, `--quiet`, `--no-color`, `--help`, `--version`.
  Evidence: S2/S3; every command has global fallthrough; help/version are exercised for all 11 commands in `test_json_help_version_globals_and_usage`, and snapshot/list cover placement plus quiet/verbose/no-color behavior.
- [x] Exit codes exactly per the §28.3 table; explicit CLI11 parse handling maps usage errors to `2` (the `CLI11_PARSE` macro caveat, §42.10).
  Evidence: S2; no `CLI11_PARSE` usage. Runtime matrix is separately tracked below.
- [x] Output rules (§28.4): results → stdout, progress/errors → stderr; `--json` emits exactly one valid JSON document with `schema_version`; progress is TTY-aware and honors `--quiet`/`--no-color`.
  Evidence: S3/S7; shared strict document assertion covers results and fatal errors. JSON progress is structured stderr; quiet snapshot stderr is asserted empty.
- [x] Cancellation: SIGINT (POSIX) and `SetConsoleCtrlHandler` (Windows) set a flag only (async-signal-safe); cooperative stop; exit `130`; a second interrupt forces immediate exit.
  Evidence: S4; three signal cases in S7 cover interrupted snapshot recovery, silent prompt cancellation, and blocked-diagnostic forced exit. Platform runtime gates remain below.
- [x] `--overwrite prompt` wired through the `ConflictResolver` with apply-to-all; behavior without a TTY decided, documented, and (if adopted) amended into §28 in the plan.
  Evidence: S5; `test_scripted_overwrite_decisions_and_eof` asserts skip/replace/apply-to-all, prompt counts, preserved bytes, CRLF/final-answer handling, cancel, and EOF exit 2.
- [x] `main` contains no storage logic: parse → dispatch → map exceptions (§28.5).
  Evidence: S1/S2; inspected all of `src/cli/main.cpp` and both command adapters. Core owns repository views, queries, snapshot/diff/restore/verify/delete/GC logic.

**Tests (all green)**

- [x] e2e happy path (§32.7): `init → snapshot → list → show → files → diff → restore → verify → delete → gc` with restored bytes compared to source.
  Evidence: N2. `test_full_happy_path_bytes_queries_and_history_retention` covers the sequence, 9MiB multi-chunk content, empty/Unicode files, alternate restore bytes, history retention after GC, and statistics.
- [x] Every exit code exercised: 0, 2 (bad flag), 3 (missing repo), 4, 5 (corrupted fixture), 6 (partial success via a permission-denied file), 7 (harness holds the lock), 130 (interrupt).
  Evidence: N2. Proving cases: happy path (0); globals/usage (2); repository/filesystem errors (3/4); corrupt object and forged file hash (5); denied source file and overwrite skips (6); harness-held lock (7); all three interrupt cases (130). Windows denied-file fixture holds an exclusive file handle to cause native access/sharing denial; POSIX uses chmod and skips if the account bypasses it.
- [x] `--json` output parses strictly for every supporting command — no stray bytes before/after the document.
  Evidence: N2. S7's shared document assertion is used for every command, help/version, and failures. Forced immediate exit intentionally emits no stdout document, as asserted by its test.
- [x] Prompt mode driven non-interactively via scripted stdin (skip / replace / apply-to-all paths).
  Evidence: N2. `test_scripted_overwrite_decisions_and_eof` and `test_delete_scripted_confirmation` assert decisions and destination/history effects.
- [x] Cancellation e2e: interrupt mid-snapshot → 130, repository recovers on next open (POSIX in CI; Windows via CI e2e if stable, otherwise VM + logged).
  Evidence: C1 and N2. `test_snapshot_interrupt_recovers_prior_snapshot` sends SIGINT/Windows Ctrl+Break, asserts 130, creates another snapshot, restores prior bytes, and verifies files. It and both prompt/forced-exit signal cases pass without skips on Linux, macOS, Windows, and ASan/UBSan. H1 separately closes the physical human gate.

**Platform & CI**

- [x] e2e suite green on all three CI platforms (acceptance).
  Evidence: C1; all 14 cases pass without skips on Linux/macOS/Windows. ASan/UBSan also passes the full suite; TSan passes its selected concurrency suite.
- [x] Human Windows session (native host or VM, once): hands-on Ctrl+C behaves, and `localvault list --json | ConvertFrom-Json` works in PowerShell. Recorded in the verification log.
  Evidence: H1, user-executed physical session, preserved evidence JSON, and independent confirmation of original destination bytes. This closes the human gate; automated Ctrl+Break alone was not used to close it.

**Process**

- [x] Implementation + verification logs under `docs/implementation-logs/M7/`, including this checklist's state.
  Evidence: `implementation.md`, `critical-review.md`, and this `verification.md`; critical review reports all source findings repaired, with runtime gates explicitly separate.
- [x] Log records FR-500, FR-503, and the restore/overwrite FRs (FR-303, FR-307) as fully exposed, with proving e2e cases.
  Evidence: mapping below records full CLI exposure, test assertions, and completed acceptance. FR-503's separate GUI scope is not certified by M7.

## Requirement-to-proof mapping

| Requirement | Current CLI exposure | Proving e2e assertions | Acceptance state |
|---|---|---|---|
| FR-500 — all required operations through `localvault` | Fully exposed by S1/S2: all 11 commands and documented flags use core services. | `test_full_happy_path_bytes_queries_and_history_retention`; `test_json_help_version_globals_and_usage`; `test_snapshot_ignore_hidden_and_reuse`; warning paging; hash/no-final-hash; scripted delete/restore. | Native and three-platform CLI acceptance complete (N1–N3, C1). |
| FR-503 — structured, actionable errors in both interfaces | CLI scope fully exposed by S2/S3: stderr context and JSON `error.code/message/path`, partial-warning results and exit 6. M7 does not certify the other interface. | Shared `document`; globals/usage; repository/filesystem errors; corrupt object; denied source; busy (asserts lock path); EOF; cancellation. | CLI acceptance complete (N2, C1); overall both-interface requirement is not closed here. |
| FR-303 — restore to an alternate destination | Fully exposed by S5 through `--output`, selected relative paths, and explicit full restore `--all`. | Happy path compares all original bytes in two alternate destinations, including a deleted historical file and multi-chunk/empty/Unicode files; unsafe path and missing/conflicting `--all` return 2. | Native and three-platform CLI acceptance complete (N2, C1). |
| FR-307 — `never`, `prompt`, `always` overwrite policies | Fully exposed by S5; prompt uses core resolver with per-entry/apply-to-all/cancel decisions. | `test_scripted_overwrite_decisions_and_eof` checks every policy, skip/replace mixtures, one prompt for apply-to-all, preserved/replaced bytes, EOF 2, and cancel 130; silent prompt/second interrupt cases protect original bytes. | Native, physical Windows, and three-platform CLI acceptance complete (N2, H1, C1). |

Additional core regressions inspected include
`IntegrityVerifierTest.MissingWholeFileHashDoesNotAbortLaterFileHashChecks`,
`WholeFileHashMismatchIsDetectedWithIntactChunks`,
`CancellationDuringWholeFileVerificationReleasesLockWithoutWrites`,
`QueryServiceTest.WarningsAreSnapshotScopedAndPagedWithDeterministicOrdering`,
`SingleSnapshotStatisticsUseFilesAndReferencedChunksOnly`,
`ReadOnlyQueriesWorkWhileWriterLockIsHeldWithoutReadingObjects`,
`RestoreEngineTest.ExplicitNoFinalHashRetainsMetadataAndChunkValidation`, and the
repository/GC standalone-live-WAL, closed-sidecar-free, busy-before-DB-access,
read-access-only lock, and no-create regressions. Their passing executions are
recorded in N3, independently checked against the saved CTest results.

## Original independent conclusion — before CI

No additional source blocker was found in this verification pass. The corrected
warning fixture and all other native e2e cases pass (N2), and final native
warning-strict build/CTest evidence is recorded (N1/N3). The physical Windows
session and PowerShell JSON pipeline passed (H1), with original destination bytes
independently confirmed. At this stage M7 remained in progress until current-revision
three-platform CI (including POSIX cancellation) was recorded. C1 now closes that
gate. The human session's Unicode display repair is recorded under H1.
Do not infer CI outcomes from code review, native-only runtime results, or workflow
configuration.

## Historical CI repairs — root, 2026-10-05–06 PDT

- Committed and pushed M7 as `5ec34e1c9a4cc088318b01f08149fa016f2a1d53`
  to `Latest-Dev`; [initial run 37412891121](https://github.com/ASDFGHJKLZXC123/FileVault/actions/runs/37412891121).
- Source review identified the CLI fixture's lexical macOS `/var` alias: restore
  intentionally rejects symlink ancestors before canonicalization. Fixture-only
  correction `Path(self.temporary.name).resolve()` preserves those production
  checks. Python AST parsing and `git diff --check` passed. Enabled verbose CTest
  logs to expose nested CLI test skips. Pushed as
  `0097f3e44737d8b02035e5cf423f8ef59bec18c1`;
  [corrected run 37413141510](https://github.com/ASDFGHJKLZXC123/FileVault/actions/runs/37413141510).
- Corrected macOS job [112105824606](https://github.com/ASDFGHJKLZXC123/FileVault/actions/runs/37413141510/job/112105824606)
  failed in vcpkg configuration: three attempts to download GoogleTest 1.17.0
  timed out. Cache restoration found zero packages. No project build or tests
  executed in this job; it supplies no acceptance evidence.
- The GitHub connector refused a job retry with Actions-write permission error
  403. Automatic approval review rejected using Git's saved credential to retry
  and cancel jobs, identifying credential extraction/access-control bypass.
  Neither mutation executed. Requested a user-triggered macOS retry and continued
  checking unaffected jobs. Platform/sanitizer results were pending at that stage;
  later reviewed source repairs and normal authorized pushes started fresh CI runs.
- The TSan preset covers the existing concurrency filter and excludes
  `CliEndToEnd`; platform and ASan/UBSan jobs are configured to run the full CLI
  suite. Passing evidence was pending at that stage and is now recorded in C1.
  Windows CLI signal skips, if present, must be reported separately from N2/H1.
- All three corrected Linux jobs also failed during configuration, before any
  project build/test: repeated gperf 3.3 download timeouts at both GNU origins.
  Saved logs are under `build/m7-ci/37413141510-{linux,linux-sanitizers,linux-tsan}.log`.
- Download repair: `scripts/ci-prefetch.cmake` preloads the exact pinned GoogleTest
  archive from GitHub codeload, and the pinned Linux gperf archive from the kernel.org
  GNU mirror. SHA512 values and filenames match the pinned vcpkg portfiles. Verified
  cache files are reused; downloads are bounded and verified before publication;
  an alternate network failure leaves vcpkg's original fallback available, while
  a checksum mismatch fails immediately. The pinned dependency graph is unchanged.
  Native checks passed for both cold downloads, simulated Linux host routing,
  verified cache reuse, and deliberate wrong-hash rejection without publication.
  A fresh review confirmed these safeguards and the five workflow call sites.
- Download-fixed revision `1f24c35f3e92e33b6b6c34e74f161ade1ac04040`,
  [run 37415937849](https://github.com/ASDFGHJKLZXC123/FileVault/actions/runs/37415937849),
  reached macOS build/tests successfully. Its only failure was the forced-interrupt
  CLI fixture: a failed atomic 4096-byte pipe write could leave room for the short
  cancellation diagnostic and graceful JSON output. All other 13 CLI cases passed
  with no skips, including snapshot interrupt/recovery and silent prompt cancellation.
  CTest recorded 269 passed, one failed, one external-dataset skip, and one disabled
  manual benchmark (272 registered), 52.54 seconds. The fixture now exhausts the
  remaining pipe capacity with single-byte writes; forced-exit assertions remain.
  Native pipe saturation probes confirm that no single byte fits after bulk/tail
  filling, including when the pipe starts with a seeded partial write.
- Further source review identified a second mechanism behind that macOS result:
  SIGINT was installed without `SA_RESTART`, so the second signal could interrupt
  the blocked stderr write with EINTR. A failed stream can then allow graceful JSON
  and monitor destruction before its next poll. This is inferred from the source
  and observed alive-before-second-signal / JSON-after-second-signal sequence;
  the log does not directly capture errno. Added `SA_RESTART` to keep diagnostic
  writes blocked until the monitor forces exit. Signal handlers remain flag-only;
  cooperative input cancellation retains its bounded 10 ms polling. The pipe
  saturation repair is retained. C1 now validates all signal cases successfully.
- The same run's Linux, ASan/UBSan, and TSan jobs failed compilation at `src/cli/output.cpp:131`:
  GCC emitted `-Werror=ignored-attributes` for `decltype(&std::fclose)` as a
  `unique_ptr` deleter. Replaced it with a lambda deleter retaining the same
  close-on-destruction behavior. No tests ran on those failed builds.
- Secondary Windows evidence from `0097f3e` / run 37413141510:
  [job 112105824744](https://github.com/ASDFGHJKLZXC123/FileVault/actions/runs/37413141510/job/112105824744)
  passed 264 of 265 registered CTest entries, with only the opt-in external dataset
  skipped; 47.83 seconds. All 14 CLI cases passed with zero skips (10.131 seconds).
  This secondary result is not substituted for C1's fully corrected revision.

## Corrected implementation CI — root evidence

Revision `0b2dfaa5f198f3123703d1d16d2508ff490bb579`,
[run 37420211705](https://github.com/ASDFGHJKLZXC123/FileVault/actions/runs/37420211705).
This historical revision did not close acceptance; its POSIX forced-exit cases failed.

| Job | Executed result |
|---|---|
| [Windows](https://github.com/ASDFGHJKLZXC123/FileVault/actions/runs/37420211705/job/112127655188) | Passed: 264 tests, one opt-in external-dataset skip, 265 registered; 49.17 s. All 14 CLI cases passed, zero skips; 10.328 s. |

Windows log: `build/m7-ci/37420211705-windows.log`. The updated forced-interrupt
case, permission-denied source, warning pagination, and every CLI command ran.
GoogleTest prefetch was SHA512-verified; the vcpkg binary cache restored successfully.

The same revision's macOS job later failed the forced-exit case again despite the
pipe-tail repair: graceful JSON was emitted after the second signal. All 14 CLI
cases ran (13 passed, one failed, zero skips; 18.692 s); CTest recorded 269 passed,
one failed, one external-dataset skip, and one disabled manual benchmark, 52.33 s.
This establishes that pipe saturation alone was insufficient. The subsequent
`8c473b7ddc5a1b2cc4a6245f8646e16655128fc5` revision includes the `SA_RESTART`
source repair; [run 37422902785](https://github.com/ASDFGHJKLZXC123/FileVault/actions/runs/37422902785)
is now the final validation target.

The `0b2dfaa` Linux build also passed, confirming the lambda-deleter repair.
Its sole test failure was the same forced-interrupt case (graceful JSON after
the second SIGINT): 269 passed, one failed, one external-dataset skip, 271
registered; 9.02 s. All 14 CLI cases executed, 13 passed and one failed, zero
skips; 3.034 s. The latest `SA_RESTART` repair applies to both POSIX platforms.

The `0b2dfaa` ASan/UBSan job also built and executed the full suite: 269 passed,
one failed (the same forced-interrupt assertion), one external-dataset skip, 271
registered; 38.98 s. CLI cases: 13 passed, one failed, zero skips; 8.420 s. The
inspected log contained no ASan, LeakSanitizer, or UBSan runtime error report;
  this older job remains a failed validation; C1 subsequently passed the corrected revision.

The `0b2dfaa` TSan job succeeded: all 19 selected concurrency tests passed,
zero skips or failures, 3.00 s. The log contained no ThreadSanitizer warning,
fatal error, or race summary. This filtered suite excludes CLI signal cases;
final acceptance still targets `8c473b7` across all five jobs.

## Final signal-corrected CI — root evidence

Revision `8c473b7ddc5a1b2cc4a6245f8646e16655128fc5`,
[run 37422902785](https://github.com/ASDFGHJKLZXC123/FileVault/actions/runs/37422902785).
All five jobs passed. All configure/build/test steps succeeded with project
warnings treated as errors. This is the final implementation acceptance run.

| Job | Executed result |
|---|---|
| [Linux](https://github.com/ASDFGHJKLZXC123/FileVault/actions/runs/37422902785/job/112136013079) | Passed: 270 tests, one external-dataset skip, 271 registered; 9.96 s. CLI 14/14 passed, zero skips; 3.286 s. |
| [Windows](https://github.com/ASDFGHJKLZXC123/FileVault/actions/runs/37422902785/job/112136013059) | Passed: 264 tests, one external-dataset skip, 265 registered; 41.24 s. CLI 14/14 passed, zero skips; 9.927 s. |
| [macOS](https://github.com/ASDFGHJKLZXC123/FileVault/actions/runs/37422902785/job/112136013102) | Passed: 270 tests, one external-dataset skip, one disabled M5 manual benchmark, 272 registered; 44.52 s. CLI 14/14 passed, zero skips; 17.883 s. |
| [Linux ASan/UBSan](https://github.com/ASDFGHJKLZXC123/FileVault/actions/runs/37422902785/job/112136013065) | Passed: 270 tests, one external-dataset skip, 271 registered; 40.45 s. CLI 14/14 passed, zero skips; 9.895 s. No ASan, LeakSanitizer, or UBSan runtime error reports. |
| [Linux TSan](https://github.com/ASDFGHJKLZXC123/FileVault/actions/runs/37422902785/job/112136012923) | Passed: all 19 selected concurrency tests, zero skips/failures; 3.02 s. No ThreadSanitizer warning, fatal error, or race summary. |

The macOS forced-exit assertion passes with the source-level `SA_RESTART` fix;
the prior fully saturated fixture still failed without that flag. All 14 CLI
cases, including all three signal cases, executed successfully with zero skips
on Linux, macOS, Windows, and ASan/UBSan. The TSan preset runs its existing
19-test concurrency filter, not the CLI suite. The only full-suite skip is the
unset opt-in M3 dataset; macOS additionally disables the M5 manual benchmark.
Completed logs are saved under `build/m7-ci/37422902785-*.log`.

## Independent final CI audit — 2026-10-06 PDT

A separate read-only verifier fetched run/jobs once and independently confirmed
the successful run identity, branch, full SHA, attempt 1, and every configure/build/test
conclusion. It recounted the five saved logs, including each nested CLI method,
zero CLI skips, all three signal cases, expected dataset/manual omissions, and the
absence of ASan/LeakSanitizer/UBSan/TSan runtime diagnostics. It also inspected
the `SA_RESTART` installation, independent stop/force-exit monitors, and unchanged
exit-130/empty-stdout/destination-byte assertions. Both remaining M7 boxes and
both sanitizer gates can close. The verifier authored no code, ran no builds/tests,
and performed no Git or CI mutations. No M7 acceptance gate remains open.

The final documentation-only closure is committed and pushed separately; its
automatic CI is checked before reporting branch completion. No implementation
changes follow the successful run above.
