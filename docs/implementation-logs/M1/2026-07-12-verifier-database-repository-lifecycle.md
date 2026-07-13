# Milestone 1 Independent Verification

Date: 2026-07-12
Verifier role: fresh independent verification; no implementation fixes made
Workspace: `/Users/f8fq/dev/LocalVault`
Verified tree state: M1 implementation is uncommitted on `main`; `HEAD` is
`a9ac009863ae8e9c16f99f25ccbfdeb146dde305` (the M0 completion commit).

## Local command evidence

- `cmake --preset development -DLOCALVAULT_WARNINGS_AS_ERRORS=ON && cmake --build
  --preset development --parallel 8 && ctest --preset development` — build succeeded and 37/37
  tests passed. Configuration emitted a non-fatal CMake author warning that `SQLite::SQLite3` is
  deprecated; no project compiler warning failed the warning-as-error build.
- `cmake --build --preset sanitizers --parallel 8 &&
  ASAN_OPTIONS=detect_leaks=0:halt_on_error=1
  UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
  ./build/sanitizers/tests/localvault_tests` — sanitizer target was current and 37/37 tests passed
  directly under ASan/UBSan with leak detection disabled for this macOS host.
- `find src include tests benchmarks -type f \( -name '*.cpp' -o -name '*.hpp' -o -name
  '*.h' -o -name '*.cc' -o -name '*.cxx' \) -print0 | xargs -0 clang-format --dry-run
  --Werror` with clang-format 22.1.8 — passed.
- `git diff --check` — passed.
- Win32 sources and CMake selection were inspected statically only. No Windows compiler or VM was
  used in this verification.
- C4 re-verification: the orchestrator reran the current warning-strict development build/CTest and
  direct ASan/UBSan binary, reporting 37/37 passed in both. Independently, the verifier ran the
  nine repaired/adjacent repository and database tests via development CTest and then the same
  nine with a direct sanitizer-binary GoogleTest filter; both runs passed 9/9. A fresh full-tree
  clang-format 22.1.8 dry run also passed.

## Completion checklist

Every row is explicitly classified as **PASS**, **FAIL**, or **PENDING**. The local M1 amendment is
authoritative: FR-005 recovery is deferred to M4 and is not expected in M1.

### Implementation

1. **PASS — Database/Statement/Transaction wrappers.** `Database::execute` prepares a single
   `Statement` (`src/core/database/database.cpp:186-193`); `Statement` uses
   `sqlite3_prepare_v3`, rejects trailing statements, and checks prepare/bind/step/reset/finalize
   outcomes (`src/core/database/statement.cpp:41-67,90-143,188-198`). `Transaction` begins via the
   wrapper, commits only in explicit `commit()`, and its `noexcept` destructor attempts only
   rollback while swallowing/reporting all exceptions (`src/core/database/transaction.cpp:10-30`).

2. **PASS — Connection modes and immutable selection.** The connection path enables and verifies
   foreign keys, FULL synchronous mode, and a 5000 ms busy timeout; writable connections establish
   and verify WAL, ordinary read-only connections verify WAL, and immutable connections
   intentionally skip WAL verification (`src/core/database/database.cpp:195-219`). Read-only open
   uses SQLite read-only flags and does not create paths (`src/core/database/database.cpp:135-142`),
   as also proved by `Database.ReadOnlyConnectionCreatesNoDirectoryEntries`. POSIX immutable
   lifecycle selection now requires the filesystem-reported `ST_RDONLY` flag and no longer treats
   directory permission bits as proof (`src/core/filesystem/platform/posix_repository_support.cpp:166-170`);
   Win32 requires `FILE_READ_ONLY_VOLUME` (`src/core/filesystem/platform/win32_repository_support.cpp:75-85`).
   `database_access_for_open` selects immutable mode only after that platform proof when both WAL
   sidecars are absent (`src/core/repository.cpp:151-173`). The repaired and adjacent targeted tests
   passed 9/9 in both development and sanitizer runs.

3. **PASS — Migration runner.** `run_migrations` owns an exclusive transaction, reads the installed
   maximum, walks the ordered migration array, records one row after applying each missing
   migration, and commits only after all work (`src/core/database/migrations.cpp:100-114,122-161`).
   `Migrations.FailureRollsBackEveryChange` injects failure at the migration record and proves zero
   rows and no partially-created schema objects remain.

4. **PASS — Migration 001 complete schema.** The embedded migration creates `repository_info`,
   `snapshots` including `deleting`, `entries` including `change_time_ns` and
   `windows_attributes`, `chunks`, `entry_chunks`, `snapshot_warnings`, `repository_settings`, and
   all five specified named indexes (`src/core/database/migrations.cpp:16-98`).
   `Migrations.InitialSchemaContainsAllRequiredObjectsAndColumns` proves the tables, columns,
   indexes, accepted `deleting` status, and rejected unknown status.

