# M6 critical invariant review — delete and garbage collection

Date: 2026-09-20. Scope: packet C, guide §§22–23, GC/delete implementation, OS locking,
forced recovery, failure injection, and the corresponding integration tests.

The initial pass was a fresh read-only review by an available inherited agent. The old
orchestrator's named review model was unavailable. No builds or tests were run by this
review agent; execution evidence belongs in the independent verification log.

## Findings and resolution

1. **P1 — repository-root redirection after open.** Child-only no-follow checks could
   combine an old open database with deletion through a replaced root. Resolved by caching
   the canonical root when the repository opens, requiring the current root to remain a real
   directory resolving to that baseline, and checking the cached canonical ancestor chain.
   GC validates before/after locking, after the initial callback, immediately before recovery,
   and before each object removal. Existing stable aliases such as macOS `/var` remain valid.
   Regression tests: `RejectsRepositoryRootRedirectedAfterOpen` (POSIX),
   `RejectsRedirectedObjectDirectoriesWithoutTouchingTargets`, and
   `RejectsTemporaryIndirectionWithoutTouchingItsTarget`.

2. **P2 — orphan preview disagreed with recovery.** Stale entry-chunk mappings without a
   chunk row hid orphan objects from preview even though recovery would remove the mappings.
   Resolved by using predicted post-recovery reachability in preview orphan detection;
   complete-snapshot references and references with unknown parents remain protected.
   `PreviewPredictsOrphansAfterStaleReferencesAreRecovered` covers pending, failed, and
   deleting snapshots, asserting unchanged repository bytes and matching execution totals.

3. **P2 — forced recovery ignored cancellation.** Resolved by carrying the operation token
   through recovery, checking around bounded metadata commit boundaries and between
   temporary-removal calls. `CancelledForcedRecoveryStopsBetweenBatchesAndRetainsCompleteSnapshot`
   proves committed progress, rollback of the cancelled batch, retained temporary leftovers,
   successful retry, and restoration of retained snapshot content.

The review agent authored the repository cancellation helper, its regression test, and the
repository-root validation helper after reporting these findings. Those additions therefore
require inspection by the independent verifier; this follow-up review is not independent
verification of the review agent's own edits. Root implemented the GC call-site changes,
preview fix, and path/orphan regression tests. Follow-up source inspection confirms all three
reported findings are addressed; no outstanding source-review blocker was found.

## Invariants checked

- Execution holds the exclusive repository lock, forces recovery on every invocation,
  selects unreferenced chunks afterward, and rechecks actual relationships in bounded
  exclusive SQLite transactions. No reference counts are used.
- Object removal precedes chunk-row removal. Interruption or cancellation leaves only
  unreferenced missing-object rows, which a later GC can safely remove.
- Complete snapshots protect shared chunks. The acceptance test
  `DeleteThenGcPreservesSharedChunksAndRestoresOlderSnapshot` restores retained files and
  compares their bytes after deleting another snapshot and collecting its exclusive chunks.
- Preview does not run recovery, create/rewrite the lock file, or execute metadata writes.
  `PreviewLeavesEntireRepositoryAndDatabaseByteIdentical` captures the complete tree,
  including SQLite sidecars, lock contents, and temporary files.
- Orphan removal requires canonical hash/shard naming and regular files; unknown names are
  retained. Object, shard, temporary, and repository-root indirection are rejected.
- Deletion uses the existing resumable `deleting` state and bounded metadata machinery,
  retaining objects until explicit GC.

Limits: root validation is path-based, not a directory-handle/inode defense against an
uncooperative process replacing filesystem components between checks. An individual
`remove_all` subtree operation is synchronous; cancellation is checked between such calls.
These limits must not be described as stronger race resistance or per-file cancellation.

## Supplemental independent source review

A separate reviewer inspected the WAL-aware read-only view and independently reviewed the
repository cancellation and root-validation repairs above. One additional blocker was found:
closing a `unix-none` connection bypassed SQLite's deferred native-close accounting and could
release POSIX database locks held by the retained normal connection. The replacement
`locked_read_vfs` preserves native SHARED locking and native close accounting, synthesizes
only the EXCLUSIVE upgrade needed for the private WAL index, and refuses repository-file
writes, truncation, deletion, and shared-index mapping. Follow-up source inspection found this
blocker resolved. The cross-process lock regression covers normal and exception cleanup.

The repository helpers check cancellation at metadata commit boundaries and between temporary
subtree removals, retain retryable recovery state, and validate the opened root's canonical
path and ancestors within the limitations above. No remaining source-review blockers were
found. This supplemental review ran no builds or tests; runtime evidence is recorded
separately and was pending this source review.
