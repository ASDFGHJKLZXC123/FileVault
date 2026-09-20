# Milestone 6 — Diff, verification, delete, and GC

> Part of the LocalVault build plan ([index](../00-INDEX.md)). Spec source: guide §37.
> **Have these open — the complete reading set for this milestone:**
> [Part 04](../04-repository-format-and-database.md) (§14.5: chunks table, the no-refcount rule) ·
> [Part 05](../05-snapshot-engine.md) (§18.8: storage-savings formulas) ·
> [Part 06](../06-restore-diff-verify-gc.md) (§20–22: diff, verification, deletion, GC — the core of this milestone) ·
> [Part 07](../07-consistency-concurrency-platform.md) (§23.4: recovery-before-GC · §23.6: locking) ·
> [Part 09](../09-quality-testing-ci-packaging.md) (§32.1: corruption helpers · §32.3: verification/GC test lists) ·
> [Part 11](../11-appendix-skeletons.md) (§42.9: statistics SQL).
> Nothing else is required; no other milestone file is ever needed.

## Role in the overall project

M6 adds the trust-and-maintenance toolset: compare snapshots (§20), prove repository health (§21), delete snapshots, and reclaim space (§22). After M6 the storage engine is functionally complete — M7–M9 only wrap it in interfaces and polish.

## Why it matters

- **GC is the only code in the project that deletes user data.** Deduplication means chunks are shared across snapshots; a GC bug that removes a still-referenced chunk silently destroys *older* backups — the worst possible failure for a backup tool. The acceptance line "GC never removes shared chunks" is the most important single test in the suite.
- Verification is the counterpart promise: if corruption happens (disk rot, bad RAM), LocalVault *detects* it rather than restoring garbage — quick mode for routine checks, full mode reading every referenced object.
- Diff is what makes snapshots browsable history instead of opaque blobs, and its streaming sorted-merge design (§20.3) is the pattern that keeps million-entry snapshots out of memory.

## How it works

