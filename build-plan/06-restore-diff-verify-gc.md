# LocalVault Build Plan — Part 06: Restore, Diff, Verification, and GC

> Sections 19–22 of the Technical Implementation Guide (Revision 2), split verbatim.
> Section numbers are unchanged, so cross-references like “Section 25.10”
> resolve via the lookup table in [00-INDEX.md](00-INDEX.md).

---

# 19. Restore algorithm

## 19.1 Request validation

Before writing anything:

1. Acquire the exclusive repository lock for the duration of the restore (the v1 locking rule, Section 23.6): restores read object files and must not race garbage collection.
2. Require the requested snapshot to exist and have status `complete`.
3. Normalize every requested relative path.
4. Reject:
   - Absolute paths.
   - Paths containing a `..` component after lexical normalization.
   - Empty selections unless the request explicitly means the complete snapshot.
   - Paths not present in the snapshot.
5. Resolve the destination root to an absolute path.
6. Reject a destination root that equals the repository root, lies inside the repository, or contains the repository — restoring into `objects/` or over `repository.db` must be impossible.
7. Apply overwrite policy.
8. Calculate a restore plan before writing. On case-insensitive or normalization-insensitive destinations, detect entry-name collisions in the plan and mark losers as skipped (Section 25.12).

## 19.2 Restore order

Restore in this order:

1. Create directories from shallowest to deepest.
2. Restore regular files.
3. Recreate symbolic links.
4. Apply directory timestamps and modes from deepest to shallowest.

Applying directory metadata last prevents child creation from changing final directory timestamps.

## 19.3 Regular-file reconstruction

For each regular file:

1. Validate the output path with `PathSafety`.
2. Create parent directories.
3. Create a unique temporary file in the same destination directory:
   ```text
   .<filename>.localvault-<random>.tmp
   ```
4. Initialize a full-file BLAKE3 hasher.
5. Query ordered `entry_chunks`.
6. For each chunk:
   - Check cancellation.
   - Open the object file.
   - Read compressed bytes with an upper size bound.
   - Decompress to the expected raw size.
   - Hash raw bytes and compare with `chunk_hash`.
   - Append raw bytes to the temporary output.
   - Feed raw bytes to the full-file hasher.
7. Flush and fsync the temporary output.
8. Require written bytes to equal the entry's logical size.
9. Compare the final file hash with `entries.file_hash`.
10. Apply file mode and modification time to the temporary file where possible.
11. Atomically publish it according to overwrite policy. For `never`, use an atomic no-replace rename — `renameat2(..., RENAME_NOREPLACE)` on Linux, `renamex_np(..., RENAME_EXCL)` on macOS, `MoveFileExW` without `MOVEFILE_REPLACE_EXISTING` on Windows — never a check-then-rename sequence, which races concurrent writers. For `always`, use a replacing rename (`rename(2)` / `MOVEFILE_REPLACE_EXISTING`).
12. Fsync the destination parent directory.
13. On any failure, remove the temporary output and leave the previous destination unchanged.

## 19.4 Symbolic-link restore

- Store and recreate the link text, not the target contents.
- Never follow the saved target during restore.
- Apply overwrite rules before creating the link.
- Reject a symbolic-link destination path if an ancestor is an existing symbolic link unless a hardened path-safe implementation proves containment.
- Display a warning when a saved absolute symlink target is restored; the link may point outside the restore tree by design, but LocalVault itself must not follow it.
- On Windows, creating symbolic links requires Developer Mode or `SeCreateSymbolicLinkPrivilege`. When creation fails for that reason, skip the link with a warning and continue (partial success), and say so in the result summary. Use `SYMBOLIC_LINK_FLAG_DIRECTORY` for links whose stored target was a directory, and `SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE` when available.

## 19.5 Conflict policies

### `never`

- Skip or fail every path that already exists.
- The CLI should return partial-success status when some selected paths were skipped.

### `prompt`

- The core emits a conflict request through an interface callback.
- The CLI asks on the terminal.
- The GUI displays a dialog.
- Allow "apply to all" decisions.

### `always`

- Replace regular files and links through safe temporary publication.
- Replacing a directory with a non-directory, or the reverse, requires explicit handling and should fail by default in the first release.

## 19.6 Restore result verification

A successful file restore requires:

- Every object existed.
- Every object decompressed to its expected raw size.
- Every raw chunk matched its BLAKE3 hash.
- Total reconstructed bytes matched `logical_size`.
- Full reconstructed file matched `file_hash`.
- Atomic publication succeeded.

---

# 20. Snapshot diff

## 20.1 Comparison key

Compare entries by normalized `relative_path`.

## 20.2 Classification

For each path in the union of both snapshots:

- Present only in newer snapshot: `added`.
- Present only in older snapshot: `removed`.
- Different entry type: `type_changed`.
- Both regular files and `file_hash` differs, or both symbolic links and `symlink_target` differs: `content_modified`.
- Same content (file hash or link target) but mode, attributes, or mtime differs: `metadata_modified`.
- Equivalent content and selected metadata: `unchanged`.

