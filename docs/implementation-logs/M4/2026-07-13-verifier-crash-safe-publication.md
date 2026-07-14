# Milestone 4 Independent Verification — Crash-safe Publication

Date: 2026-07-13
Baseline: `e1ce408e5bb27ce9a28ad03dffbd68a5d36a1683`
Verified M4 implementation head: `ae7810ba849ed782541028ca6c10e0989b12d6f2`
External gates closed: 2026-07-14
Verdict: **M4 passes every implementation, test, platform/CI, and process item.**

## Summary

- **PASS: 20**
- **FAIL: 0**
- **PENDING: 0**
- **Blockers:** none.

The independent audit covered the complete diff from the baseline, including untracked M4 files.
No implementation defect was found. The object publication dependency chain is preserved: object
bytes are synced and atomically published, the containing shard is synced on POSIX, the `chunks`
row is inserted, entry references are committed in bounded transactions, and only a separate final
transaction changes the snapshot from `pending` to `complete`.

## Completion checklist

### Implementation

- [x] **PASS — Object temp files live under `temporary/objects` with unique names, on the same
  filesystem as `objects/`.** `ObjectStore::store` selects `<repository>/temporary/objects`
  (`src/core/storage/object_store.cpp:267-270`); `TemporaryOutputFile` uses exclusive unique
  creation and best-effort destructor cleanup (`posix_file_metadata.cpp:95-131`, with the Win32
  equivalent). `ObjectStoreFixture.RepositoryTemporaryObjectIsUniqueAndCleanedWithoutPublication`
  proves the location, uniqueness, and cleanup.

- [x] **PASS — POSIX durability sequence implemented in the platform layer: flush → fsync
  (`F_FULLFSYNC` on macOS where full durability is required) → `rename` → parent-directory fsync
  (§23.2).** The unbuffered write loop is followed by `F_FULLFSYNC` on Apple or `fsync` elsewhere
  (`posix_file_metadata.cpp:139-194`), atomic no-replace rename uses `renamex_np(RENAME_EXCL)` on
  Apple or `renameat2(RENAME_NOREPLACE)` on Linux (`:260-289`), and
  `flush_containing_directory` fsyncs the directory (`posix_repository_support.cpp:210-224`). The
  object path orders those calls before `ensure_chunk` (`object_store.cpp:270-296`). New shard
  retry safety is explicit: after creating or finding a shard, `objects/` is unconditionally synced
  on every attempt (`object_store.cpp:262-266`), so a retry cannot skip a failed parent-link
  durability barrier.

- [x] **PASS — Win32 durability sequence implemented: flush → `FlushFileBuffers` →
  `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` (§23.2).** The platform write is
  unbuffered `WriteFile`, `TemporaryOutputFile::sync` calls `FlushFileBuffers`, and publication
  always supplies `MOVEFILE_WRITE_THROUGH` while adding `MOVEFILE_REPLACE_EXISTING` for replacement
  (`win32_file_metadata.cpp:153-196,266-283`). Immutable object publication intentionally requests
  the no-replace form, detects an existing destination, and reuses it; this retains the required
  write-through atomic move and avoids overwriting an immutable object. Windows correctly has no
  directory-fsync workaround (`win32_repository_support.cpp:135-137`). Runtime confirmation remains
  part of the pending Windows CI/manual gates, not this code-inspection item.

- [x] **PASS — Snapshot lifecycle: `pending` row committed before scanning; entry metadata commits
  in bounded batches (batch size a constant, overridable in tests); final transaction computes
  counters and flips to `complete` (§16.2–16.3, §23.3).** The autocommitted pending insert precedes
  scanning (`snapshot_engine.cpp:172-188`); defaults are 500 records or 64 MiB with a private test
  override (`include/localvault/snapshot_engine.hpp:44-53`); warnings and entries share the record
  limit and entries also enforce the byte limit (`snapshot_engine.cpp:189-278`). Publication is a
  separate transaction (`:280-287`), and final counters are derived from committed entries before
  the guarded `pending`→`complete` update (`metadata_store.cpp:242-291`).
  `SnapshotEngineTest.TinyBatchOverrideCommitsMultipleBatchesBeforePublishing` and
  `SnapshotEngineTest.ScanWarningsCountTowardTinyMetadataRecordBatches` prove the override and
  warning accounting.

