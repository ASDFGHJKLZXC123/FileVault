# LocalVault Build Plan — Part 05: Snapshot Engine

> Sections 16–18 of the Technical Implementation Guide (Revision 2), split verbatim.
> Section numbers are unchanged, so cross-references like “Section 25.10”
> resolve via the lookup table in [00-INDEX.md](00-INDEX.md).

---

# 16. Snapshot algorithm

## 16.1 High-level sequence

```text
Validate source and repository
        │
Acquire exclusive repository lock
        │
Recover stale incomplete operations
        │
Insert pending snapshot row
        │
Scan source and enqueue jobs
        │
Workers read/hash/compress/store chunks
        │
Single metadata writer inserts entries and chunk mappings
        │
Validate counters and referenced objects
        │
Mark snapshot complete
        │
Release repository lock
```

## 16.2 Detailed steps

1. Validate that the source exists and is a directory.
2. Reject a source located inside the repository.
3. Reject a repository located inside the source unless the repository path is automatically ignored. Prefer rejecting this configuration to avoid recursive backup.
4. Acquire `RepositoryLock` exclusively.
5. Run stale-operation recovery.
6. Insert a `pending` snapshot row and commit it so a crash is detectable.
7. Load repository configuration and ignore rules.
8. Start:
   - One scanner producer.
   - A bounded file-job queue.
   - A fixed worker pool.
   - One result/metadata writer.
9. Insert directory and symlink metadata.
10. For each regular file:
    - Capture pre-read metadata: size, modification time, change time, mode or attributes, and file identifier (device/inode on POSIX; volume serial/file ID on Windows).
    - Decide whether an earlier entry can be reused.
    - Otherwise stream the file in 4 MiB chunks.
    - Feed every byte to the full-file BLAKE3 hasher.
    - For each chunk:
      - Compute chunk hash.
      - Check the in-memory recent-object cache.
      - Check the database/object store when not cached.
      - Compress and atomically store only if absent.
      - Produce an ordered `ChunkReference`.
    - Capture post-read size and modification time.
    - If changed, discard the produced entry result and retry once when enabled.
    - If still unstable, record a warning and skip it.
11. The metadata writer:
    - Inserts or verifies each `chunks` row.
    - Inserts the `entries` row.
    - Inserts all `entry_chunks` rows.
    - Commits in bounded batches, for example every 500 entries or 64 MiB of metadata input.
12. If cancellation is requested:
    - Stop accepting new file jobs.
    - Allow in-flight object writes to finish safely.
    - Drain or discard uncommitted results.
    - Mark the snapshot `cancelled`.
    - Delete its entries and warnings or retain them only for diagnostics. Recommended: delete them transactionally.
    - Leave orphan objects for later garbage collection.
13. If a fatal error occurs:
    - Mark the snapshot `failed` with a concise failure message.
    - Remove its entry metadata transactionally.
    - Leave completed immutable objects unreferenced.
14. On success:
    - Verify all entry chunk references exist.
    - Calculate final counters.
    - Update the snapshot row to `complete` inside one transaction.
15. Flush final database state.
16. Emit a final progress event.
17. Release the repository lock.

## 16.3 Batch metadata transaction

Do not keep one enormous transaction open for a multi-hour snapshot. Use batches while the snapshot remains `pending`.

Visibility rule:

```sql
SELECT ... FROM snapshots WHERE status = 'complete';
```

Recovery can delete all entries belonging to stale non-complete snapshots.

## 16.4 Stable file check

For each regular file:

```text
before = {size, mtime_ns, ctime_ns, file_id}
read and process file
after  = {size, mtime_ns, ctime_ns, file_id}
```

`file_id` is device/inode on POSIX and volume serial/file ID on Windows; `ctime_ns` is POSIX ctime or the NTFS change time.

Treat the file as unstable when:

- Size changed.
- Modification time changed.
- Change time changed.
- File identifier changed.
- The read ended earlier than expected.
- The file disappeared.

Retry once by default. Do not silently store a mixture of versions.

## 16.5 Snapshot warnings

Warnings are non-fatal per-entry outcomes:

- Permission denied.
- File locked by another process (Windows sharing violation).
- Cloud placeholder file skipped without hydration.
- Directory junction recorded as a link; volume mount point skipped.
- File disappeared.
- File changed repeatedly.
- Unsupported file type.
- Path not convertible to valid UTF-8 (including unpaired UTF-16 surrogates on Windows).
- Symlink target could not be read.

The CLI and GUI must show the count and allow viewing details. A snapshot with warnings may still be `complete`, but the operation result must be marked partial.

---

# 17. Incremental snapshot optimization

Correctness must work without this optimization. Add it after basic snapshot/restore tests pass.

## 17.1 Fast reuse rule

Find the newest complete snapshot for the same source root. For an entry at the same relative path, reuse its existing chunk list when all of these match:

- Entry type is regular file.
- Logical size.
- Modification time at nanosecond precision when available.
- Change time (POSIX ctime; NTFS change time) when available — this catches mtime-preserving modifications such as `touch -r` or checkout tools.
- File identifier (device/inode or volume serial/file ID) where meaningful.
- Platform metadata (POSIX mode or Windows attributes) if metadata-only changes should be detected.

When reused:

- Insert a new `entries` row for the new snapshot.
- Copy the old `entry_chunks` rows.
- Reuse the previous full-file hash.
- Do not read or hash file contents.

## 17.2 Safety modes

- Default fast mode: metadata-based reuse.
- `--force-rehash`: read and hash every regular file.
- Future optional paranoid mode: reuse only after a secondary lightweight fingerprint.

Document that metadata-based reuse assumes the filesystem maintains modification and change times correctly, and that the false-negative window is exactly: a file modified without changing size, mtime, ctime, or file identifier.

## 17.3 SQL copy pattern

After inserting the new entry:

```sql
INSERT INTO entry_chunks (
    entry_id,
    sequence_number,
    chunk_hash,
    raw_offset,
    raw_length
)
SELECT
    :new_entry_id,
    sequence_number,
    chunk_hash,
    raw_offset,
    raw_length
FROM entry_chunks
WHERE entry_id = :previous_entry_id
ORDER BY sequence_number;
```

---

# 18. Chunking, hashing, compression, and deduplication

## 18.1 Fixed-size chunking

Default raw chunk size:

```text
4 MiB = 4 * 1024 * 1024 bytes
```

Rules:

- Read up to 4 MiB at a time.
- The last chunk may be shorter.
- Empty files have zero chunks.
- Chunk size is stored in repository metadata and cannot be changed for an existing repository without a format migration.
- Use binary file I/O.

## 18.2 Hashing

Hash raw, uncompressed bytes:

```cpp
chunk_hash = BLAKE3(raw_chunk)
```

Hash the full file concurrently as the stream is read:

```cpp
full_file_hasher.update(raw_chunk);
```

Do not calculate a file hash by concatenating textual chunk hashes; hash the actual file bytes.

## 18.3 Compression

Use zstd level `3` by default.

Per chunk:

1. Calculate `ZSTD_compressBound(raw_size)`.
2. Allocate a destination buffer no larger than that bound.
3. Compress.
4. Check `ZSTD_isError`.
5. Resize the compressed buffer to the returned byte count.
6. Write the compressed bytes to a temporary file.
7. Record raw and compressed sizes.

Decompression:

1. Retrieve expected raw size from SQLite.
2. Reject sizes greater than configured safety limits.
3. Allocate exactly the expected chunk size.
4. Decompress.
5. Require the returned size to equal expected raw size.
6. Recompute BLAKE3 and compare with the object identifier.

## 18.4 Deduplication lookup

Use a two-level lookup:

1. A thread-safe in-memory cache of known hashes for the current process.
2. SQLite/object existence check on cache miss.

The database is authoritative for known objects, but verification also checks the file exists.

## 18.5 Duplicate-write race

Two worker threads may discover the same new hash simultaneously. Prevent duplicate publication with a striped mutex table:

```cpp
std::array<std::mutex, 256> object_mutexes;
auto& mutex = object_mutexes[first_hash_byte];
```

Inside the selected mutex:

1. Recheck whether the object already exists.
2. If absent, write and publish it.
3. Insert or verify the database `chunks` row through the metadata writer.

The repository-level writer lock prevents a separate process from writing concurrently. The striped mutex handles worker threads inside one process.

## 18.6 Object write algorithm

```text
derive final object path
lock stripe for hash
recheck final object and database record
create unique temp file under temporary/objects
write compressed bytes
flush userspace stream
fsync temp file
atomically rename temp file to final object path
fsync final object's parent directory
unlock stripe
```

The temporary directory must be on the same filesystem as `objects/` so rename remains atomic. On POSIX platforms publication uses `rename(2)`; on Windows it uses `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING`, which is atomic for same-volume moves on NTFS. Windows cannot fsync a directory; see Section 23.2 for the per-platform durability statement.

If the final object appears before publication because another worker stored it, delete the temporary file and reuse the existing object.

## 18.7 Hash collision policy

A BLAKE3 collision is not expected, but code must not blindly accept conflicting metadata.

When a hash already exists:

- Require the stored `raw_size` to match.
- During full verification, require decompressed bytes to hash to the same digest.
- If an existing row has a conflicting raw size, report repository corruption and stop the operation.

## 18.8 Storage-savings formulas

For a selected snapshot or repository scope:

```text
logical_bytes     = sum of logical file sizes
unique_raw_bytes  = sum raw sizes of distinct referenced chunks
stored_bytes      = sum compressed sizes of distinct referenced chunks

deduplication_savings = 1 - unique_raw_bytes / logical_bytes
compression_savings   = 1 - stored_bytes / unique_raw_bytes
total_savings         = 1 - stored_bytes / logical_bytes
```

When a denominator is zero, return `0.0`.

---
