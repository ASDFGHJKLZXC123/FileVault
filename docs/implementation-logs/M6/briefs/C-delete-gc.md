# Packet C — snapshot deletion and garbage collection

Owner: root. Files: garbage_collector public header, implementation, and integration tests;
small lock/recovery/failure-injection integration edits are also root-owned.

## Required specification

- "Deletion requires an exclusive repository lock." Set `deleting`, commit, delete entries and
  warnings in bounded transactions, then delete the snapshot row. Retain objects until explicit GC.
- "GC order: exclusive lock → recovery → unreferenced query (§22.2) → recheck → delete object file,
  then row, in bounded batches (§22.4); orphan object files handled per §22.5."
- "gc --dry-run reports counts/bytes and performs zero writes (§22.3)."
- "GC-after-delete restore test: delete snapshot B, run GC, then fully restore snapshot A
  byte-identically." Interrupted object-then-row deletion must recover on the next GC.

## Decisions and invariants

- GarbageCollector exposes deletion and collection separately; collection defaults to preview.
- Preview acquires the existing OS lock without rewriting owner diagnostics and never runs
  recovery. Its SQL predicts the post-recovery live set from complete snapshots; execution forces
  recovery on every invocation, even when this Repository handle previously recovered.
- No reference counts. Execution rechecks actual entry-chunk relationships inside a bounded
  exclusive SQLite transaction and holds the repository lock throughout.
- Only canonical hash-derived regular object files are removed. Reject indirection in object
  ancestors or temporary cleanup trees; do not recurse through junctions/symlinks. Unknown object
  names are left untouched and counted as ignored paths.
- Failure seam after object removal and before row deletion demonstrates safe retry. Cancellation
  before each metadata batch leaves deleting snapshots resumable using M4 machinery.
- Test byte-identical preview (including database, sidecars, lock and temporary files), missing
  unreferenced objects, stale incomplete references, orphan detection, shared-chunk survival,
  corruption/path indirection rejection, lock contention, read-only rejection and cancellation.

The old orchestrator's named implementation/review models are unavailable in this session;
available inherited agents perform disjoint implementation and later independent review/verification.