5. **PASS — Optional operation history decision.** The implementation log explicitly records the
   decision not to include `operation_history`; migration 001 omits it and
   `Migrations.InitialSchemaContainsAllRequiredObjectsAndColumns` asserts it is absent
   (`docs/implementation-logs/M1/2026-07-12-codex-database-repository-lifecycle.md:7-10`,
   `tests/unit/migrations_test.cpp:100-116`).

6. **PASS — `Repository::create` §15 lifecycle order and cleanup.** Normalization, non-empty and
   reserved-path checks, filesystem policy, POSIX permission application, required path creation,
   lock acquisition, database setup/migration/info insertion, checked checkpoint/cache flush and
   containing-directory flush are present (`src/core/repository.cpp:375-545`). Cleanup is
   non-recursive and ledger-limited, with tests for
   preserving unmarked/pre-existing content (`src/core/filesystem/creation_ledger.hpp:35-45`,
   `CreationLedgerTest.CleanupRemovesOnlyMarkedPaths`,
   `RepositoryTest.ExistingSqliteSidecarCollisionIsPreserved`). The repaired success path first
   validates the complete repository while the lock is held, marks it published, explicitly resets
   the writer lock, and then performs the required final normal post-release open
   (`src/core/repository.cpp:478-490,506-555`). All catch paths clean up only while unpublished;
   a post-publication final-self-check failure therefore reports the error without deleting a
   repository that was already proven valid (`src/core/repository.cpp:556-575`). The roundtrip and
   adjacent lifecycle tests passed in development and sanitizer re-verification.

7. **PASS — `Repository::open` semantics.** Open validates the non-symlink layout, migration/table/
   index set, repository-info row/storage types, format, algorithm, encoding, and ranges
   (`src/core/repository.cpp:89-139,207-327,564-587`). Read-only mode neither acquires a
   `RepositoryLock` nor invokes recovery; `Repository::Impl` holds only root, info, database, and a
   future injector, so read-write open has no lifetime writer lock (`src/core/repository.cpp:364-373`).
   Recovery is intentionally absent under the M1 amendment and deferred to M4.

8. **PASS — RepositoryLock platforms and diagnostics.** CMake selects Win32 versus POSIX lock and
   repository-support sources (`src/core/CMakeLists.txt:1-25`). The shared interface is
   `src/core/filesystem/platform/platform_lock.hpp`; POSIX uses nonblocking `flock` and Win32 uses
   nonblocking `LockFileEx` while keeping the descriptor/handle for RAII lifetime
   (`posix_lock.cpp:60-97`, `win32_lock.cpp:64-107`). PID/acquisition time are file diagnostics,
   and the contention test proves the lock file remains after release. Win32 compilation remains
   covered only by the pending CI gate.

9. **PASS — Repository information query.** `RepositoryInfo` exposes UUID, format version, chunk
   size, zstd level, and hash algorithm (`include/localvault/repository.hpp:19-25`), populated by
   checked database reads (`src/core/repository.cpp:235-317`). The roundtrip test verifies all five.

### Tests

10. **PASS — Create/open roundtrip.** `RepositoryTest.CreateAndOpenRoundTripPreservesRepositoryInformation`
    passed in development and sanitizer runs.

11. **PASS — Random directory rejection.** `RepositoryTest.RandomDirectoryIsNotARepository` passed
    and asserts `ErrorCode::invalid_repository` (`tests/integration/repository_test.cpp:90-100`).

12. **PASS — Future format rejection.** `RepositoryTest.FutureFormatVersionIsRejectedClearly`
    passed and asserts `unsupported_repository_version` plus `format version 2` in the message
    (`tests/integration/repository_test.cpp:102-118`).

13. **PASS — Exception rollback.** `Transaction.ExceptionRollsBackAllRows` passed and verifies the
    post-exception row count is zero (`tests/unit/database_test.cpp:170-191`).

14. **PASS — Foreign-key violation.** `Database.EnforcesForeignKeys` passed and proves an orphan
    insert raises `database_error` (`tests/unit/database_test.cpp:207-221`).

15. **PASS — Migration idempotence.** `Migrations.AppliesMigrationOnceAndReopeningIsIdempotent`
    passed; count stays one and `applied_at_ns` is unchanged after the second run
    (`tests/unit/migrations_test.cpp:81-98`).

16. **PASS — Second process contention.** `RepositoryLockIntegrationTest.SecondProcessReceivesRepositoryBusy`
    passed. It launches a distinct helper process and interprets helper exit zero only after the
    helper catches `repository_busy` (`tests/integration/lock_contention_test.cpp:92-130,156-180`,
    `tests/support/lock_contention_helper.cpp:21-46`).

