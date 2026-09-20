# LocalVault Build Plan — Part 04: Repository Format and Database

> Sections 13–15 of the Technical Implementation Guide (Revision 2), split verbatim.
> Section numbers are unchanged, so cross-references like “Section 25.10”
> resolve via the lookup table in [00-INDEX.md](00-INDEX.md).

---

# 13. Repository-on-disk format

A repository is a normal directory:

```text
MyVault/
├── repository.db
├── objects/
│   ├── 00/
│   ├── 01/
│   └── ...
├── temporary/
│   ├── objects/
│   └── restores/
├── logs/
└── repository.lock
```

## 13.1 Required paths

| Path | Purpose |
|---|---|
| `repository.db` | SQLite metadata |
| `objects/` | Immutable compressed chunks |
| `temporary/objects/` | Incomplete object writes |
| `temporary/restores/` | Optional internal restore staging |
| `logs/` | Diagnostic logs |
| `repository.lock` | Cross-process writer lock |

## 13.2 Object identifier

For a raw chunk:

```text
object_id = lowercase_hex(BLAKE3(raw_chunk))
```

BLAKE3 produces 32 bytes, rendered as 64 lowercase hexadecimal characters.

## 13.3 Object path

Use the first two hash characters as a shard:

```text
objects/<first-two-hex>/<full-hash>.zst
```

Example:

```text
objects/7a/7a2f...e91c.zst
```

The object path is derived from the hash. Store the relative path in SQLite for diagnostics, but verify that it matches the derived path when opening old metadata.

## 13.4 Object payload

The first release stores one valid zstd frame containing exactly one raw chunk. The database records:

- Expected raw size.
- Compressed file size.
- Hash.
- Relative object path.

No custom binary header is required in the first release. Repository format versioning allows a header to be introduced later.

## 13.5 Invariants

- Objects are immutable.
- A hash identifies uncompressed bytes, not compressed bytes.
- Two identical raw chunks share one object.
- An object is published before metadata references it.
- A complete snapshot never references a missing object under normal operation.
- Extra unreferenced objects are harmless and removable by garbage collection.
- Temporary files are never treated as valid objects.

---

# 14. SQLite schema

Use UTF-8 text, integer byte counts, and integer UTC epoch nanoseconds.

Enable these pragmas on every connection as appropriate:

```sql
PRAGMA foreign_keys = ON;
PRAGMA journal_mode = WAL;
PRAGMA synchronous = FULL;
PRAGMA busy_timeout = 5000;
```

`journal_mode` is persistent, but set and verify it when initializing the repository. Use one write connection and separate read connections only after correctness is established.

## 14.1 Migration table

```sql
CREATE TABLE IF NOT EXISTS schema_migrations (
    version      INTEGER PRIMARY KEY,
    applied_at_ns INTEGER NOT NULL,
    description  TEXT NOT NULL
);
```

## 14.2 Repository information

```sql
CREATE TABLE repository_info (
    singleton_id              INTEGER PRIMARY KEY CHECK (singleton_id = 1),
    repository_uuid           TEXT NOT NULL UNIQUE,
    format_version            INTEGER NOT NULL,
    created_at_ns             INTEGER NOT NULL,
    application_version       TEXT NOT NULL,
    chunk_size_bytes          INTEGER NOT NULL CHECK (chunk_size_bytes > 0),
    zstd_level                INTEGER NOT NULL,
    hash_algorithm            TEXT NOT NULL CHECK (hash_algorithm = 'blake3'),
    path_encoding             TEXT NOT NULL DEFAULT 'utf-8'
);
```

Exactly one row exists with `singleton_id = 1`.

## 14.3 Snapshots

