# LocalVault Build Plan — Part 07: Crash Consistency, Concurrency, and Platform Behavior

> Sections 23–27 of the Technical Implementation Guide (Revision 2), split verbatim.
> Section numbers are unchanged, so cross-references like “Section 25.10”
> resolve via the lookup table in [00-INDEX.md](00-INDEX.md).

---

# 23. Crash consistency and recovery

## 23.1 Consistency model

LocalVault guarantees:

- Previously complete snapshots are never modified.
- New objects are immutable.
- New snapshots are invisible until marked `complete`.
- A crash may leave temporary files, unreferenced objects, or a non-complete snapshot row.
- Recovery can remove those leftovers without changing complete snapshots.

## 23.2 Object durability sequence

For a new object:

```text
write temporary file
→ flush stream
→ fsync temporary file
→ atomic rename into objects/
→ fsync objects shard directory
→ insert/confirm chunks row
→ allow entry metadata to reference it
```

Do not reference an object before it is durably published.

Platform durability statement:

- **Linux/macOS:** flush the stream, `fsync` the temporary file (`F_FULLFSYNC` on macOS where full durability is required), `rename(2)`, then `fsync` the shard directory.
- **Windows:** flush the stream, `FlushFileBuffers` on the temporary file, then `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`. Windows has no directory fsync; NTFS journals metadata, so rename durability relies on the filesystem journal. This is a documented, weaker guarantee than POSIX (Section 39). The recovery model already tolerates a lost rename: the object is then simply absent, and its snapshot is still `pending`.

## 23.3 Snapshot publication

The final transaction:

1. Confirms no fatal worker error occurred.
2. Confirms all metadata queues are drained.
3. Calculates counters from committed metadata where practical.
4. Updates the snapshot row from `pending` to `complete`.
5. Sets completion time and duration.
6. Commits.

No other status transition can make a snapshot restorable.

## 23.4 Startup recovery

On repository open for a mutating operation:

1. Acquire the writer lock.
2. Find snapshots with `status != 'complete'`.
3. Mark stale `pending` snapshots as `failed`, preserve the row with failure information, and delete their entries, mappings, and warnings in bounded batches.
4. Resume `deleting` snapshots: continue the batched deletion of Section 22.1 to completion.
5. Remove all files under `temporary/`. The exclusive writer lock guarantees no live operation owns them (the v1 locking rule, Section 23.6), so no age threshold is needed. On Windows, tolerate sharing violations by skipping the file and retrying on the next recovery.
6. Leave orphan final objects for explicit GC.
7. Run a quick relationship check.

## 23.5 SQLite transaction wrapper

Use RAII:

```cpp
class Transaction {
public:
    explicit Transaction(Database& db);
    ~Transaction();  // rolls back if not committed

    void commit();

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

private:
    Database* db_;
    bool committed_{false};
};
```

Never call `COMMIT` from a destructor. The destructor only performs best-effort rollback.

## 23.6 Repository lock

### The v1 locking rule

One exclusive lock covers every operation that mutates the repository **or reads object files**:

- Take the exclusive lock: `init`, `snapshot`, `delete`, `gc`, recovery, `restore`, and `verify`. Restore and verify take it because they read object files and must not race garbage collection deleting those objects.
- No lock required: pure-metadata queries (`list`, `show`, `files`, `diff`, `stats`) read only SQLite, and WAL gives them a consistent view concurrent with a writer.

Relaxing this later requires a shared/exclusive (reader–writer) lock so restores and verifies can run concurrently with each other while still excluding GC. Do not relax it ad hoc.

### Implementation

POSIX (Linux/macOS): advisory lock with `flock(LOCK_EX | LOCK_NB)` or `fcntl(F_SETLK)` on `repository.lock`.

Windows: open `repository.lock` with `CreateFileW` and acquire `LockFileEx(LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY)`. Keep the handle open for the lock lifetime; release with `UnlockFileEx`/`CloseHandle` in the RAII destructor. Never delete `repository.lock` during cleanup — an open file cannot be deleted on Windows, and the file's existence is harmless.

Required behavior on all platforms:

- Open/create `repository.lock`.
- Acquire the exclusive non-blocking lock.
- Return `repository_busy` if another process owns it.
- Keep the descriptor/handle open for the lock lifetime.
- Release in the RAII destructor.
- Record PID and start time in the lock file for diagnostics, but do not treat text content alone as lock ownership.

---

# 24. Concurrency and cancellation

## 24.1 Pipeline

```text
Scanner thread
    │ FileJob
    ▼
BoundedQueue<FileJob>
    │
    ├── Worker 1 ─┐
    ├── Worker 2 ─┤ ProcessedEntry
    ├── Worker 3 ─┤
    └── Worker N ─┘
                  ▼
       BoundedQueue<ProcessedEntry>
                  │
                  ▼
          Metadata writer thread
```

## 24.2 Work units

```cpp
struct FileJob {
    std::filesystem::path absolute_path;
    std::filesystem::path relative_path;
    FileMetadata before;
};

struct ChunkReference {
    std::string hash_hex;
    std::uint64_t raw_offset{};
    std::uint32_t raw_length{};
    std::uint64_t compressed_size{};
    bool newly_stored{};
};

struct ProcessedEntry {
    EntryInfo entry;
    std::vector<ChunkReference> chunks;
    bool reused_previous_metadata{};
};
```

## 24.3 Bounded queues

Implement a queue with:

- Maximum item count and/or byte budget.
- `push` that waits until space or cancellation.
- `pop` that waits until data, closure, or cancellation.
- `close` to signal no more items.
- No busy waiting.
- Condition variables protected by one mutex.
- Exception-safe element transfer.

A byte budget is useful because one result may contain thousands of chunk references.

## 24.4 Worker count

Default:

```cpp
auto count = std::thread::hardware_concurrency();
count = count == 0 ? 4 : count;
count = std::clamp(count, 1U, 16U);
```

Allow configuration but enforce a safe maximum.

Disk-bound workloads may perform worse with too many workers. Benchmark before changing the default.

## 24.5 SQLite access

Initial policy:

- One write connection owned by the metadata writer.
- Read-only queries from the coordinating thread or separate read connections.
- No SQLite connection is shared concurrently unless opened/configured for that use and protected.
- Prepared statements are not shared across threads.

## 24.6 Cancellation

Use `std::stop_source`, `std::stop_token`, and `std::jthread`.

Check the token:

- Before scanning a directory.
- Before opening a file.
- Between chunks.
- Before compression.
- Before long database batches.
- Between objects during verification and GC.

Cancellation is cooperative. Do not forcibly terminate worker threads.

## 24.7 Error propagation

- The first fatal worker error is stored in a synchronized `std::exception_ptr`.
- Request stop on all workers.
- Close queues.
- Join all threads.
- Convert the original exception to a structured operation failure.
- Do not allow worker exceptions to escape a thread function.

## 24.8 Progress aggregation

Workers update atomic counters. A coordinator emits throttled progress events, for example at most 10 times per second, plus phase changes. Avoid invoking expensive UI callbacks for every chunk.

---

# 25. Filesystem behavior

## 25.1 Relative path representation

Store repository paths as normalized UTF-8 with `/` separators.

First-release policy:

- Accept paths convertible to valid UTF-8. On Windows, convert from UTF-16; names containing unpaired surrogates are not convertible and are skipped with a warning.
- Skip non-convertible paths with a warning.
- Do not silently replace invalid bytes.
- Use exact stored path strings for identity. **No Unicode normalization is applied**: macOS commonly produces NFD names while Linux and Windows commonly produce NFC, so two names that render identically but differ in normalization are distinct entries. Document this; never normalize silently.

On case-insensitive or normalization-insensitive destination filesystems, detect collisions during restore planning and skip losers with warnings (Section 25.12).

## 25.2 Regular files

- Open in binary mode.
- Stream contents.
- Save size, mtime, change time, and platform metadata per the policy matrix (Section 25.10).
- Do not preserve ownership in the first release.
- Do not preserve sparse holes in the first release.

## 25.3 Directories

- Store the root and empty directories.
- Restore parents before children.
- Apply final directory metadata after restoring children.

## 25.4 Symbolic links