17. **PASS — Existing non-empty create rejection.** `RepositoryTest.ExistingNonEmptyDirectoryRequiresApproval`
    passed and confirms the user file remains while no database appears
    (`tests/integration/repository_test.cpp:168-182`).

18. **PASS — Filesystem classifier policy.** The passing tests
    `NetworkFilesystemsRequireExplicitApproval`, `FatFilesystemsAreAllowedWithAWarning`, and
    `UnknownFilesystemsAreAllowedSilently` prove reject/override, warn, and allow-silently behavior
    (`tests/unit/filesystem_classifier_test.cpp:14-37`).

19. **PASS — POSIX root mode.** On this macOS host,
    `RepositoryTest.RepositoryRootHasOwnerOnlyPermissions` passed and observed mode `0700`
    (`tests/integration/repository_test.cpp:332-342`).

20. **PASS — Read-only no-create/no-lock, immutable proof, and sidecar edges.** The no-create/
    no-lock behavior and ordinary WAL pair, missing pair, sidecar-free writable-storage rejection,
    and immutable URI no-create paths are exercised by repository tests 13-16 and database tests
    23-24 in the current development CTest listing. The repaired
    `RepositoryTest.ChmodOnlyRepositoryWithoutWalSidecarsIsRejected` confirms that mode `0500`
    alone no longer selects immutable mode and that rejection creates no entries
    (`tests/integration/repository_test.cpp:270-285`). Platform source inspection proves lifecycle
    selection now requires `ST_RDONLY`/`FILE_READ_ONLY_VOLUME`, while
    `Database.ExplicitImmutableReadOnlyCreatesNoFilesForEncodedPath` proves the selected explicit
    immutable connection can read without creating paths. Read-only open still takes no writer lock,
    proved by concurrent acquisition in
    `RepositoryTest.ReadOnlyOpenCreatesNoEntriesAndTakesNoWriterLock`. All nine repaired/adjacent
    tests passed independently in development and direct ASan/UBSan runs.

### Platform and process

21. **PENDING — Exact-M1-commit CI.** `git status --short` shows the M1 implementation and its log
    as modified/untracked while `HEAD` and `origin/main` are still the same M0 commit
    `a9ac009863ae8e9c16f99f25ccbfdeb146dde305`. Therefore no exact M1 commit exists for the three CI
    jobs to have proven; local macOS success cannot substitute for Linux/macOS/Windows CI.

22. **PENDING — Windows VM manual verification.** No independent evidence was provided or produced
    for observing contention in a current Windows VM or opening a Mac-created repository read-only
    there. This is a Richard/user manual gate and is not self-certified.

23. **PASS — Implementation log.** The implementation log exists at
    `docs/implementation-logs/M1/2026-07-12-codex-database-repository-lifecycle.md` and records the
    operation-history decision, immutable/sidecar and cleanup decisions, FR-005 amendment, Windows
    flush deviation, sanitizer gotcha, FR map, and later-milestone gotchas. Its positive read-only
    proof claim now matches the repaired platform implementation described in rows 2 and 20.

24. **PASS — Full verification checklist state.** This verification log contains all 25 requested
    rows with one explicit state and concrete source/test/command evidence for each, followed by
    totals and gate verdict.

25. **PASS — Functional-requirement mapping and FR-005 amendment.** The implementation log maps
    FR-001 to `CreateAndOpenRoundTripPreservesRepositoryInformation`,
    `ExistingNonEmptyDirectoryRequiresApproval`, and
    `ApprovedNonEmptyDirectoryPreservesExistingContent`; FR-002 to
    `RandomDirectoryIsNotARepository`, `MissingRequiredSchemaObjectIsRejected`, and
    `OpenRejectsSymlinkLayoutEntry`; FR-003 to `FutureFormatVersionIsRejectedClearly` and
    `RealFormatVersionIsRejectedWithoutIntegerTruncation`; and FR-004 to the distinct-process
    `SecondProcessReceivesRepositoryBusy` test. It explicitly defers FR-005 to M4 and does not claim
    a recovery proving test (`docs/implementation-logs/M1/2026-07-12-codex-database-repository-lifecycle.md:35-48`).

## Deviations and gate verdict

- Evidence deviation: the strict configure is successful but reports the deprecated
  `SQLite::SQLite3` CMake target as a non-fatal author warning.
- External evidence remains absent for exact-M1-commit tri-platform CI and both Windows VM checks.

**Totals: 23 PASS, 0 FAIL, 2 PENDING (25 rows).**

**M1 gate verdict:** code-complete on the current local tree: every implementation and test row is
PASS. M1 is not yet milestone-complete because rows 21 and 22 remain PENDING. Exact-M1-commit CI
must pass on all three platforms and the user/Richard Windows VM checks must be recorded before M1
can be declared milestone-complete.