```sql
CREATE TABLE snapshots (
    id                 INTEGER PRIMARY KEY AUTOINCREMENT,
    created_at_ns      INTEGER NOT NULL,
    completed_at_ns    INTEGER,
    source_root         TEXT NOT NULL,
    message             TEXT NOT NULL DEFAULT '',
    status              TEXT NOT NULL
                        CHECK (status IN ('pending', 'complete', 'failed', 'cancelled', 'deleting')),
    file_count          INTEGER NOT NULL DEFAULT 0,
    directory_count     INTEGER NOT NULL DEFAULT 0,
    symlink_count       INTEGER NOT NULL DEFAULT 0,
    logical_size        INTEGER NOT NULL DEFAULT 0,
    new_stored_size     INTEGER NOT NULL DEFAULT 0,
    new_chunk_count     INTEGER NOT NULL DEFAULT 0,
    reused_chunk_count  INTEGER NOT NULL DEFAULT 0,
    duration_ms         INTEGER NOT NULL DEFAULT 0,
    failure_message     TEXT
);

CREATE INDEX idx_snapshots_status_created
    ON snapshots(status, created_at_ns DESC);
```

Only `status = 'complete'` is visible to normal restore and browse operations.

## 14.4 Entries

Use one table for files, directories, and symbolic links:

```sql
CREATE TABLE entries (
    id                  INTEGER PRIMARY KEY AUTOINCREMENT,
    snapshot_id         INTEGER NOT NULL
                        REFERENCES snapshots(id) ON DELETE CASCADE,
    relative_path       TEXT NOT NULL,
    parent_path         TEXT NOT NULL,
    name                TEXT NOT NULL,
    entry_type          TEXT NOT NULL
                        CHECK (entry_type IN ('file', 'directory', 'symlink')),
    logical_size        INTEGER NOT NULL DEFAULT 0,
    modified_time_ns    INTEGER NOT NULL DEFAULT 0,
    change_time_ns      INTEGER,
    posix_mode          INTEGER NOT NULL DEFAULT 0,
    windows_attributes  INTEGER,
    file_hash           TEXT,
    symlink_target      TEXT,
    source_device       INTEGER,
    source_inode        INTEGER,
    UNIQUE(snapshot_id, relative_path)
);

CREATE INDEX idx_entries_snapshot_parent_name
    ON entries(snapshot_id, parent_path, name);

CREATE INDEX idx_entries_snapshot_path
    ON entries(snapshot_id, relative_path);
```

`source_device` and `source_inode` hold the POSIX device/inode pair on Linux/macOS and the volume serial number/file ID on Windows. Together with `change_time_ns` (POSIX ctime; NTFS change time) they feed the incremental reuse rule (Section 17.1) and can later support hard-link preservation, but the first release does not recreate hard links.

`posix_mode` is `0` for entries captured on Windows. `windows_attributes` stores the captured `FILE_ATTRIBUTE_*` bits on Windows and is `NULL` for entries captured on POSIX platforms. See the platform policy matrix (Section 25.10).

The source root itself is represented with `relative_path = ''`, `parent_path = ''`, and `name = ''`. Child listing queries must exclude the root row when listing the root level:

```sql
SELECT ... FROM entries
WHERE snapshot_id = ? AND parent_path = '' AND relative_path <> '';
```

Test this rule explicitly; the root is the only row whose `parent_path` equals its own `relative_path`.

## 14.5 Chunks

```sql
CREATE TABLE chunks (
    hash                TEXT PRIMARY KEY,
    raw_size            INTEGER NOT NULL CHECK (raw_size > 0),
    compressed_size     INTEGER NOT NULL CHECK (compressed_size > 0),
    object_path         TEXT NOT NULL UNIQUE,
    created_at_ns       INTEGER NOT NULL
);
```

Do not store an authoritative mutable reference count. Derive references from `entry_chunks`; this avoids reference-count drift after crashes or bugs.

## 14.6 Entry-to-chunk mapping

```sql
CREATE TABLE entry_chunks (
    entry_id            INTEGER NOT NULL
                        REFERENCES entries(id) ON DELETE CASCADE,
    sequence_number     INTEGER NOT NULL CHECK (sequence_number >= 0),
    chunk_hash          TEXT NOT NULL
                        REFERENCES chunks(hash),
    raw_offset          INTEGER NOT NULL CHECK (raw_offset >= 0),
    raw_length          INTEGER NOT NULL CHECK (raw_length > 0),
    PRIMARY KEY(entry_id, sequence_number)
);

CREATE INDEX idx_entry_chunks_hash
    ON entry_chunks(chunk_hash);
```