- Use `symlink_status`.
- Save `read_symlink` text.
- Do not recurse through symlink targets.
- Restore the link itself.
- On Windows, NTFS symbolic links are saved and restored like POSIX symlinks (restore may require privilege, Section 19.4). Directory junctions are saved as symlink entries with their target text and are never traversed; volume mount points are skipped with a warning (FR-117). Never traversing junctions also prevents the classic recursion loops in Windows user profiles (for example the legacy `Application Data` junction).

## 25.5 Special files

Skip and warn for:

- Sockets.
- FIFOs/named pipes.
- Block devices.
- Character devices.
- Unknown file types.

Never attempt to read a device as a regular file.

## 25.6 Hidden files

Include hidden files by default. Ignore rules may exclude them.

## 25.7 Hard links

The first release treats hard-linked paths as independent logical files. Chunk deduplication prevents duplicated stored bytes, but restore creates separate files. Record device/inode fields for future hard-link support.

## 25.8 Source/repository containment

Reject:

- Source path equal to repository root.
- Source inside repository.
- Repository inside source.

A future version may automatically ignore the repository, but explicit rejection is safer.

## 25.9 Metadata failures

Content restore success should not be discarded because applying a nonessential timestamp or mode failed. Report metadata failure as a warning and return partial success.

## 25.10 Platform metadata policy matrix

This matrix is the single normative statement of what is saved and restored per platform. FR-104 and FR-309 refer here.

| Item | Linux | macOS | Windows |
|---|---|---|---|
| File content, size | saved + restored | saved + restored | saved + restored |
| Modification time | saved + restored | saved + restored | saved + restored |
| Change time (ctime) | saved (reuse rule only, never restored) | saved (reuse only) | saved (NTFS change time, reuse only) |
| POSIX mode | saved + restored | saved + restored | not saved (`posix_mode = 0`) |
| Windows attributes | — | — | READONLY/HIDDEN/SYSTEM saved + restored; ARCHIVE saved, not restored |
| Ownership (uid/gid, SID) | not saved | not saved | not saved |
| ACLs / xattrs / ADS | not saved | not saved | not saved |
| Symbolic links | saved + restored | saved + restored | saved; restored when privilege allows, else skip + warn |
| Directory junctions | — | — | saved as link entries, never traversed |
| Hard-link relationships | not recreated | not recreated | not recreated |

Cross-platform restores apply only the columns the destination supports; everything else degrades to a warning (Section 25.12).

## 25.11 Windows filesystem specifics

- **Long paths:** embed a `longPathAware` application manifest in both executables and use `\\?\`-prefixed absolute paths inside the Win32 platform layer, so paths beyond 260 characters work regardless of system policy.
- **Sharing violations:** files opened exclusively by other processes (`ERROR_SHARING_VIOLATION`) are skipped with a warning after the standard unstable-file retry (FR-116). VSS integration is future work (Section 40.5).
- **Cloud placeholders:** files carrying `FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS` or `FILE_ATTRIBUTE_RECALL_ON_OPEN` (OneDrive Files On-Demand and similar) are skipped with a warning; reading them would silently download content (FR-118).
- **Antivirus interference:** rapid creation of many object files can trigger transient open failures from real-time scanners. Wrap object publication in a small bounded retry, and document Defender exclusions as a user-level optimization.
- **8.3 short names:** store long names only; never store or match `PROGRA~1`-style aliases.
- **One-file-system option:** `one_file_system` (FR-119) stops the scanner at filesystem boundaries on POSIX (compare `st_dev`) and at volume boundaries on Windows. Junctions and mount points are never traversed on Windows regardless of this option.

## 25.12 Cross-platform repository portability

A repository created on any supported platform must open on every other supported platform: SQLite, zstd frames, and hash-derived object paths are byte-portable, and stored paths are normalized UTF-8 with `/` separators.

The constraint is restore-side representability. During restore planning:

- On Windows, entries whose names contain characters illegal on NTFS (`< > : " / \ | ? *`, control characters), reserved device names (`CON`, `PRN`, `AUX`, `NUL`, `COM1`–`COM9`, `LPT1`–`LPT9`, with or without extension), or trailing dots/spaces are skipped with warnings (FR-310).
- On case-insensitive or normalization-insensitive destinations, entries that collide after the filesystem's folding are detected in the plan; the first entry in path order is restored and the rest are skipped with warnings.
- Metadata degrades per the policy matrix: restoring a Windows-captured snapshot on Linux applies umask-default permissions with a warning; restoring a POSIX snapshot on Windows ignores `posix_mode`.

