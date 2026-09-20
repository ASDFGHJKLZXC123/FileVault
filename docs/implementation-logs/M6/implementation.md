# M6 implementation decision record

## Decisions

- M6 exposes core services only: streaming/keyset-paged diff, verification, deletion/GC, and
  metadata queries/statistics. CLI commands and their exit-code adapter remain M7; `ok()` is false
  for health errors and true for maintenance notes. Optional whole-file hash reconstruction is
  deferred; full verification checks every distinct referenced chunk with bounded decompression.
- GC defaults to preview. Preview predicts post-recovery references without running recovery;
  execution always forces recovery after acquiring the repository lock, then rechecks references
  within bounded transactions and removes objects before rows. No reference counts are stored.
- Preview and verification acquire the existing lock without rewriting diagnostics. SQLite's
  ordinary read transactions can update WAL shared-memory readmarks, so byte preservation needs
  a separate read-only, heap-WAL-index connection under that lock; the original database handle
  remains open throughout. Immutable SQLite mode is unsuitable because it ignores live WAL data.
- Canonical object paths and no-follow filesystem checks prevent redirected cleanup; repository
  root resolution is remembered at open and revalidated before maintenance. Unknown object names
  remain untouched. Cancellation is checked between metadata batches and temporary removal calls;
  a single synchronous filesystem removal is not interruptible.
- Diff holds a consistent SQL read transaction over two ordered cursors. QueryService uses
  complete snapshots only, literal case-sensitive path search, bounded pages, and checked unsigned
  sums; zero denominators yield zero savings, while incompressible content can yield negative savings.
- Shared SQLite binding now distinguishes empty TEXT from NULL, including default string views.

## Plan amendments and scope

- Development for this session is native Windows; macOS, Linux, Windows, ASan/UBSan, and TSan
  remain required CI gates. No VM/manual exercise is required by M6, and no Mac-local run is claimed.
- The orchestrator's named Spark/opus/sonnet routes are unavailable; available inherited agents
  handled disjoint packets, dedicated GC review, and fresh independent verification. Review repair
  authorship is disclosed in `critical-review.md`; verification must inspect those changes afresh.
- The M6 checklist clarifies the core verification result versus M7's command exit mapping, and
  accepts the active supported native development host plus all five existing CI jobs.

## Requirements → proving tests

Suite prefixes are shown once per row; full acceptance output and status belong in `verification.md`.

| Requirements | Proving tests |
|---|---|
| FR-201 metadata/statistics | `QueryServiceTest.SnapshotPagesAreNewestFirstWithStableIdTieBreakAndMetadata`, `SingleSnapshotStatisticsUseFilesAndReferencedChunksOnly`, `ChunksSharedAcrossSnapshotsCountUniqueBytesOnce`, `UnsignedStatisticsTotalsAllowSignedRangeAndRejectOverflow` |
| FR-202 browsing | `QueryServiceTest.ChildrenArePagedDirectDescendantsAndExcludeRootSentinel` |
| FR-203 search | `QueryServiceTest.SearchIsLiteralCaseSensitiveAndPagedAcrossRelativePaths` |
| FR-204 diff | `DiffEngineTest.ClassifiesAllChangesInSortedStreamingOrder`, `LongAndUnicodePathsFollowSqlByteOrder`, `StreamingUsesOneConsistentViewAcrossConcurrentDeletion` |
| FR-400/401 quick/full | `IntegrityVerifierTest.HealthyQuickAndFullCheckDistinctReferencedObjects`, `AllIndependentCorruptObjectsAreReportedWithoutRepair` |
| FR-402/403 missing/corrupt | `IntegrityVerifierTest.MissingReferencedObjectIsFatalInBothModes`, `TruncatedObjectIsFatalInQuickMode`, `ModifiedCompressedObjectIsDetectedOnlyByFullMode`, `ValidZstdWithWrongContentFailsBlake3` |
| FR-404 relationships | `IntegrityVerifierTest.ForeignKeyAndMissingChunkRowsAreReported`, `InvalidChunkSequenceOffsetAndTotalsAreReported` |
| FR-405 deletion | `GarbageCollectorTest.CancelledDeletionRemainsResumableAndRetainsObjects`, `RejectsPendingAndMissingSnapshotsWithoutPublishingDeletion` |
| FR-406 preview | `GarbageCollectorTest.PreviewLeavesEntireRepositoryAndDatabaseByteIdentical`, `PreviewPredictsOrphansAfterStaleReferencesAreRecovered` |
| FR-407 safe collection | `GarbageCollectorTest.DeleteThenGcPreservesSharedChunksAndRestoresOlderSnapshot`, `InterruptedObjectDeletionLeavesRecoverableUnreferencedRow`, `RejectsRepositoryRootRedirectedAfterOpen` |
| FR-408 stale recovery | `GarbageCollectorTest.PreviewPredictsRecoveryWithoutChangingStaleReferences`, `RepositoryTest.CancelledForcedRecoveryStopsBetweenBatchesAndRetainsCompleteSnapshot` |
