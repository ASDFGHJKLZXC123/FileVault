# M6 independent verification

Date: 2026-09-20. Status: **19 of 19 checklist items satisfied**. Implementation,
focused acceptance tests, the native full suite, and all five CI jobs pass.
Final CI evidence below was appended by the root orchestrator; the independent
source review and local-evidence verification above it retain their original authorship.

This verifier independently read `AGENTS.md`, the M6 milestone and its required
sections in plan parts 04–07, 09, and 11, the implementation decision record,
both critical-review passes, the four new services, the database/locking changes,
the recovery changes, and their tests. The verifier authored this log only: no
implementation/test edits and no competing build or test process. Available inherited
agents were used because the orchestrator's named model routes were unavailable.
Runtime evidence below was produced by the root agent and inspected from its logs.

## Execution evidence

- Actual local host: Windows 11, MSVC 19.44, Visual Studio 2022 generator,
  Debug, CMake 4.3.3, pinned vcpkg manifest. The existing isolated build is
  `build/m5-junction/cmake`; `LOCALVAULT_WARNINGS_AS_ERRORS=ON`, GUI and
  benchmarks OFF. No Mac-local run or VM exercise is claimed.
- Warning-strict build succeeds: `build/m5-junction/m6-build.log`.
- Focused run: `build/m5-junction/m6-focused-tests.log`, 72 registered tests,
  68 passed, 4 skipped for unavailable symlink privilege, 0 failed, 16.42 seconds.
  This includes all four M6 suites, forced-recovery cancellation, the live-WAL
  zero-write database tests, and empty-TEXT binding regression.
- The four focused skips are
  `GarbageCollectorTest.RejectsRedirectedObjectDirectoriesWithoutTouchingTargets`,
  `GarbageCollectorTest.RejectsTemporaryIndirectionWithoutTouchingItsTarget`,
  `IntegrityVerifierTest.DirectoryIndirectionIsNotTraversed`, and
  `IntegrityVerifierTest.ReferencedObjectIndirectionIsFatalWithoutReadingTarget`.
  POSIX-only root-redirection and native-lock preservation tests are not registered
  on Windows. Both passed on Linux, macOS, and Linux ASan/UBSan CI.
- Final full run: `build/m5-junction/m6-full-tests.log`, 246 registered,
  235 passed, 11 skipped, 0 failed, 56.67 seconds. Ten skips require unavailable
  symlink privilege; the other is the opt-in external M3 large-file dataset.
  The initial run exposed two M4 crash-matrix tests that expected the new GC-only
  failure point without running GC. Root extended both tests to exercise collection:
  `M4CrashSafety.EveryFailurePointPreservesPriorSnapshotAndCleansTemporaryResidue`
  now injects failure after removing an unreferenced object and restores the retained
  snapshot; `M4CrashSafety.ActualSnapshotRestoreAndDeletingLifecycleHitsEveryFailurePoint`
  now collects after deletion and still requires every failure seam. This verifier
  inspected the extensions and the successful final log; the matrix was not weakened.
- Subsequent fixture-only portability adjustment: the GC-after-delete restore test
  canonicalizes its existing temporary parent before appending `restored`, avoiding
  an incidental macOS `/var` alias in the destination's ancestor chain. This verifier
  inspected the one-line diff; production code and byte-comparison assertions are
  unchanged. Root reports the warning-strict rebuild passes, and the inspected
  `build/m5-junction/m6-canonical-restore-test.log` records the affected acceptance
  test passing, 1/1, 0 failures, 0.30 seconds. The full-suite run above remains the
  production baseline; final CI must include this fixture adjustment.
- Formatting: root reports `clang-format --dry-run --Werror` passes on all
  14 new C++ files. This verifier did not independently rerun formatting.
