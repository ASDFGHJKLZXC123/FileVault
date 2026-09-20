# LocalVault Build Plan — Part 03: Architecture and Public API

> Sections 11–12 of the Technical Implementation Guide (Revision 2), split verbatim.
> Section numbers are unchanged, so cross-references like “Section 25.10”
> resolve via the lookup table in [00-INDEX.md](00-INDEX.md).

---

# 11. Architecture and dependency rules

## 11.1 Layered architecture

```text
┌─────────────────────────────────────────────────────────────┐
│ Interfaces                                                  │
│                                                             │
│ localvault CLI                 localvault_desktop (Qt)       │
└──────────────────────┬───────────────────────┬──────────────┘
                       │                       │
                       └──────────┬────────────┘
                                  ▼
┌─────────────────────────────────────────────────────────────┐
│ Application services                                        │
│                                                             │
│ SnapshotEngine     RestoreEngine       DiffEngine            │
│ IntegrityVerifier GarbageCollector     QueryService          │
└───────────────────────────────┬─────────────────────────────┘
                                ▼
┌─────────────────────────────────────────────────────────────┐
│ Domain and infrastructure components                        │
│                                                             │
│ Repository       FileScanner       IgnoreRules              │
│ Chunker          Blake3Hasher      ZstdCodec                 │
│ ObjectStore      MetadataStore     RepositoryLock            │
│ PathSafety       POSIX metadata    Progress reporting        │
└─────────────────────┬─────────────────────────┬─────────────┘
                      ▼                         ▼
               SQLite database          Repository filesystem
```

## 11.2 Dependency rules

1. `localvault_core` must not include Qt or CLI11 headers.
2. CLI code may depend on `localvault_core` and CLI11.
3. GUI code may depend on `localvault_core` and Qt.
4. Database classes may use SQLite directly; higher-level services must use database wrappers.
5. Storage wrappers may use BLAKE3 and zstd directly; higher-level services must not.
6. Platform-specific file locking and metadata code stays under `src/core/filesystem/platform`, as POSIX and Win32 implementations of one shared header. No `#ifdef` platform blocks outside that directory.
7. UI code must not execute the CLI as a subprocess.
8. Qt widgets must not contain snapshot, restore, verification, or garbage-collection algorithms.
9. Public headers under `include/localvault` expose stable application types, not SQLite handles or zstd contexts.
10. Every mutating repository operation requires an exclusive `RepositoryLock`.

## 11.3 Core components

### `Repository`

- Validates repository paths.
- Opens and configures SQLite.
- Checks repository format compatibility.
- Acquires the writer lock for mutating operations.
- Runs recovery of stale incomplete operations.
- Exposes repository paths and metadata services.

### `FileScanner`

- Recursively enumerates a source root.
- Uses `symlink_status` so it does not follow symlinks accidentally.
- Never traverses Windows directory junctions or volume mount points (Section 25.4).
- Optionally stays within one filesystem/volume (`one_file_system`).
- Applies ignore rules.
- Produces normalized relative paths.
- Emits regular-file, directory, and symbolic-link records.
- Skips unsupported special files and cloud placeholders with warnings.

### `Chunker`

- Reads regular files as a stream.
- Produces chunks up to 4 MiB.
- Handles empty files without producing a content chunk.
- Supplies each chunk to the full-file hasher and chunk processor.

### `Blake3Hasher`

- Wraps the BLAKE3 C API.
- Supports incremental update.
- Returns a fixed 32-byte digest.
- Converts digests to lowercase 64-character hexadecimal identifiers.

### `ZstdCodec`

- Compresses one raw chunk.
- Decompresses one stored object.
- Enforces expected raw-size limits during decompression.
- Converts zstd error codes into `LocalVaultError`.

### `ObjectStore`

- Maps a hash to an object path.
- Writes new objects through unique temporary files.
- Publishes objects atomically.
- Handles duplicate hashes safely.
- Reads and validates object data.
- Deletes only unreferenced objects selected by garbage collection.