Add integration tests that create a repository on one platform in CI, hand it off as an artifact, and open/verify/restore it on the other platforms (Section 32.3).

---

# 26. Restore security

## 26.1 Lexical validation

For every stored relative path:

```cpp
bool is_safe_relative_path(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_path()) {
        return false;
    }

    const auto normalized = path.lexically_normal();
    for (const auto& component : normalized) {
        if (component == "..") {
            return false;
        }
    }
    return normalized != ".";
}
```

This is necessary but not sufficient when existing symbolic links are present in the destination.

On Windows, additionally reject before the checks above: paths with a root name (`C:`, `\\server\share`, including drive-relative forms like `C:foo`), backslashes in stored paths (stored paths use `/` only), reserved device names, and components ending in a dot or space (Section 25.12).

## 26.2 Containment validation

Minimum implementation:

1. Require an absolute destination root.
2. Create or canonicalize the destination root.
3. Reject unsafe relative paths.
4. Join root and normalized relative path.
5. Verify existing ancestors are not symbolic links.
6. Re-check immediately before publication.

Hardened POSIX implementation:

- Open the destination root directory once.
- Walk/create components with `openat`/`mkdirat`.
- Use `O_NOFOLLOW` for existing components.
- Create output files relative to trusted directory descriptors.
- Avoid security decisions based only on string prefix comparisons.

Hardened Windows implementation (there is no `openat` equivalent in the C runtime):

- Canonicalize the destination root once and operate on `\\?\`-prefixed absolute paths only.
- For each existing component, open it with `FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS` and reject any component carrying `FILE_ATTRIBUTE_REPARSE_POINT` (symlink, junction, or any other reparse tag).
- Create output files with `CREATE_NEW` against the validated path, and re-verify the parent chain immediately before publication.
- The same rule applies: never decide containment from string prefixes alone.

## 26.3 String-prefix checks are forbidden

This is unsafe:

```cpp
candidate.string().starts_with(root.string())
```

For example, `/safe/root2` begins with `/safe/root` as text but is not inside it.

## 26.4 Malicious repository metadata

Treat repository metadata as untrusted during restore even when LocalVault created it. A damaged database must not cause writes outside the destination.

## 26.5 Resource limits

Before allocating:

- Require chunk `raw_size <= repository chunk size`.
- Require compressed size below a reasonable configured limit.
- Detect integer overflow in byte-count arithmetic.
- Limit search page size.
- Limit worker count.
- Limit error collection or stream issues incrementally for severely corrupt repositories.

---

# 27. Ignore rules

Use a root-level file named:

```text
.localvaultignore
```

## 27.1 First-release syntax

Support:

- Blank lines.
- Comments beginning with `#`.
- Exact relative paths.
- Directory patterns ending in `/`.
- `*` within one path component.
- `?` for one character within a component.
- Patterns without `/` matching a name at any depth.
- `!` negation may be deferred; if implemented, define ordering clearly.

Example:

```gitignore
# Build output
build/
out/

# Temporary files
*.tmp
*.log

# Tool caches
.cache/
.DS_Store
```

## 27.2 Matching rules

- Normalize separators to `/`.
- Match against repository-relative paths.
- Decide case sensitivity according to source filesystem behavior; do not force lowercase.
- When a directory is ignored, do not recurse into it.
- The ignore file itself is included unless explicitly ignored.
- When `--ignore-file <path>` is supplied on the CLI, it replaces the source root's `.localvaultignore` entirely; the two are never merged.
- Always ignore the repository if a future configuration allows it inside the source, though the first release rejects that layout.

## 27.3 Test cases

Test:

- Exact filename.
- Extension wildcard.
- Directory wildcard.
- Nested directory.
- Pattern containing spaces.
- Escaped leading `#` if escape syntax is supported.
- Hidden files.
- Case differences.
- Non-match close to a pattern.

Do not claim full Git ignore compatibility unless it is actually implemented.

---