For an empty file, insert an `entries` row with `logical_size = 0` and no `entry_chunks` rows.

## 14.7 Skipped entries

```sql
CREATE TABLE snapshot_warnings (
    id                  INTEGER PRIMARY KEY AUTOINCREMENT,
    snapshot_id         INTEGER NOT NULL
                        REFERENCES snapshots(id) ON DELETE CASCADE,
    relative_path       TEXT NOT NULL,
    warning_code        TEXT NOT NULL,
    message             TEXT NOT NULL
);

CREATE INDEX idx_snapshot_warnings_snapshot
    ON snapshot_warnings(snapshot_id);
```

## 14.8 Optional operation history

```sql
CREATE TABLE operation_history (
    id                  INTEGER PRIMARY KEY AUTOINCREMENT,
    operation_type      TEXT NOT NULL,
    started_at_ns       INTEGER NOT NULL,
    completed_at_ns     INTEGER,
    status              TEXT NOT NULL,
    summary_json        TEXT,
    failure_message     TEXT
);
```

This table is optional for the first CLI milestone, but useful for the GUI.

## 14.9 Repository settings

Mutable repository-scoped settings (Section 30.1) are stored as key/value rows and created by the initial migration:

```sql
CREATE TABLE repository_settings (
    key            TEXT PRIMARY KEY,
    value          TEXT NOT NULL,
    updated_at_ns  INTEGER NOT NULL
);
```

Defined keys in the first release: `default_source_root`, `default_worker_count`, `default_ignore_file`, `default_overwrite_policy`, `log_level`. Unknown keys are preserved but ignored. Immutable configuration (format version, hash algorithm, chunk size) stays in `repository_info` and is never stored here.

## 14.10 Schema migrations

Store SQL migrations as numbered embedded resources or C++ string constants:

```text
001_initial_schema.sql
002_add_operation_history.sql
```

Migration rules:

1. Begin an exclusive transaction.
2. Read the current maximum migration version.
3. Apply missing migrations in order.
4. Insert one `schema_migrations` row per migration.
5. Update `repository_info.format_version` only when repository compatibility changes.
6. Roll back everything if any migration fails.
7. Back up `repository.db` before destructive migrations in future versions.
8. Never modify a migration after it has been released; add a new migration.

---

# 15. Repository initialization

`Repository::create` performs these steps:

1. Convert the requested root to an absolute normalized path.
2. Reject an existing non-empty directory unless the caller explicitly approved it.
3. Classify the destination filesystem:
   - Network filesystems (NFS, SMB/CIFS, and other remote mounts; `DRIVE_REMOTE` on Windows) are rejected unless `allow_risky_filesystem` is set, because SQLite WAL and file locking are unreliable there.
   - FAT/exFAT produce a prominent warning: no permission bits and weaker rename/flush durability.
   - Detection uses `statfs`/`f_fstypename` on macOS, `statfs` `f_type` on Linux, and `GetDriveTypeW`/`GetVolumeInformationW` on Windows.
4. Create the repository root with restrictive access: mode `0700` on POSIX platforms. On Windows, rely on inherited user-profile ACLs; repositories created outside the user profile are not additionally restricted in the first release (documented limitation).
5. Create `objects`, `temporary/objects`, `temporary/restores`, and `logs`.
6. Create `repository.lock`.
7. Acquire the exclusive writer lock.
8. Create `repository.db`.
9. Enable SQLite foreign keys, WAL, and full synchronous mode.
10. Run migration `001_initial_schema`.
11. Insert the `repository_info` row:
    - New random UUID (version 4; generate from `getentropy`/`arc4random_buf` on POSIX and `BCryptGenRandom` on Windows — the standard library has no UUID facility).
    - Repository format version `1`.
    - Current application version.
    - Chunk size `4 * 1024 * 1024`.
    - zstd level `3`.
    - Hash algorithm `blake3`.
12. Commit the database transaction.
13. Flush the database and containing directory when practical.
14. Release the writer lock.
15. Re-open the repository using the normal validation path as a final self-check.

Failure handling:

- If creation fails before the repository becomes valid, remove only paths created by this operation.
- Never recursively delete a pre-existing user directory.
- Return a detailed error with the failing path.

---