### `MetadataStore`

This is implemented by the database wrapper and query methods. It:

- Runs migrations.
- Creates and updates snapshots.
- Inserts entries and chunk relationships.
- Lists snapshots and entries.
- Computes statistics.
- Finds unreferenced chunks.
- Deletes snapshot metadata transactionally.

### `SnapshotEngine`

Coordinates scanning, workers, object storage, metadata insertion, progress, retry policy, and snapshot state.

### `RestoreEngine`

Validates requested paths, retrieves ordered chunks, reconstructs files, verifies hashes, applies overwrite policy, and restores metadata.

### `DiffEngine`

Compares two snapshots by normalized relative path and entry metadata.

### `IntegrityVerifier`

Supports quick and full verification modes.

### `GarbageCollector`

Deletes stale incomplete snapshot metadata, stale temporary files, and unreferenced objects under an exclusive repository lock.

### `QueryService`

Provides read-only queries used by both interfaces:

- Snapshot list.
- Snapshot details.
- Directory children.
- Path search.
- Repository statistics.
- Verification history if stored.

---

# 12. Core public API

The exact implementation may evolve, but preserve the separation below.

## 12.1 Common types

`include/localvault/types.hpp`:

```cpp
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace localvault {

using SnapshotId = std::int64_t;
using ByteCount = std::uint64_t;
using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;

enum class EntryType {
    regular_file,
    directory,
    symbolic_link
};

enum class SnapshotStatus {
    pending,
    complete,
    failed,
    cancelled,
    deleting
};

enum class OverwritePolicy {
    never,
    prompt,
    always
};

enum class VerifyMode {
    quick,
    full
};

struct SnapshotSummary {
    SnapshotId id{};
    TimePoint created_at{};
    std::filesystem::path source_root;
    std::string message;
    SnapshotStatus status{SnapshotStatus::pending};
    std::uint64_t file_count{};
    std::uint64_t directory_count{};
    ByteCount logical_size{};
    ByteCount new_stored_size{};
    std::chrono::milliseconds duration{};
};

struct EntryInfo {
    std::int64_t id{};
    SnapshotId snapshot_id{};
    std::filesystem::path relative_path;
    EntryType type{EntryType::regular_file};
    ByteCount logical_size{};
    std::int64_t modified_time_ns{};
    std::uint32_t posix_mode{};
    std::optional<std::uint32_t> windows_attributes;
    std::optional<std::string> file_hash_hex;
    std::optional<std::filesystem::path> symlink_target;
};

struct RepositoryStats {
    std::uint64_t complete_snapshot_count{};
    std::uint64_t unique_chunk_count{};
    ByteCount logical_bytes{};
    ByteCount unique_raw_bytes{};
    ByteCount stored_bytes{};
    double deduplication_savings{};
    double compression_savings{};
    double total_savings{};
};

struct SkippedEntry {
    std::filesystem::path relative_path;
    std::string reason;
};

}  // namespace localvault
```

## 12.2 Error model

`include/localvault/error.hpp`:

```cpp
#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

namespace localvault {

enum class ErrorCode {
    invalid_argument,
    repository_not_found,
    invalid_repository,
    unsupported_repository_version,
    repository_busy,
    filesystem_error,
    database_error,
    compression_error,
    hashing_error,
    object_missing,
    object_corrupt,
    unsafe_restore_path,
    destination_exists,
    source_changed,
    cancelled,
    partial_success,
    internal_error
};

class LocalVaultError final : public std::runtime_error {
public:
    LocalVaultError(
        ErrorCode code,
        std::string message,
        std::filesystem::path path = {})
        : std::runtime_error(std::move(message)),
          code_(code),
          path_(std::move(path)) {}

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    ErrorCode code_;
    std::filesystem::path path_;
};

}  // namespace localvault
```

