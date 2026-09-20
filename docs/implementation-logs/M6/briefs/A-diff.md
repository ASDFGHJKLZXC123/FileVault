# M6 packet A — streaming snapshot diff

## Specification excerpts

From `build-plan/06-restore-diff-verify-gc.md`:

- §20.1: "Compare entries by normalized `relative_path`."
- §20.2: "Present only in newer snapshot: `added`." "Present only in older snapshot: `removed`." "Different entry type: `type_changed`."
- §20.2: "Both regular files and `file_hash` differs, or both symbolic links and `symlink_target` differs: `content_modified`."
- §20.2: "Same content (file hash or link target) but mode, attributes, or mtime differs: `metadata_modified`." "Equivalent content and selected metadata: `unchanged`."
- §20.2: "For directories, compare mode and modification time only when metadata comparison is enabled. Directory modification times often change because children changed; provide an option to suppress noisy directory metadata changes."
- §20.3: "Use two sorted SQL cursors by `relative_path` and perform a merge comparison. Do not load millions of entries into memory."
- §20.4: "Support streaming results through a callback and paged results for the GUI."
- M6 checklist: "directory-mtime-noise suppression option, root row excluded."

## Decisions before implementation

- Both snapshots must be complete. One deferred read transaction includes validation and both cursors, so concurrent writers cannot produce a mixed view. Diff reads only metadata, accepts a read-only repository, and takes no OS writer lock.
- `DiffEntry` contains the specified path, classification, and optional before/after entries. The public callback receives each result immediately; its exceptions propagate. Stop requests throw `LocalVaultError(cancelled)`.
- Metadata comparison and unchanged rows default on. Directory-mtime suppression defaults on; callers can opt into directory mtime comparison. Directory mode remains significant, while directory attributes are excluded per §20.2.
- Pagination uses an exclusive UTF-8 `relative_path` keyset cursor and a positive page size. A page holds at most the requested results; one result of lookahead establishes whether another page exists. Each call revalidates snapshot completeness in its own transaction.
- Separate page calls do not share a transaction. Complete snapshots are immutable in normal operation; deletion between pages makes the next call fail its completeness check, rather than presenting a partial snapshot. Use the same snapshot IDs/options with each returned cursor.
- SQL keys are compared in their stored UTF-8 byte order, matching SQLite BINARY ordering on every platform. Link content compares saved target text, not normalized path equivalence. Snapshot creation already supplies normalized relative paths.
- Tests construct metadata fixtures directly to cover long paths, link text, and metadata deterministically without platform-specific filesystem privileges. Actual snapshot-engine interoperability is also covered.
- No schema, CLI, GUI, or shared-file edits belong to this packet. The root agent integrates source/test CMake entries and runs the consolidated build.

## Validation

Implemented in `include/localvault/diff_engine.hpp` and `src/core/diff_engine.cpp`.
The root agent owns consolidated CMake integration and build/test execution; no independent concurrent build was launched for this packet.

FR-204 is covered by `DiffEngineTest`:

- `ClassifiesAllChangesInSortedStreamingOrder`
- `SuppressesDirectoryMtimeNoiseButRetainsModeChanges`
- `ComparesLinkTextExactlyAndClassifiesLinkMetadata`
- `EmptySnapshotsAndRootSentinelsProduceNoResults`
- `LongAndUnicodePathsFollowSqlByteOrder`
- `PagesByExclusiveKeyAndFiltersUnchangedRows`
- `ReadOnlyDiffDoesNotAcquireWriterLock`
- `RejectsMissingAndEveryIncompleteSnapshotInEitherPosition`
- `CancellationBeforeAndDuringStreamingRollsBackReadTransaction`
- `CallbackFailurePropagatesAndConnectionRemainsUsable`
- `StreamingUsesOneConsistentViewAcrossConcurrentDeletion`
- `ComparesSnapshotsProducedBySnapshotEngine`

Source review caught the existing `Statement::bind` null-data behavior for a default-constructed empty `string_view`: the cursor explicitly binds non-null empty text so an omitted page cursor cannot become SQL NULL.
Execution results are pending the consolidated build and are recorded in the M6 implementation/verification logs.