- [x] **PASS — Failure/cancel paths: snapshot marked `failed`/`cancelled` with message; its
  entries/mappings/warnings removed transactionally; objects left for GC.** Snapshot exception
  handling distinguishes cancellation and performs bounded cleanup (`snapshot_engine.cpp:288-304`;
  `metadata_store.cpp:294-310,463-509`). Entry deletion cascades mappings, warning deletion is
  independently bounded, and no object/chunk deletion occurs.
  `SnapshotEngineTest.CancellationMidSnapshotMarksCancelledAndCleansCommittedMetadata`,
  `SnapshotEngineTest.FatalProgressFailureMarksSnapshotFailedAndHidesItFromListings`, and
  `StoreFixture.IncompleteSnapshotCleanupIsTransactionalAndLeavesObjectsForGc` prove the paths.

- [x] **PASS — `deleting` status machinery: batched entry deletion, resumable, final row delete
  (§22.1).** `transition_snapshot_to_deleting` commits invisibility first; deletion drains bounded
  entry and warning batches and deletes the snapshot row in a final transaction
  (`metadata_store.cpp:343-425`).
  `StoreFixture.DeletingSnapshotResumesBoundedEntryDeletionAndCascadesFinalMetadata`,
  `StoreFixture.DeletingSnapshotDrainsWarningsInBoundedOrderedResumableBatches`, and
  `RepositoryTest.InterruptedDeletingRecoveryRetriesAndFinishesOnTheSameOpenRepository` prove
  bounded resume behavior.

- [x] **PASS — Recovery on first mutating operation (§23.4): stale pendings failed and cleaned in
  batches, `deleting` resumed to completion, everything under `temporary/` removed, quick
  relationship check runs.** Snapshot acquires the writer lock and calls recovery before creating
  the pending row (`snapshot_engine.cpp:172-178`); restore does the same before object reads
  (`restore_engine.cpp:559-564`). Recovery handles every non-complete state, clears/recreates the
  temporary subdirectories, preserves orphan final objects, and runs the relationship check
  (`repository.cpp:437-469,746-782`). Windows sharing violations leave recovery incomplete for a
  retry. `RepositoryTest.FirstSnapshotOperationRecoversSnapshotsAndTemporaryTreeButKeepsOrphans`,
  `RepositoryTest.FirstRestoreOperationRunsRecoveryBeforeReadingSnapshotMetadata`, and the three
  interrupted recovery tests prove first-operation and retry behavior.

- [x] **PASS — Only `status='complete'` is visible to list/browse/restore — verified everywhere
  snapshots are queried.** Independent SQL call-site audit found outward reads guarded in
  `require_complete_snapshot`, `list_complete_snapshots`, `list_entries`, `list_children`, and
  `list_entry_chunks` (`metadata_store.cpp:313-340,695-727`); non-complete queries are internal
  recovery/deletion state-machine operations.
  `StoreFixture.BrowseQueriesExposeOnlyCompleteSnapshotMetadata` covers pending, failed,
  cancelled, deleting, and complete rows; restore rejects incomplete snapshots through
  `require_complete_snapshot`.

- [x] **PASS — `FailureInjector` + `Repository::set_failure_injector` seam; production default is
  a no-op; `hit()` called at every §32.5 `FailurePoint`.** The public enum contains the exact six
  values and an exhaustive `all_failure_points` array (`include/localvault/failure_injector.hpp`);
  the repository seam is public (`include/localvault/repository.hpp:50`) and null restores the
  shared no-op (`repository.cpp:474-487,793-794`). Object, batch, publish, delete/recovery, and
  restore paths contain the calls. `RepositoryTest.FailureInjectorDefaultsToNoopAndNullRestoresIt`
  and `M4CrashSafety.ActualSnapshotRestoreAndDeletingLifecycleHitsEveryFailurePoint` prove the seam
  and exhaustive firing.

### Tests (all green)