Use exceptions for fatal operation errors crossing service boundaries. Use value types for expected per-file outcomes such as skipped or unstable files. Never throw through a C callback.

`ErrorCode::partial_success` is reserved for interface-boundary exit-code mapping. Core operations never throw it; they report partial success through warning lists in their result types (Section 31.4).

## 12.3 Progress API

`include/localvault/progress.hpp`:

```cpp
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace localvault {

enum class OperationPhase {
    preparing,
    scanning,
    reading,
    hashing,
    compressing,
    writing_objects,
    writing_metadata,
    restoring,
    verifying,
    garbage_collecting,
    finalizing,
    complete
};

struct ProgressEvent {
    OperationPhase phase{OperationPhase::preparing};
    std::filesystem::path current_path;
    std::uint64_t discovered_entries{};
    std::uint64_t processed_entries{};
    std::uint64_t processed_bytes{};
    std::optional<std::uint64_t> total_entries;
    std::optional<std::uint64_t> total_bytes;
    std::uint64_t new_chunks{};
    std::uint64_t reused_chunks{};
    std::string message;
};

using ProgressCallback = std::function<void(const ProgressEvent&)>;

}  // namespace localvault
```

The callback may be invoked from worker threads. Interface adapters must marshal events to their own threads.

`total_entries` and `total_bytes` are empty until the scan phase completes, because the pipeline overlaps scanning with processing. Interfaces must show indeterminate progress until totals arrive, then switch to a percentage.

## 12.4 Repository API

`include/localvault/repository.hpp`:

```cpp
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

namespace localvault {

class FailureInjector;

struct RepositoryCreateOptions {
    std::uint64_t chunk_size_bytes{4ULL * 1024ULL * 1024ULL};
    int zstd_level{3};
    bool allow_risky_filesystem{false};
};

enum class OpenMode {
    read_only,
    read_write
};

class Repository final {
public:
    static void create(
        const std::filesystem::path& root,
        const RepositoryCreateOptions& options = {});

    static Repository open(
        const std::filesystem::path& root,
        OpenMode mode = OpenMode::read_write);

    Repository(Repository&&) noexcept;
    Repository& operator=(Repository&&) noexcept;
    ~Repository();

    Repository(const Repository&) = delete;
    Repository& operator=(const Repository&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const noexcept;
    [[nodiscard]] std::uint32_t format_version() const noexcept;

    // Test-only seam (Section 32.5). Production callers never call this;
    // the default injector is a no-op.
    void set_failure_injector(std::shared_ptr<FailureInjector> injector);

private:
    class Impl;
    explicit Repository(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend class SnapshotEngine;
    friend class RestoreEngine;
    friend class DiffEngine;
    friend class IntegrityVerifier;
    friend class GarbageCollector;
    friend class QueryService;
};

}  // namespace localvault
```

Use a PImpl to prevent SQLite and platform details from leaking into public headers.

Open-mode semantics:

- `read_write` (default): mutating engines may run. Each mutating or object-reading operation acquires the exclusive repository lock per operation (Section 23.6). Stale-operation recovery runs at the start of the first mutating operation, under the lock.
- `read_only`: never creates files, never acquires the writer lock, never runs recovery, and works on read-only media. Only `QueryService` and `DiffEngine` accept a read-only repository; other engines throw `invalid_argument`.

## 12.5 Snapshot API

`include/localvault/snapshot_engine.hpp`:

```cpp
#pragma once

#include "localvault/progress.hpp"
#include "localvault/types.hpp"

#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

namespace localvault {

class Repository;

struct SnapshotOptions {
    std::string message;
    std::size_t worker_count{};
    bool retry_unstable_files{true};
    bool force_rehash{false};
    bool include_hidden{true};
    bool one_file_system{false};
};

struct SnapshotResult {
    SnapshotId snapshot_id{};
    std::uint64_t file_count{};
    std::uint64_t directory_count{};
    ByteCount logical_bytes{};
    ByteCount new_stored_bytes{};
    std::uint64_t new_chunks{};
    std::uint64_t reused_chunks{};
    std::vector<SkippedEntry> skipped_entries;
};

class SnapshotEngine final {
public:
    explicit SnapshotEngine(Repository& repository);

    SnapshotResult create_snapshot(
        const std::filesystem::path& source_root,
        const SnapshotOptions& options,
        std::stop_token stop_token = {},
        ProgressCallback progress = {});

private:
    Repository& repository_;
};

}  // namespace localvault
```