For directories, compare mode and modification time only when metadata comparison is enabled. Directory modification times often change because children changed; provide an option to suppress noisy directory metadata changes.

## 20.3 Efficient implementation

Use two sorted SQL cursors by `relative_path` and perform a merge comparison. Do not load millions of entries into memory.

Conceptually:

```sql
SELECT relative_path, entry_type, logical_size, modified_time_ns,
       posix_mode, file_hash, symlink_target
FROM entries
WHERE snapshot_id = ?
ORDER BY relative_path;
```

## 20.4 Diff output type

```cpp
enum class DiffKind {
    added,
    removed,
    type_changed,
    content_modified,
    metadata_modified,
    unchanged
};

struct DiffEntry {
    std::filesystem::path relative_path;
    DiffKind kind{};
    std::optional<EntryInfo> before;
    std::optional<EntryInfo> after;
};
```

Support streaming results through a callback and paged results for the GUI.

---

# 21. Integrity verification

## 21.1 Quick verification

Quick verification checks metadata and object presence without reading every object body:

1. `PRAGMA integrity_check`.
2. Foreign-key check:
   ```sql
   PRAGMA foreign_key_check;
   ```
3. Validate one `repository_info` row exists.
4. Validate repository format and algorithms.
5. Ensure complete snapshots have valid counters.
6. Ensure every `entry_chunks.chunk_hash` has a `chunks` row.
7. Ensure every referenced chunk object file exists.
8. Ensure object file sizes match `compressed_size`.
9. Ensure derived object paths match stored object paths.
10. Detect stale pending, failed, and unfinished `deleting` snapshots.
11. Detect stale temporary files.

## 21.2 Full verification

Full verification includes quick verification and, for every distinct referenced chunk:

1. Read the object.
2. Decompress to exactly `raw_size`.
3. Compute BLAKE3.
4. Compare against `chunks.hash`.
5. Report corruption without stopping at the first issue unless continuing is unsafe.

Optionally verify full file hashes by reconstructing streams from chunks without writing destination files. This is expensive and may be exposed as:

```bash
localvault verify --full --files
```

## 21.3 Verification issue handling

Verification does not automatically delete corrupt data.

- Missing/corrupt referenced objects are fatal repository health problems.
- Missing unreferenced objects are stale metadata and can be removed by GC.
- Extra object files not represented in SQLite are orphan candidates, not corruption of complete snapshots.
- The command returns a nonzero exit code when any required object or relationship is invalid.

## 21.4 Repair policy

The first release should not claim automatic repair. Safe repair actions may include:

- Remove stale temporary files.
- Delete metadata for stale incomplete snapshots.
- Register or delete verified orphan objects only through a deliberate maintenance command.

Never fabricate missing backup content.

---

# 22. Snapshot deletion and garbage collection

## 22.1 Delete snapshot

Deletion requires an exclusive repository lock. Do not delete millions of entry rows in one transaction — the same principle as Section 16.3.

1. Validate the snapshot exists.
2. Prevent deletion of a `pending` snapshot being processed by the current operation.
3. Set the snapshot `status` to `deleting` and commit; the snapshot is now invisible to browsing and restore, and recovery treats it as a resumable deletion.
4. Delete its `entries` rows in bounded batches (for example 10,000 entries per transaction); `ON DELETE CASCADE` removes the matching `entry_chunks` rows and warnings with each batch.
5. Delete the snapshot row in a final transaction.
6. Do not delete objects in the same command unless `--gc` is explicitly requested.

A crash mid-deletion leaves a `deleting` snapshot; startup recovery (Section 23.4) finishes the batched deletion. This keeps transactions bounded and object cleanup independently recoverable.

## 22.2 Identify unreferenced chunks

```sql
SELECT c.hash, c.object_path, c.compressed_size
FROM chunks AS c
LEFT JOIN entry_chunks AS ec ON ec.chunk_hash = c.hash
WHERE ec.chunk_hash IS NULL
ORDER BY c.hash;
```

Because normal browsing and restore only use complete snapshots, recovery must remove entry metadata for stale incomplete snapshots before GC.

## 22.3 Dry run

`gc --dry-run` reports:

- Number of unreferenced chunk rows.
- Number of orphan object files.
- Bytes reclaimable.
- Stale temporary files.
- Stale incomplete snapshot metadata.

It performs no deletions.

## 22.4 Deletion order

For each unreferenced chunk:

1. Confirm it is still unreferenced under the exclusive lock.
2. Delete the object file.
3. Delete the `chunks` row.
4. Commit in bounded batches.

A crash after object deletion but before row deletion leaves an unreferenced row pointing to a missing object. The next GC can remove it safely because no snapshot references it.

## 22.5 Orphan object files

Walk `objects/` and find files not represented in `chunks`.

Before deleting one:

- Verify its filename has the expected hash format.
- Ensure it is not a temporary file.
- Optionally decompress and hash it for diagnostics.
- Delete only under the exclusive repository lock.

---