1. Diff: two SQL cursors ordered by `relative_path`, merge-compare, classify per §20.2 (symlink target change = `content_modified`), stream results.
2. Quick verify: SQLite `integrity_check` + FK check + counters + object existence/size/path-derivation checks (§21.1). Full verify: additionally decompress and re-hash every distinct referenced chunk (§21.2).
3. Delete: `status='deleting'` → batched entry deletion → row removal (§22.1, built on M4's resumable machinery).
4. GC: under the exclusive lock, after recovery — find unreferenced chunks (the §22.2 LEFT JOIN), delete object-then-row in batches; orphan object files handled per §22.5; `--dry-run` reports without touching anything.

## Specification (verbatim from §37)

Implement:

- Streaming snapshot diff.
- Quick verification.
- Full object verification.
- Snapshot deletion.
- GC dry run.
- GC execution.
- Repository statistics.

Acceptance:

- Corruption tests detect missing and modified objects.
- GC never removes shared chunks.
- Dry run makes no changes.

## Likely problems and confusions — with answers

1. **"GC deleted a chunk an old snapshot needed."** The classic cause: computing "unreferenced" while stale incomplete snapshot metadata still exists, or from a cached set while deletion proceeds. The spec's ordering is mandatory: exclusive lock → recovery first → then the LEFT JOIN → recheck within the lock (§22.2, §22.4). The regression test: snapshot A and B sharing chunks, delete B, GC, then **fully restore A and byte-compare**.
2. **"Should I keep a reference count on chunks instead of the JOIN?"** No — explicitly forbidden (§14.5). Counts drift after crashes; the join derives truth from actual references. If GC is slow, add an index, not a counter.
3. **"Verification reports orphan objects as corruption."** Wrong severity: extra unreferenced objects are *GC candidates*, not corruption; missing/corrupt **referenced** objects are fatal (§21.3). Get the exit-code mapping right (5 vs. clean-with-notes).
4. **"How do I make corrupt fixtures?"** The §32.1 helpers: `corrupt_byte(path, offset)` on an object file (full verify must flag it), `truncate_file` (size check catches it in quick mode), delete an object file (missing), and hand-edit a DB row's `raw_size` for the mismatch case (§32.3 verification list).
5. **"Diff drowns in directory mtime changes."** By design directory mtimes churn; implement the §20.2 suppression option now, or every diff of a real tree is noise.
6. **"Crash mid-GC leaves a chunks row pointing at a deleted object — verify now fails!"** Check who references it: nobody (that's why GC chose it), so it's *stale metadata*, not corruption — the next GC removes the row (§22.4). Encode this exact scenario as the "interrupted GC is recoverable" test rather than "fixing" the ordering.
7. **"Statistics savings come out negative/over 100%."** Zero-denominator and overflow rules are §18.8 + §42.5 (`checked_to_u64`, zero → 0.0). The §32.2 statistics cases cover empty repo, shared chunks, incompressible data.
8. **Mac-primary note:** everything here is portable — full local development, no VM required. Run full verification against a repository you created back in the M1 VM check for a free cross-platform portability data point.

## Completion checklist

M6 is complete only when **every** box is checked. Copy this checklist into the verification log and check items there with evidence.

**Implementation**

- [ ] `DiffEngine`: two sorted SQL cursors, streaming merge, classification per §20.2 (symlink target change = `content_modified`), directory-mtime-noise suppression option, root row excluded.
- [ ] Quick verification performs all eleven §21.1 checks (integrity_check, FK check, `repository_info` row, format/algorithms, counters, chunk rows, object existence, object sizes, derived-path match, stale non-complete snapshots, stale temp files).
- [ ] Full verification decompresses and re-hashes every distinct referenced chunk; reports all issues rather than stopping at the first (§21.2). Decision recorded on the optional `--files` reconstruction mode.
- [ ] Issue severity per §21.3: missing/corrupt **referenced** objects make the core verification result fail; orphans are nonfatal GC candidates. M7 maps this result to command exit code 5 (zero for clean-with-notes).
- [ ] Snapshot delete: refuses a pending-in-use snapshot, uses the M4 `deleting` machinery, keeps objects unless `--gc` (§22.1).
- [ ] GC order: exclusive lock → recovery → unreferenced query (§22.2) → recheck → delete object file, then row, in bounded batches (§22.4); orphan object files handled per §22.5.
- [ ] `gc --dry-run` reports counts/bytes and performs zero writes (§22.3).
- [ ] Statistics per §18.8 formulas with the §42.9 queries and checked arithmetic (§42.5); zero denominators return 0.0.

**Tests (all green)**

- [ ] Diff battery (§32.2): added, removed, type changed, content modified, metadata modified, unchanged, empty snapshots, very long path, streaming merge order.
- [ ] Verification corruption matrix (§32.3): healthy quick, healthy full, missing referenced object, truncated object, modified compressed bytes, wrong `raw_size`, invalid object path in DB, FK inconsistency in a malformed test DB — each detected and classified correctly (acceptance).
- [ ] Delete one of two snapshots sharing chunks: shared objects retained.
- [ ] **GC-after-delete restore test: delete snapshot B, run GC, then fully restore snapshot A byte-identically** — the single most important test in the project (acceptance: GC never removes shared chunks).
- [ ] Dry run: repository directory tree and DB are byte-identical before/after (acceptance).
- [ ] GC removes unreferenced objects and their rows; orphan object file detected; stale temp removed.
- [ ] Interrupted GC (injected failure between object delete and row delete) is recoverable by the next GC.
- [ ] Statistics: empty repository, single snapshot, duplicate chunks, shared chunks across snapshots, zero denominators.

**Platform & CI**

- [ ] Active native development host suite green; Linux, macOS, Windows, Linux ASan/UBSan, and Linux TSan CI jobs green. Record the actual local host; do not claim an unavailable Mac-local run. No VM session required (optionally verify a VM-created repository for a free portability point).

**Process**

- [ ] Implementation + verification logs under `docs/implementation-logs/M6/`, including this checklist's state.
- [ ] Log records FR-201–FR-204 and FR-400–FR-408 with proving test names.
