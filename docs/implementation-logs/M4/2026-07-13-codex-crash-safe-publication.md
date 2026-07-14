# Milestone 4 Implementation Log

Date: 2026-07-13
Milestone: Crash-safe object and snapshot publication
Baseline: `e1ce408e5bb27ce9a28ad03dffbd68a5d36a1683`

## Decisions

- Keep M4 single-threaded. `ObjectStore` publishes unique files from `temporary/objects` only after
  file flush and sync; POSIX uses atomic no-replace rename followed by shard-directory sync, while
  Win32 uses `FlushFileBuffers` and write-through publication. A new shard's parent link is synced
  on every attempt so a retry cannot skip a previously failed durability barrier.
- Preserve the required ordering: durable object, `chunks` row, entry mapping, then a separate final
  transaction that derives counters from SQLite and flips `pending` to `complete`. Only complete
  snapshots are returned by list, browse, and restore queries.
- Bound snapshot metadata at 500 records or 64 MiB of completed entry bytes, with a private test
  override. Scan warnings count as records. Failed, cancelled, and deleting cleanup removes entries
  and warnings in deterministic bounded batches; objects remain for M6 GC.
- Run recovery after the repository's exclusive lock is acquired on its first snapshot or restore:
  fail and clean stale pending snapshots, finish deleting snapshots, clear repository temporary
  residue, retain orphan objects, and run the relationship check. Interrupted cleanup remains
  resumable and preserves the original failure.
- Add the six exact M4 failure points behind an owning no-op-default `FailureInjector`. Ordinary
  throws cover all six; child-process `_Exit(86)` covers the five repository-owned snapshot points.
  Abrupt restore termination is not advertised as repository-cleanable because its temporary output
  may be beside an arbitrary external destination; its throw path proves RAII cleanup and unchanged
  destination publication.

## Functional requirements and proving tests

- **FR-005 — interrupted-operation recovery:**
  `M4CrashSafety.ProcessExitAtSnapshotFailurePointsRecoversCommittedState`,
  `RepositoryTest.FirstSnapshotOperationRecoversSnapshotsAndTemporaryTreeButKeepsOrphans`, and
  `RepositoryTest.FirstRestoreOperationRunsRecoveryBeforeReadingSnapshotMetadata`.
- **FR-111 — valid-snapshot publication:**
  `M4CrashSafety.EveryFailurePointPreservesPriorSnapshotAndCleansTemporaryResidue`,
  `SnapshotEngineTest.FailureBeforeFinalPublicationLeavesNoVisibleOrReferencedSnapshot`, and
  `StoreFixture.PublishesOnlyCompleteSnapshotsWithDatabaseDerivedFinalCounters`.
- **FR-113 — cancellation:**
  `SnapshotEngineTest.CancellationMidSnapshotMarksCancelledAndCleansCommittedMetadata`.
- **FR-408 (partial; full orphan GC remains M6) — stale temporary/incomplete cleanup:**
  the process-exit test, `RepositoryTest.InterruptedPendingCleanupRetriesWithoutLosingFailureInformation`,
  `RepositoryTest.InterruptedWarningCleanupFinishesAfterRepositoryReopen`, and the manual recovery
  probe. Orphan content objects are deliberately retained for M6.

## Local evidence

- Warning-strict development build and CTest passed: 127/127 runnable tests, 0 failures; only the
  opt-in external-dataset test was skipped. The focused M4/acceptance/dedup/corruption/object-store
  matrix passed 32/32.
- The sanitizer preset reproduced Apple ASan's unsupported leak-detection error. Running the freshly
  built executable with `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1` and
  `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1` passed 127 runnable tests with no diagnostic.
- The deterministic 2 GiB `large-files` dataset (`seed=12345`) restored byte-identically in 172.93
  seconds; `/usr/bin/time -l` measured maximum RSS 17,416,192 bytes.
- All repository C++ files, including new untracked helpers, passed clang-format dry-run. Ruff,
  Python syntax, `git diff --check`, and the complete development suite passed.
- A fresh critical review initially reported five durability/coverage issues. Repairs added the
  new-shard parent sync and retry barrier, durable orphan adoption, correct rename injection order,
  bounded warning cleanup, real process-exit coverage, and a post-batch manual probe. The same
  reviewer rechecked all five as closed with no regressions.

## Manual Windows gate

`localvault_m4_recovery_probe` creates a complete baseline plus at least one committed 500-record
pending batch before printing `SAFE TO KILL NOW`. After Task Manager terminates it, `verify` requires
the old snapshot to restore byte-identically and incomplete metadata and repository temp residue to
be absent. This external gate must be recorded by the independent verifier before M4 completion.