- Final implementation CI: `970df1a4dfa5554be6065936705e464ca3354fc8`,
  [run 35544431396](https://github.com/ASDFGHJKLZXC123/FileVault/actions/runs/35544431396).
  All five jobs succeeded. No previous milestone's run is counted as M6 evidence.

  | Job | Result and executed-suite evidence |
  |---|---|
  | [Linux](https://github.com/ASDFGHJKLZXC123/FileVault/actions/runs/35544431396/job/106167724452) | 251 passed, 1 opt-in dataset skip; 6.89 s |
  | [macOS](https://github.com/ASDFGHJKLZXC123/FileVault/actions/runs/35544431396/job/106167724390) | 251 passed, 1 opt-in dataset skip; manual M5 benchmark disabled; 23.57 s |
  | [Windows](https://github.com/ASDFGHJKLZXC123/FileVault/actions/runs/35544431396/job/106167724396) | 245 passed, 1 opt-in dataset skip; 34.70 s; symlink tests ran successfully |
  | [Linux ASan/UBSan](https://github.com/ASDFGHJKLZXC123/FileVault/actions/runs/35544431396/job/106167724415) | 251 passed, 1 opt-in dataset skip; 24.22 s; no sanitizer failure |
  | [Linux TSan](https://github.com/ASDFGHJKLZXC123/FileVault/actions/runs/35544431396/job/106167724314) | Existing 19-test concurrency filter passed; 2.98 s; no sanitizer failure |

  Root inspected job conclusions and logs, including the shared-chunk restore,
  zero-write preview, POSIX root-redirection, and native database-lock regressions.
  The final documentation-only closure is pushed separately and its CI is checked
  before announcing branch finalization; no implementation changes follow this run.
- Root CI supplement: initial implementation `f71290281ed128b409b0dd7aef1dbfc0c4ecbd68`
  in [run 35544208683](https://github.com/ASDFGHJKLZXC123/FileVault/actions/runs/35544208683)
  passed Linux and TSan. macOS built successfully and failed only the temporary-parent
  alias fixture described above; both POSIX root-redirection and native-lock-preservation
  tests passed there and on Linux. The corrected fixture passed the complete run above.

## Independent source conclusions

No additional blocking source defect was found in this walk. The following
conclusions are source inspection backed by the named tests, with platform execution
limits stated above.

`DiffEngine` keeps two prepared, ordered SQL cursors within one read transaction,
advances only the lower/equal key, and holds one current entry per cursor. An empty
exclusive key excludes the root sentinel. File hashes and exact stored link text
precede metadata classification; directory mtime suppression preserves mode changes.

Verification runs without recovery and records independent errors rather than
repairing data. Full mode visits the unique `chunks` rows with references and
delegates bounded decompression and BLAKE3 validation to `ObjectStore::read_verified`.
Missing unreferenced objects, stale states, temporary files, and orphans are notes;
missing/corrupt referenced objects and invalid relationships are errors. `ok()` is
false when any error exists. Optional whole-file reconstruction is explicitly
deferred; CLI mapping to exit 5 belongs to M7.

GC execution acquires the repository lock, forces recovery even after prior recovery,
then selects rows using `NOT EXISTS` over `entry_chunks` (the anti-join equivalent of
§22.2's LEFT JOIN). Each bounded transaction rechecks references before removing the
canonical object and then its row. No authoritative reference count was added.
Complete references protect shared chunks; interrupted object removal leaves a
retryable unreferenced row. Orphans must have canonical lowercase hash/shard names;
unknown names and live references are retained.

Preview opens a separate `locked_read_only` database under the existing repository
lock, without changing lock diagnostics or invoking recovery. It predicts recovery
when determining stale references and orphans. The custom VFS opens database/WAL
files read-only, refuses repository writes/truncation/deletion and shared-WAL-index
mapping, and uses SQLite's heap WAL index. Native SHARED locks, unlocks, and close
accounting remain delegated; only the EXCLUSIVE upgrade is synthetic. Anonymous
delete-on-close SQLite scratch files may be used outside the repository. The original
database handle remains open. This is a repository-byte-preservation guarantee under
the cooperative repository lock, not immutable mode that would miss live WAL data.

The reviewed recovery helper checks cancellation before/after metadata failure-hook
boundaries and between temporary subtree removals; failed/cancelled work remains
retryable. Root validation remembers the canonical path at repository open and
rechecks it and canonical ancestors before maintenance and removals. This remains
path-based: it does not prove inode identity or eliminate replacement races by an
uncooperative process. One synchronous subtree removal is not interruptible per file.

Statistics include only complete snapshots and distinct referenced chunks. They sum
individual signed SQLite values through checked unsigned accumulation, avoiding
signed SQL-SUM overflow, and implement all three §18.8 formulas. Zero denominators
return zero; compressed expansion is allowed to produce negative savings.

## All eleven quick-verification checks

| §21.1 check | Source/evidence |
|---|---|
| 1. SQLite integrity | `verify` executes `PRAGMA integrity_check`, reports each non-`ok` row; healthy quick/full tests exercise the path. |
| 2. Foreign keys | `PRAGMA foreign_key_check`; `ForeignKeyAndMissingChunkRowsAreReported` deliberately disables FK enforcement and removes a chunk row. |
| 3. Exactly one repository row | `check_repository_info`; `RepositoryInfoMustHaveExactlyOneRow`, `DuplicateRepositoryInfoRowsAreRejected`. |
| 4. Format and algorithms | Explicit format/hash/encoding/type/limit validation; `RepositoryFormatAndAlgorithmAreRecheckedAfterOpen`, `InvalidRepositoryChunkLimitIsReportedAfterOpen`. |
| 5. Complete counters | `check_snapshots` compares stored counters with actual entries and rejects invalid types/negative values; `CompleteSnapshotCountersMustMatchEntries`. |
| 6. Chunk relationships | Left-joined chunk rows, contiguous sequence/offset/totals; `ForeignKeyAndMissingChunkRowsAreReported`, `InvalidChunkSequenceOffsetAndTotalsAreReported`. |
| 7. Referenced object existence | `check_objects` and no-follow path inspection; `MissingReferencedObjectIsFatalInBothModes`. |
| 8. Compressed size | Actual file size compared with metadata; `TruncatedObjectIsFatalInQuickMode`. |
| 9. Derived object path | Hash-derived path compared with stored path, stored path never followed; `ForgedObjectPathIsRejectedWithoutFollowingIt`. |
| 10. Stale snapshots | Pending, failed, cancelled, deleting states are maintenance notes; `StaleSnapshotsTemporaryFilesAndOrphansAreOnlyNotes`, `PendingUnfinishedFileMetadataIsOnlyANote`. |
| 11. Stale temporary files | No-follow storage walk; `StaleSnapshotsTemporaryFilesAndOrphansAreOnlyNotes`. |

Test names in this table have the `IntegrityVerifierTest.` prefix.

## Completion checklist

The 19 original checklist items are reproduced below. Checked implementation/test
items mean the source and cited focused evidence satisfy that item; the combined
platform gate remains open and therefore M6 is not yet complete.

**Implementation**

- [x] `DiffEngine`: two sorted SQL cursors, streaming merge, classification per §20.2 (symlink target change = `content_modified`), directory-mtime-noise suppression option, root row excluded.
  Evidence: `DiffEngineTest.ClassifiesAllChangesInSortedStreamingOrder`,
  `ComparesLinkTextExactlyAndClassifiesLinkMetadata`,
  `SuppressesDirectoryMtimeNoiseButRetainsModeChanges`,
  `EmptySnapshotsAndRootSentinelsProduceNoResults`,
  `StreamingUsesOneConsistentViewAcrossConcurrentDeletion`.
- [x] Quick verification performs all eleven §21.1 checks (integrity_check, FK check, `repository_info` row, format/algorithms, counters, chunk rows, object existence, object sizes, derived-path match, stale non-complete snapshots, stale temp files).
  Evidence: the eleven-check table above and focused verifier tests.
- [x] Full verification decompresses and re-hashes every distinct referenced chunk; reports all issues rather than stopping at the first (§21.2). Decision recorded on the optional `--files` reconstruction mode.
  Evidence: `IntegrityVerifierTest.HealthyQuickAndFullCheckDistinctReferencedObjects`,
  `AllIndependentCorruptObjectsAreReportedWithoutRepair`,
  `ValidZstdWithWrongContentFailsBlake3`; optional mode deferred in `implementation.md`.
- [x] Issue severity per §21.3: missing/corrupt **referenced** objects make the core verification result fail; orphans are nonfatal GC candidates. M7 maps this result to command exit code 5 (zero for clean-with-notes).
  Evidence: `IntegrityVerifierTest.MissingReferencedObjectIsFatalInBothModes`,
  `StaleSnapshotsTemporaryFilesAndOrphansAreOnlyNotes`,
  `MissingUnreferencedObjectIsStaleMetadataAndNeverDeleted`; M7 adapter is not claimed implemented.
- [x] Snapshot delete: refuses a pending-in-use snapshot, uses the M4 `deleting` machinery, keeps objects unless `--gc` (§22.1).
  Evidence: `GarbageCollectorTest.RejectsPendingAndMissingSnapshotsWithoutPublishingDeletion`,
  `CancelledDeletionRemainsResumableAndRetainsObjects`,
  `DeleteThenGcPreservesSharedChunksAndRestoresOlderSnapshot`. Explicit collection is a
  separate core call; the CLI `--gc` adapter belongs to M7.
- [x] GC order: exclusive lock → recovery → unreferenced query (§22.2) → recheck → delete object file, then row, in bounded batches (§22.4); orphan object files handled per §22.5.
  Evidence: `GarbageCollectorTest.PreviewPredictsRecoveryWithoutChangingStaleReferences`,
  `DeleteThenGcPreservesSharedChunksAndRestoresOlderSnapshot`,
  `InterruptedObjectDeletionLeavesRecoverableUnreferencedRow`,
  `IgnoresNonCanonicalOrphanNamesAndRetainsLiveObjects`; source review confirms transaction order.
- [x] `gc --dry-run` reports counts/bytes and performs zero writes (§22.3).
  Evidence: `GarbageCollectorTest.PreviewLeavesEntireRepositoryAndDatabaseByteIdentical`,
  `PreviewPredictsRecoveryWithoutChangingStaleReferences`,
  `PreviewPredictsOrphansAfterStaleReferencesAreRecovered`, and
  `Database.LockedReadOnlyReadsLiveWalWithoutChangingAnyDatabaseFile`.
- [x] Statistics per §18.8 formulas with the §42.9 queries and checked arithmetic (§42.5); zero denominators return 0.0.
  Evidence: `QueryServiceTest.SingleSnapshotStatisticsUseFilesAndReferencedChunksOnly`,
  `ChunksSharedAcrossSnapshotsCountUniqueBytesOnce`, `EmptyFilesHaveZeroDenominatorSavings`,
  `UnsignedStatisticsTotalsAllowSignedRangeAndRejectOverflow`, `ChunkStatisticsUseCheckedUnsignedAggregates`.

**Tests (all green)**

- [x] Diff battery (§32.2): added, removed, type changed, content modified, metadata modified, unchanged, empty snapshots, very long path, streaming merge order.
  Evidence: `DiffEngineTest.ClassifiesAllChangesInSortedStreamingOrder`,
  `EmptySnapshotsAndRootSentinelsProduceNoResults`, `LongAndUnicodePathsFollowSqlByteOrder`
  (16,000-byte path), and `PagesByExclusiveKeyAndFiltersUnchangedRows`.
- [x] Verification corruption matrix (§32.3): healthy quick, healthy full, missing referenced object, truncated object, modified compressed bytes, wrong `raw_size`, invalid object path in DB, FK inconsistency in a malformed test DB — each detected and classified correctly (acceptance).
  Evidence: `IntegrityVerifierTest.HealthyQuickAndFullCheckDistinctReferencedObjects`,
  `MissingReferencedObjectIsFatalInBothModes`, `TruncatedObjectIsFatalInQuickMode`,
  `ModifiedCompressedObjectIsDetectedOnlyByFullMode`, `WrongRawSizeAndDecompressionSizeAreReported`,
  `ForgedObjectPathIsRejectedWithoutFollowingIt`, `ForeignKeyAndMissingChunkRowsAreReported`.
- [x] Delete one of two snapshots sharing chunks: shared objects retained.
  Evidence: `GarbageCollectorTest.DeleteThenGcPreservesSharedChunksAndRestoresOlderSnapshot`
  asserts two chunk rows remain immediately after deleting the newer snapshot.
- [x] **GC-after-delete restore test: delete snapshot B, run GC, then fully restore snapshot A byte-identically** — the single most important test in the project (acceptance: GC never removes shared chunks).
  Evidence: `GarbageCollectorTest.DeleteThenGcPreservesSharedChunksAndRestoresOlderSnapshot`
  restores both older files, compares each file's bytes, excludes the newer-only file,
  and proves repeat GC removes no more objects.
- [x] Dry run: repository directory tree and DB are byte-identical before/after (acceptance).
  Evidence: `GarbageCollectorTest.PreviewLeavesEntireRepositoryAndDatabaseByteIdentical`
  captures every repository directory and file, including database, WAL, SHM, lock,
  objects, and temporary data; equality is checked after preview returns.
- [x] GC removes unreferenced objects and their rows; orphan object file detected; stale temp removed.
  Evidence: `GarbageCollectorTest.PreviewLeavesEntireRepositoryAndDatabaseByteIdentical`
  then executes GC, checks two removed objects, exact reclaimed bytes, missing orphan/temp,
  and zero remaining chunk rows.
- [x] Interrupted GC (injected failure between object delete and row delete) is recoverable by the next GC.
  Evidence: `GarbageCollectorTest.InterruptedObjectDeletionLeavesRecoverableUnreferencedRow`
  proves the object is gone while the row remains, preview reports zero reclaimable bytes,
  and retry removes the row successfully.
- [x] Statistics: empty repository, single snapshot, duplicate chunks, shared chunks across snapshots, zero denominators.
  Evidence: `QueryServiceTest.EmptyRepositoryHasZeroStatisticsAndNoSnapshots`,
  `SingleSnapshotStatisticsUseFilesAndReferencedChunksOnly`,
  `DuplicateChunksWithinSnapshotCountUniqueBytesOnce`,
  `ChunksSharedAcrossSnapshotsCountUniqueBytesOnce`, `EmptyFilesHaveZeroDenominatorSavings`.

**Platform & CI**

- [x] Active native development host suite green; Linux, macOS, Windows, Linux ASan/UBSan, and Linux TSan CI jobs green. Record the actual local host; do not claim an unavailable Mac-local run. No VM session required (optionally verify a VM-created repository for a free portability point).
  Native Windows full suite passes. All five jobs pass in run 35544431396 for
  `970df1a4dfa5554be6065936705e464ca3354fc8`; job links and runtime evidence are above.
  Windows host and exact skip scope are recorded above.

**Process**

- [x] Implementation + verification logs under `docs/implementation-logs/M6/`, including this checklist's state.
  Evidence: `implementation.md`, `critical-review.md` (including supplemental VFS review),
  and this independent `verification.md`.
- [x] Log records FR-201–FR-204 and FR-400–FR-408 with proving test names.
  Evidence: all 13 requirements are mapped individually below.

## Requirement-to-test mapping

These map core requirements; M7 supplies command presentation and exit-code adapters.
All listed tests passed in the focused Windows run unless explicitly marked POSIX-only.

| Requirement | Proving tests |
|---|---|
| FR-201 — snapshot metadata/statistics | `QueryServiceTest.SnapshotPagesAreNewestFirstWithStableIdTieBreakAndMetadata`, `SingleSnapshotStatisticsUseFilesAndReferencedChunksOnly`, `ChunksSharedAcrossSnapshotsCountUniqueBytesOnce`, `UnsignedStatisticsTotalsAllowSignedRangeAndRejectOverflow`. |
| FR-202 — browse snapshot entries | `QueryServiceTest.ChildrenArePagedDirectDescendantsAndExcludeRootSentinel`, `IncompleteSnapshotsAndUnreferencedChunksAreExcluded`. |
| FR-203 — search relative paths | `QueryServiceTest.SearchIsLiteralCaseSensitiveAndPagedAcrossRelativePaths`. |
| FR-204 — compare snapshots | `DiffEngineTest.ClassifiesAllChangesInSortedStreamingOrder`, `LongAndUnicodePathsFollowSqlByteOrder`, `StreamingUsesOneConsistentViewAcrossConcurrentDeletion`. |
| FR-400 — quick verify | `IntegrityVerifierTest.HealthyQuickAndFullCheckDistinctReferencedObjects`, `CompleteSnapshotCountersMustMatchEntries`, `RepositoryFormatAndAlgorithmAreRecheckedAfterOpen`. |
| FR-401 — full verify | `IntegrityVerifierTest.HealthyQuickAndFullCheckDistinctReferencedObjects`, `AllIndependentCorruptObjectsAreReportedWithoutRepair`, `ValidZstdWithWrongContentFailsBlake3`. |
| FR-402 — missing objects | `IntegrityVerifierTest.MissingReferencedObjectIsFatalInBothModes`, `MissingUnreferencedObjectIsStaleMetadataAndNeverDeleted`. |
| FR-403 — corrupt objects | `IntegrityVerifierTest.TruncatedObjectIsFatalInQuickMode`, `ModifiedCompressedObjectIsDetectedOnlyByFullMode`, `WrongRawSizeAndDecompressionSizeAreReported`, `ForgedObjectPathIsRejectedWithoutFollowingIt`. |
| FR-404 — invalid relationships | `IntegrityVerifierTest.ForeignKeyAndMissingChunkRowsAreReported`, `InvalidChunkSequenceOffsetAndTotalsAreReported`. |
| FR-405 — transactional deletion | `GarbageCollectorTest.CancelledDeletionRemainsResumableAndRetainsObjects`, `RejectsPendingAndMissingSnapshotsWithoutPublishingDeletion`; existing `RepositoryTest.InterruptedDeletingRecoveryRetriesAndFinishesOnTheSameOpenRepository`. |
| FR-406 — preview GC | `GarbageCollectorTest.PreviewLeavesEntireRepositoryAndDatabaseByteIdentical`, `PreviewPredictsOrphansAfterStaleReferencesAreRecovered`; `Database.LockedReadOnlyReadsLiveWalWithoutChangingAnyDatabaseFile`. |
| FR-407 — collect only unreferenced objects | `GarbageCollectorTest.DeleteThenGcPreservesSharedChunksAndRestoresOlderSnapshot`, `InterruptedObjectDeletionLeavesRecoverableUnreferencedRow`, `InvalidStoredPathCannotDeleteOutsideRepository`, `IgnoresNonCanonicalOrphanNamesAndRetainsLiveObjects`. Additional passing POSIX CI evidence: `RejectsRepositoryRootRedirectedAfterOpen`, `Database.LockedReadOnlyClosePreservesNativeDatabaseLocksAcrossProcesses`. |
| FR-408 — stale cleanup | `GarbageCollectorTest.PreviewPredictsRecoveryWithoutChangingStaleReferences`, `PreviewLeavesEntireRepositoryAndDatabaseByteIdentical`, `PreviewPredictsOrphansAfterStaleReferencesAreRecovered`; `RepositoryTest.CancelledForcedRecoveryStopsBetweenBatchesAndRetainsCompleteSnapshot`. |

Within a table cell, unqualified test names keep the preceding suite prefix.
The FR-405 existing recovery test passed in the full run; it was not part of the
focused filter. Stale failed/cancelled rows retain failure history by §23.4 while
their entries, mappings, and warnings are removed; deleting snapshot rows are removed.