- [x] **PASS — Injection matrix: for each `FailurePoint`, throw → reopen → assert previously
  complete snapshots restore byte-identically, the interrupted snapshot is not `complete`, and
  recovery leaves no temp files.**
  `M4CrashSafety.EveryFailurePointPreservesPriorSnapshotAndCleansTemporaryResidue` iterates
  `all_failure_points` (`m4_crash_safety_test.cpp:354-443`). The five repository-owned snapshot
  points also receive real child-process `_Exit(86)` coverage with pending-state and residue checks
  in `M4CrashSafety.ProcessExitAtSnapshotFailurePointsRecoversCommittedState` (`:248-352`).
  `during_restore_write` is tested by a throw because an abrupt restore can leave a temp beside an
  arbitrary external destination that repository recovery cannot discover; the test proves RAII
  cleanup and no destination publication.

- [x] **PASS — Meta-test: a counting injector proves every `FailurePoint` enum value fires at least
  once across a snapshot + delete cycle.**
  `M4CrashSafety.ActualSnapshotRestoreAndDeletingLifecycleHitsEveryFailurePoint` performs snapshot,
  restore, deleting transition, and deletion, then checks every value in `all_failure_points`
  (`m4_crash_safety_test.cpp:446-479`).

- [x] **PASS — Crash between object publish and `chunks`-row commit yields an orphan object that is
  tolerated (quick check passes; object queryable as orphan).** The injection is placed after rename
  but before shard sync/chunk metadata (`object_store.cpp:278-290`).
  `M4CrashSafety.RenameBeforeChunkMetadataLeavesRecoverableUnreferencedOrphan` proves the final file
  remains, no `chunks` row exists, recovery succeeds, temp residue is absent, and the relationship
  check passes (`m4_crash_safety_test.cpp:482-517`). Cold reuse fully verifies, re-syncs the orphan
  file and directory, then inserts metadata (`object_store.cpp:213-227`), proved by
  `ObjectStoreFixture.RenameFailureLeavesVerifiableOrphanThatColdStoreAdopts`.

- [x] **PASS — Batch boundaries: with batch size forced tiny, a multi-batch snapshot completes and
  an injected failure between batches recovers.**
  `SnapshotEngineTest.TinyBatchOverrideCommitsMultipleBatchesBeforePublishing` and
  `SnapshotEngineTest.FailureBetweenMetadataBatchesCleansPreviouslyCommittedEntries` passed; the
  warning-record boundary test passed too.

- [x] **PASS — Cancellation mid-snapshot: no partial snapshot becomes `complete`; recovery cleans
  its metadata (FR-113).**
  `SnapshotEngineTest.CancellationMidSnapshotMarksCancelledAndCleansCommittedMetadata` forces a
  one-entry batch, cancels during metadata writing, and asserts cancelled status with no visible
  entries or warnings (`snapshot_engine_test.cpp:318-352`).

- [x] **PASS — Interrupted `deleting` resumes to completion on reopen.** Both
  `StoreFixture.DeletingSnapshotResumesBoundedEntryDeletionAndCascadesFinalMetadata` and
  `RepositoryTest.InterruptedDeletingRecoveryRetriesAndFinishesOnTheSameOpenRepository` passed.

- [x] **PASS — Exception during rollback path does not `std::terminate` and preserves the original
  error (§31.5, §42.7).** `Transaction::~Transaction` is `noexcept`, catches and logs rollback
  errors (`transaction.cpp:14-24`); `Transaction.RollbackFailureDuringUnwindingPreservesOriginalException`
  explicitly forces rollback failure during propagation and verifies the original exception
  identity. The between-batch injection test separately exercises rollback of a live metadata
  transaction.

### Platform & CI

