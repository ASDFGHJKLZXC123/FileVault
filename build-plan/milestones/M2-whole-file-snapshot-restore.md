# Milestone 2 — Minimal whole-file snapshot/restore

> Part of the LocalVault build plan ([index](../00-INDEX.md)). Spec source: guide §37.
> **Have these open — the complete reading set for this milestone:**
> [Part 03](../03-architecture-and-public-api.md) (§11.3: component responsibilities — keep these shapes) ·
> [Part 04](../04-repository-format-and-database.md) (§14.4: entries and the root-row rule) ·
> [Part 05](../05-snapshot-engine.md) (§16: snapshot sequence, simplified) ·
> [Part 06](../06-restore-diff-verify-gc.md) (§19: restore order and file reconstruction, simplified) ·
> [Part 07](../07-consistency-concurrency-platform.md) (§25.1–25.6: paths, metadata, symlinks, special files) ·
> [Part 09](../09-quality-testing-ci-packaging.md) (§32.1: test support utilities · §32.3: fixture list) ·
> [Part 11](../11-appendix-skeletons.md) (§42.8: file reconstruction pseudocode, incl. the empty-file case).
> Nothing else is required; no other milestone file is ever needed.

## Role in the overall project

M2 is the walking skeleton: the first time bytes travel the full path — scan → store → metadata → restore → byte-identical tree. It deliberately uses the dumbest possible storage (single-threaded, one object per small file, no dedup races, no crash-safety yet) so that the *shape* of the system is proven while every bug is still trivially observable.

## Why it matters

- If restored bytes don't match the source **now**, the cause is in ~500 lines of sequential code. The same symptom after M3–M5 could be chunk ordering, a dedup race, or a worker bug — 10× the debugging surface. This milestone exists to make the scariest class of bug (silent data corruption) cheap to find.
- It forces the schema from M1 into real use for the first time: entries, the root-row rule, path normalization. Schema friction surfaces here while changing it is still easy.
- The test infrastructure built here (`TemporaryDirectory`, `DatasetBuilder`, tree comparison — §32.1) is what every later milestone's tests stand on.

## How it works

1. `FileScanner` walks the source with `symlink_status`, emitting normalized relative paths (root = `''` per §14.4).
2. For each regular file: stream it, hash it (whole file), store it as one object, insert the entry row and a single chunk mapping. Directories and symlinks get entry rows only.
3. Restore: create directories shallow→deep, rebuild files through temp-file-then-rename, recreate symlinks, apply mtime/mode deepest-last (§19.2).
4. The acceptance loop: build a fixture tree, snapshot it, delete the original, restore, and compare trees byte-for-byte plus mtime/mode.

Keep it single-threaded and skip compression if you like (store raw bytes) — but keep the *interfaces* (Chunker, ObjectStore) shaped as §11.3 defines them, so M3 swaps implementations, not architecture.

## Specification (verbatim from §37)

Before chunking, prove the end-to-end path with one object per file or one chunk per small test file:

- Scan directories.
- Store entries.
- Store file content.
- List snapshots.
- Restore complete tree.
- Compare bytes.

Acceptance:

```text
source → snapshot → delete copy → restore → tree comparison passes
```

This milestone reduces risk before concurrency and deduplication are added.

## Likely problems and confusions — with answers

1. **"Why not skip straight to chunking? This feels like throwaway work."** Only the object-store *internals* get replaced in M3; the scanner, entry insertion, restore path, and all the tests survive unchanged. The guide's ordering is the risk-management strategy — resist merging M2 and M3.
2. **"Empty files break my restore hash check."** An empty file has an entries row, `logical_size = 0`, **zero** chunk rows, and `file_hash` = BLAKE3 of empty input (§14.6, §42.8). Your hasher must finalize without any `update()` call. Add the empty-file fixture on day one.
3. **"My root directory shows up as its own child."** You forgot the `relative_path <> ''` exclusion in the children query (§14.4). This is the most predictable bug in the milestone — the test for it is spelled out in the schema section.
4. **"Restored mtimes are wrong for directories."** You applied directory metadata before writing children; the children's creation updated the parent mtime. Apply directory timestamps deepest-first, after all content (§19.2, §25.3).
5. **"mtime comparison fails by sub-second amounts."** Store and restore nanoseconds (`modified_time_ns`); on macOS APFS gives real ns via `st_mtimespec`. If you round-trip through `time_t` anywhere, you'll chase phantom diffs.
6. **"Symlink to a missing target errors my restore."** Correct behavior is to recreate the link text without caring whether the target exists (§19.4). Never `stat` the target.
7. **"Unicode fixture names look different after restore on the Mac."** That's NFD normalization display, not corruption — byte-compare the stored path strings, not renormalized ones (§25.1). Keep one NFD and one NFC fixture in the dataset to lock the policy in.
8. **Mac-primary note:** this milestone is nearly all portable `std::filesystem` code — develop and fully test on the Mac. Let CI cover Linux/Windows; no VM session needed unless CI shows a path-behavior difference.

## Completion checklist

M2 is complete only when **every** box is checked. Copy this checklist into the verification log and check items there with evidence.

**Implementation**

- [ ] `FileScanner`: uses `symlink_status`, emits normalized `/`-separated relative paths, emits the root row as `relative_path = ''` / `parent_path = ''` / `name = ''` (§14.4), skips special files with warnings.
- [ ] Entries inserted for regular files, directories, and symlinks with `logical_size`, `modified_time_ns` (nanoseconds), `posix_mode`; symlink target text stored. (`file_hash` may stay NULL until M3 introduces BLAKE3 — record this explicitly in the log so it isn't mistaken for a bug.)
- [ ] File content stored via the minimal object store (one object per file) using write-temp-then-rename.
- [ ] Snapshot row is written and marked `complete` only after all entries are committed; `list` shows complete snapshots only.
- [ ] Restore order per §19.2: directories shallow→deep, then files (temp file + rename), then symlinks, then directory mtime/mode deepest-first.
- [ ] Restore works to an alternate destination root (FR-303 basic form).
- [ ] Test support built and reusable: `TemporaryDirectory`, `DatasetBuilder`, `expect_tree_equal` with a metadata policy (§32.1).

**Tests (all green)**

- [ ] Acceptance loop: `source → snapshot → delete source copy → restore → expect_tree_equal` passes byte-for-byte plus mtime/mode policy.
- [ ] Fixture tree includes every §32.3 shape: nested dirs, an empty dir, an empty file, small text files, a file with spaces, a Unicode name (one NFC and one NFD), a hidden file, a symlink.
- [ ] Empty file round-trips: entry row exists, `logical_size = 0`, no content object, restored as an empty file.
- [ ] Root-row rule: listing children of `''` returns top-level entries and never the root itself.
- [ ] Symlink round-trips as a link (target text identical), including a link whose target does not exist; target contents never read.
- [ ] Directory mtimes survive restore (proves deepest-first metadata ordering).
- [ ] Restore into a non-empty destination with pre-existing unrelated files leaves those files untouched.

**Platform & CI**

- [ ] Mac local suite green; all three CI jobs green. No VM session required for this milestone.

**Process**

- [ ] Implementation + verification logs under `docs/implementation-logs/M2/`, including this checklist's state.
- [ ] Log records FR-100–FR-104 (FR-103 partial: symbolic-link handling here, with Windows junction/mount-point scanner rules completed in M5; FR-104 partial: POSIX metadata only until Windows attributes are exercised), FR-200, FR-300–FR-303 (basic forms), FR-309 (partial) — with proving test names.