If `worker_count == 0`, use the default worker count defined in Section 24.4 — that section is the single normative definition. Do not allow an unbounded worker count from user input.

## 12.6 Restore API

`include/localvault/restore_engine.hpp`:

```cpp
#pragma once

#include "localvault/progress.hpp"
#include "localvault/types.hpp"

#include <filesystem>
#include <functional>
#include <stop_token>
#include <vector>

namespace localvault {

class Repository;

enum class ConflictDecision {
    skip,
    replace,
    cancel
};

using ConflictResolver = std::function<ConflictDecision(
    const std::filesystem::path& destination,
    EntryType incoming_type)>;

struct RestoreRequest {
    SnapshotId snapshot_id{};
    std::vector<std::filesystem::path> relative_paths;
    std::filesystem::path destination_root;
    OverwritePolicy overwrite_policy{OverwritePolicy::never};
    ConflictResolver conflict_resolver;
    bool verify_final_file_hash{true};
};

struct RestoreResult {
    std::uint64_t restored_files{};
    std::uint64_t restored_directories{};
    std::uint64_t restored_symlinks{};
    ByteCount restored_bytes{};
    std::vector<SkippedEntry> skipped_entries;
};

class RestoreEngine final {
public:
    explicit RestoreEngine(Repository& repository);

    RestoreResult restore(
        const RestoreRequest& request,
        std::stop_token stop_token = {},
        ProgressCallback progress = {});

private:
    Repository& repository_;
};

}  // namespace localvault
```

The core library must not implement interactive prompts. `OverwritePolicy::prompt` requires a non-empty `conflict_resolver`; the core invokes it synchronously per conflict, and interface adapters marshal the question to their own threads (usage rules in Section 42.11).

## 12.7 Verification API

```cpp
struct VerificationIssue {
    enum class Kind {
        missing_object,
        corrupt_object,
        invalid_chunk_size,
        invalid_entry_relationship,
        invalid_snapshot_state,
        stale_temporary_file
    };

    Kind kind{};
    std::filesystem::path path;
    std::string detail;
};

struct VerificationResult {
    std::uint64_t checked_snapshots{};
    std::uint64_t checked_entries{};
    std::uint64_t checked_objects{};
    ByteCount checked_stored_bytes{};
    std::vector<VerificationIssue> issues;

    [[nodiscard]] bool ok() const noexcept { return issues.empty(); }
};

class IntegrityVerifier final {
public:
    explicit IntegrityVerifier(Repository& repository);

    VerificationResult verify(
        VerifyMode mode,
        std::stop_token stop_token = {},
        ProgressCallback progress = {});
};
```

## 12.8 Query API

Use page-based queries so the GUI never loads an entire large snapshot tree at once:

```cpp
struct PageRequest {
    std::uint64_t offset{};
    std::uint32_t limit{200};
};

template <typename T>
struct Page {
    std::vector<T> items;
    std::uint64_t total_count{};
};

class QueryService final {
public:
    explicit QueryService(Repository& repository);

    Page<SnapshotSummary> list_snapshots(PageRequest page);
    SnapshotSummary get_snapshot(SnapshotId id);
    Page<EntryInfo> list_children(
        SnapshotId id,
        const std::filesystem::path& parent,
        PageRequest page);
    Page<EntryInfo> search_paths(
        SnapshotId id,
        std::string_view query,
        PageRequest page);
    RepositoryStats repository_stats();
};
```

---
