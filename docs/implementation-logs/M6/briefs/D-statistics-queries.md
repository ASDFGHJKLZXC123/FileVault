# Packet D — statistics and paged queries

## Specification excerpts

§12.8: "Use page-based queries so the GUI never loads an entire large snapshot tree at once."

FR-201: "Show snapshot metadata and statistics." FR-202: "Browse entries within a snapshot."
FR-203: "Search entries by relative path."

§18.8:

```text
logical_bytes     = sum of logical file sizes
unique_raw_bytes  = sum raw sizes of distinct referenced chunks
stored_bytes      = sum compressed sizes of distinct referenced chunks
deduplication_savings = 1 - unique_raw_bytes / logical_bytes
compression_savings   = 1 - stored_bytes / unique_raw_bytes
total_savings         = 1 - stored_bytes / logical_bytes
```

"When a denominator is zero, return `0.0`."

M6 acceptance: "Statistics: empty repository, single snapshot, duplicate chunks, shared chunks across snapshots, zero denominators."

## Boundaries and implementation choices

Own `include/localvault/query_service.hpp`, `src/core/query_service.cpp`,
`tests/integration/query_service_test.cpp`, and this brief. The orchestrator owns
build registration and removal of the preexisting test-only `QueryService` friend seam.

Use the smallest complete implementation of §12.8. Queries read metadata under
a deferred transaction and require no writer lock. Only complete snapshots contribute
to browsing and statistics. Exclude root sentinels from entry pages. Snapshot pages
use newest creation time, then highest ID; entry pages use binary relative-path order.
Normalize parent paths, reject rooted paths and traversal. Search is a case-sensitive
literal substring using SQLite `instr`, as agreed with the orchestrator.

Explicit implementation choices: page limits are 1–10,000; offset plus limit must
fit SQLite's signed integer range; empty search matches all nonroot entries.
Use checked unsigned accumulation of nonnegative SQLite integers rather than SQL
SUM, so supported uint64 totals do not fail at signed int64 aggregate limits.
Negative savings from incompressible content are preserved. Unreferenced chunks,
orphan object files, and chunks referenced only by incomplete snapshots are excluded.

## Required tests and watchpoints

- Empty, single, duplicated-within-snapshot, shared-across-snapshot statistics.
- Incomplete snapshots and unreferenced chunks excluded; zero denominators; negative savings.
- Signed-to-unsigned rejection and aggregate overflow; unsigned totals above INT64_MAX.
- Snapshot metadata/count/page ordering; direct children/root exclusion; literal wildcard search.
- Invalid IDs, unsafe parent paths, invalid page requests; read-only repository queries.
- No object bytes are accessed and no exclusive writer lock is taken.

Implementation and test results are reported to the orchestrator for integration;
this packet does not build, commit, or claim milestone completion.

## Implementation handoff

Added the §12.8 API, with const read methods and default page requests, plus fifteen
`QueryServiceTest` cases. `SingleSnapshotStatisticsUseFilesAndReferencedChunksOnly`,
`DuplicateChunksWithinSnapshotCountUniqueBytesOnce`,
`ChunksSharedAcrossSnapshotsCountUniqueBytesOnce`,
`EmptyFilesHaveZeroDenominatorSavings`, and the empty-repository test cover the
statistics acceptance battery. FR-201 maps to
`SnapshotPagesAreNewestFirstWithStableIdTieBreakAndMetadata`; FR-202 to
`ChildrenArePagedDirectDescendantsAndExcludeRootSentinel`; FR-203 to
`SearchIsLiteralCaseSensitiveAndPagedAcrossRelativePaths`.

The fixture intentionally omits object files: query results rely only on metadata.
`ReadOnlyQueriesWorkWhileWriterLockIsHeldWithoutReadingObjects` exercises the actual
repository writer-lock path. Two aggregate tests cover totals above INT64_MAX and
overflow past UINT64_MAX. The orchestrator owns execution and final test evidence.