- [x] **PASS — All three CI jobs green (Windows durability code compiles and unit-level tests
  pass).** GitHub Actions
  [`build-and-test` run 29304667949](https://github.com/ASDFGHJKLZXC123/FileVault/actions/runs/29304667949)
  completed successfully for exact M4 implementation head
  `ae7810ba849ed782541028ca6c10e0989b12d6f2`. Linux job `86995471802`, macOS job
  `86995471823`, and Windows job `86995471799` each completed Configure, Build, and Test
  successfully. The first pushed head exposed one GCC `-Wrange-loop-construct` warning in a test;
  binding that structured pair by const reference fixed the warning, and the replacement exact-head
  run is green on all three platforms.

- [x] **PASS — Windows VM (once): kill `localvault` mid-snapshot from Task Manager, reopen,
  recovery is clean and old snapshots restore.** On 2026-07-14, the human operator ran the recovery
  probe, ended `localvault_m4_recovery_probe.exe` through Task Manager after `SAFE TO KILL NOW`, and
  then ran `verify` against the same workspace. The probe reported:
  `VERIFY PASS: recovery removed incomplete metadata and temporary residue; baseline snapshot
  restored byte-identically.` The probe first requires a pending snapshot with at least 500
  committed entries, then triggers recovery and checks that no pending/deleting or incomplete
  metadata/temp residue remains before byte-comparing the restored baseline
  (`localvault_m4_recovery_probe.cpp:222-302`).

### Process

- [x] **PASS — Implementation + verification logs under `docs/implementation-logs/M4/`, including
  this checklist's state.** The implementation decision log and this independent verification log
  are both present.

- [x] **PASS — Log records FR-005, FR-111, FR-113, FR-408 (partial — full GC in M6) with proving
  test names.** See the explicit requirement map below.

## Functional requirement map

- **FR-005 — Recover or clean up incomplete operations after an interrupted run:**
  `M4CrashSafety.ProcessExitAtSnapshotFailurePointsRecoversCommittedState`,
  `RepositoryTest.FirstSnapshotOperationRecoversSnapshotsAndTemporaryTreeButKeepsOrphans`,
  `RepositoryTest.InterruptedPendingCleanupRetriesWithoutLosingFailureInformation`, and
  `RepositoryTest.InterruptedDeletingRecoveryRetriesAndFinishesOnTheSameOpenRepository`.
- **FR-111 — Mark a snapshot restorable only after all required metadata and objects are valid:**
  `M4CrashSafety.EveryFailurePointPreservesPriorSnapshotAndCleansTemporaryResidue`,
  `SnapshotEngineTest.FailureBeforeFinalPublicationLeavesNoVisibleOrReferencedSnapshot`,
  `StoreFixture.PublishesOnlyCompleteSnapshotsWithDatabaseDerivedFinalCounters`, and
  `StoreFixture.BrowseQueriesExposeOnlyCompleteSnapshotMetadata`.
- **FR-113 — Support cancellation without damaging completed snapshots:**
  `SnapshotEngineTest.CancellationMidSnapshotMarksCancelledAndCleansCommittedMetadata`, with the
  prior-complete preservation invariant also covered by the failure matrix.
- **FR-408 (partial; full orphan-object garbage collection is M6) — Remove stale temporary files
  and stale incomplete snapshots:**
  `M4CrashSafety.ProcessExitAtSnapshotFailurePointsRecoversCommittedState`,
  `RepositoryTest.FirstSnapshotOperationRecoversSnapshotsAndTemporaryTreeButKeepsOrphans`, and
  `RepositoryTest.InterruptedWarningCleanupFinishesAfterRepositoryReopen`. Final orphan objects are
  deliberately retained for explicit M6 GC.

## Independent execution evidence (Mac)

- `cmake --preset development -DLOCALVAULT_WARNINGS_AS_ERRORS=ON` — configured successfully.
- `cmake --build --preset development --parallel` — succeeded with compiler warnings treated as
  errors.
- `QT_QPA_PLATFORM=offscreen ctest --preset development` — **127 passed, 0 failed, 1 skipped** out
  of 128 discovered tests. The skip was the opt-in external 2 GiB dataset test because
  `LOCALVAULT_M3_LARGE_DATASET` was not set.
- Focused M4 `ctest -R` selection — **29/29 passed**.
- `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
  build/sanitizers/tests/localvault_tests --gtest_brief=1` — **127 passed, 0 failed, 1 skipped**;
  no sanitizer diagnostic.
- `scripts/check-format.sh`, an additional clang-format dry-run over all tracked and untracked C++
  sources, and `git diff --check e1ce408e...` — all passed.

For the expensive case only, the implementation log records the current M4 tree's deterministic
2 GiB `large-files` (`seed=12345`) round trip as byte-identical in 172.93 seconds with 17,416,192-byte
maximum RSS. I did not rerun it. I independently inspected `benchmarks/generate_dataset.py` and
`M3LargeFileRoundTripTest.ExternalDatasetRoundTripsWithBoundedChunks`: generation is streamed and
deterministic, the manifest carries version/seed/profile/count/bytes, the test requires at least
2 GiB, validates bounded chunk metadata, restores the snapshot, and compares the complete trees
byte-for-byte.
