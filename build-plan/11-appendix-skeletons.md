# LocalVault Build Plan — Part 11: Appendix: Implementation Skeletons

> Section 42 of the Technical Implementation Guide (Revision 2), split verbatim.
> Section numbers are unchanged, so cross-references like “Section 25.10”
> resolve via the lookup table in [00-INDEX.md](00-INDEX.md).

---

# 42. Appendix: implementation skeletons

The following skeletons establish conventions. They are not substitutes for tests and error handling.

## 42.1 BLAKE3 wrapper

```cpp
#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string>

struct blake3_hasher;

namespace localvault {

class Blake3Hasher final {
public:
    static constexpr std::size_t digest_size = 32;
    using Digest = std::array<std::byte, digest_size>;

    Blake3Hasher();
    ~Blake3Hasher();

    Blake3Hasher(Blake3Hasher&&) noexcept;
    Blake3Hasher& operator=(Blake3Hasher&&) noexcept;

    Blake3Hasher(const Blake3Hasher&) = delete;
    Blake3Hasher& operator=(const Blake3Hasher&) = delete;

    void update(std::span<const std::byte> bytes);
    [[nodiscard]] Digest finalize() const;
    [[nodiscard]] static std::string to_hex(const Digest& digest);

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace localvault
```

Prefer `std::unique_ptr<Impl>` in the actual implementation to simplify ownership.

## 42.2 zstd wrapper

```cpp
class ZstdCodec final {
public:
    explicit ZstdCodec(int compression_level);

    [[nodiscard]] std::vector<std::byte> compress(
        std::span<const std::byte> raw) const;

    [[nodiscard]] std::vector<std::byte> decompress(
        std::span<const std::byte> compressed,
        std::size_t expected_raw_size,
        std::size_t maximum_raw_size) const;

private:
    int compression_level_;
};
```

## 42.3 SQLite statement wrapper

```cpp
class Statement final {
public:
    Statement(sqlite3* db, std::string_view sql);
    ~Statement();

    Statement(Statement&&) noexcept;
    Statement& operator=(Statement&&) noexcept;

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void bind(std::string_view name, std::int64_t value);
    void bind(std::string_view name, std::string_view value);
    void bind_null(std::string_view name);

    [[nodiscard]] bool step();
    void execute();
    void reset();

    [[nodiscard]] std::int64_t column_int64(int index) const;
    [[nodiscard]] std::string column_text(int index) const;
    [[nodiscard]] bool column_is_null(int index) const;

private:
    sqlite3_stmt* statement_{nullptr};
};
```

The implementation must check every bind, step, reset, and finalize result that can report an error.

## 42.4 Bounded queue skeleton

```cpp
template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity)
        : capacity_(capacity) {
        if (capacity_ == 0) {
            throw std::invalid_argument("queue capacity must be positive");
        }
    }

    bool push(T value, std::stop_token stop_token) {
        std::unique_lock lock(mutex_);
        condition_not_full_.wait(lock, stop_token, [this] {
            return closed_ || queue_.size() < capacity_;
        });

        if (closed_ || stop_token.stop_requested()) {
            return false;
        }

        queue_.push_back(std::move(value));
        condition_not_empty_.notify_one();
        return true;
    }

    std::optional<T> pop(std::stop_token stop_token) {
        std::unique_lock lock(mutex_);
        condition_not_empty_.wait(lock, stop_token, [this] {
            return closed_ || !queue_.empty();
        });

        if (queue_.empty()) {
            return std::nullopt;
        }

        T value = std::move(queue_.front());
        queue_.pop_front();
        condition_not_full_.notify_one();
        return value;
    }

    void close() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        condition_not_empty_.notify_all();
        condition_not_full_.notify_all();
    }

private:
    std::size_t capacity_;
    std::deque<T> queue_;
    bool closed_{false};
    std::mutex mutex_;
    std::condition_variable_any condition_not_empty_;
    std::condition_variable_any condition_not_full_;
};
```

Add tests for closure, cancellation, multiple producers/consumers, and exception-safe moves.

## 42.5 Safe integer conversion

Avoid unchecked narrowing from SQLite signed integers to unsigned byte counts:

```cpp
std::uint64_t checked_to_u64(std::int64_t value, std::string_view field) {
    if (value < 0) {
        throw LocalVaultError(
            ErrorCode::database_error,
            std::string(field) + " contains a negative value");
    }
    return static_cast<std::uint64_t>(value);
}
```

Check additions:

```cpp
std::uint64_t checked_add(std::uint64_t a, std::uint64_t b) {
    if (b > std::numeric_limits<std::uint64_t>::max() - a) {
        throw LocalVaultError(ErrorCode::internal_error, "byte count overflow");
    }
    return a + b;
}
```

## 42.6 Object path derivation

```cpp
std::filesystem::path object_relative_path(std::string_view hash_hex) {
    if (hash_hex.size() != 64 ||
        !std::ranges::all_of(hash_hex, [](unsigned char c) {
            return std::isdigit(c) != 0 ||
                   (c >= static_cast<unsigned char>('a') &&
                    c <= static_cast<unsigned char>('f'));
        })) {
        throw LocalVaultError(ErrorCode::invalid_argument, "invalid BLAKE3 hash");
    }

    return std::filesystem::path("objects") /
           std::string(hash_hex.substr(0, 2)) /
           (std::string(hash_hex) + ".zst");
}
```

Cast safely when calling character-classification functions.

## 42.7 Snapshot metadata publication pseudocode

```cpp
SnapshotResult SnapshotEngine::create_snapshot(...) {
    auto lock = repository_.acquire_exclusive_lock();
    repository_.recover_stale_operations();

    const auto snapshot_id =
        repository_.database().insert_pending_snapshot(source_root, options.message);

    try {
        run_pipeline(snapshot_id, source_root, options, stop_token, progress);

        if (stop_token.stop_requested()) {
            repository_.database().cancel_and_clean_snapshot(snapshot_id);
            throw LocalVaultError(ErrorCode::cancelled, "snapshot cancelled");
        }

        auto result = repository_.database().calculate_snapshot_result(snapshot_id);
        repository_.database().publish_snapshot(snapshot_id, result);
        return result;
    } catch (const LocalVaultError& error) {
        repository_.database().fail_and_clean_snapshot(snapshot_id, error.what());
        throw;
    } catch (const std::exception& error) {
        repository_.database().fail_and_clean_snapshot(snapshot_id, error.what());
        throw LocalVaultError(ErrorCode::internal_error, error.what());
    }
}
```

Ensure failure cleanup itself cannot replace the original exception. Log cleanup failure and preserve the original cause.

## 42.8 Full-file reconstruction pseudocode

```cpp
void RestoreEngine::restore_file(
    const EntryInfo& entry,
    const std::filesystem::path& destination,
    std::stop_token stop_token) {

    validate_restore_destination(destination);

    TemporaryOutputFile output(destination);
    Blake3Hasher file_hasher;
    std::uint64_t written = 0;

    for (const auto& chunk : database_.chunks_for_entry(entry.id)) {
        if (stop_token.stop_requested()) {
            throw LocalVaultError(ErrorCode::cancelled, "restore cancelled");
        }

        auto raw = object_store_.read_verified(
            chunk.hash_hex,
            chunk.raw_length);

        output.write(raw);
        file_hasher.update(raw);
        written = checked_add(written, raw.size());
    }

    if (written != entry.logical_size) {
        throw LocalVaultError(
            ErrorCode::object_corrupt,
            "reconstructed size does not match snapshot metadata",
            entry.relative_path);
    }

    const auto restored_hash = Blake3Hasher::to_hex(file_hasher.finalize());
    if (!entry.file_hash_hex || restored_hash != *entry.file_hash_hex) {
        throw LocalVaultError(
            ErrorCode::object_corrupt,
            "reconstructed file hash does not match snapshot metadata",
            entry.relative_path);
    }

    output.flush_and_sync();
    output.apply_metadata(entry);
    output.publish_atomically();
}
```

Handle empty files: their hasher must finalize the BLAKE3 digest of empty input.

## 42.9 Stats queries

Distinct chunks referenced by one snapshot:

```sql
SELECT
    COALESCE(SUM(c.raw_size), 0) AS unique_raw_bytes,
    COALESCE(SUM(c.compressed_size), 0) AS stored_bytes
FROM chunks AS c
WHERE c.hash IN (
    SELECT DISTINCT ec.chunk_hash
    FROM entry_chunks AS ec
    JOIN entries AS e ON e.id = ec.entry_id
    WHERE e.snapshot_id = :snapshot_id
);
```

Logical bytes:

```sql
SELECT COALESCE(SUM(logical_size), 0)
FROM entries
WHERE snapshot_id = :snapshot_id
  AND entry_type = 'file';
```

Repository-wide retained unique bytes:

```sql
SELECT
    COALESCE(SUM(raw_size), 0),
    COALESCE(SUM(compressed_size), 0)
FROM chunks
WHERE hash IN (
    SELECT DISTINCT ec.chunk_hash
    FROM entry_chunks AS ec
    JOIN entries AS e ON e.id = ec.entry_id
    JOIN snapshots AS s ON s.id = e.snapshot_id
    WHERE s.status = 'complete'
);
```

## 42.10 Minimal CLI bootstrap

```cpp
#include <CLI/CLI.hpp>

#include "localvault/error.hpp"

#include <iostream>

int main(int argc, char** argv) {
    CLI::App app{"LocalVault snapshot backup tool"};

    std::string repository_path;
    bool verbose = false;

    app.add_option("--repo", repository_path, "Repository path");
    app.add_flag("--verbose", verbose, "Enable detailed diagnostics");

    // Add subcommands and bind option structures here.

    try {
        CLI11_PARSE(app, argc, argv);
        // Dispatch selected subcommand.
        return 0;
    } catch (const localvault::LocalVaultError& error) {
        std::cerr << "localvault: " << error.what() << '\n';
        return map_error_to_exit_code(error.code());
    } catch (const std::exception& error) {
        std::cerr << "localvault: unexpected error: " << error.what() << '\n';
        return 4;
    }
}
```

`CLI11_PARSE` may return from `main` internally for parse errors. If centralized mapping is needed, use CLI11's explicit parse/error handling API instead of the convenience macro.

## 42.11 Conflict resolver usage rules

`ConflictDecision`, `ConflictResolver`, and the `conflict_resolver` field are defined once in Section 12.6; do not redeclare them elsewhere.

Rules:

- Require a non-empty resolver when policy is `prompt`; reject the request otherwise.
- The core invokes the resolver synchronously from the restore thread.
- GUI adapters must marshal the question safely to the GUI thread and block the restore worker (not the GUI) while waiting.
- Support "apply to all" by letting the adapter cache its own decision; the core asks per conflict.
- `cancel` behaves exactly like cooperative cancellation (Section 24.6).

## 42.12 Recommended implementation order inside each component

For every module:

1. Define public behavior and invariants.
2. Write failing unit tests.
3. Implement the smallest correct synchronous version.
4. Add integration tests.
5. Add error context.
6. Add cancellation.
7. Add concurrency only when synchronous correctness is established.
8. Benchmark.
9. Optimize measured bottlenecks.
10. Update documentation and compatibility tests.

## 42.13 Platform lock interface

`src/core/filesystem/platform/platform_lock.hpp`:

```cpp
class RepositoryLock final {
public:
    // Throws LocalVaultError(repository_busy) when another process holds it.
    static RepositoryLock acquire_exclusive(const std::filesystem::path& lock_file);

    ~RepositoryLock();
    RepositoryLock(RepositoryLock&&) noexcept;
    RepositoryLock& operator=(RepositoryLock&&) noexcept;

    RepositoryLock(const RepositoryLock&) = delete;
    RepositoryLock& operator=(const RepositoryLock&) = delete;

private:
    struct Impl;                 // POSIX: fd + flock; Win32: HANDLE + LockFileEx
    std::unique_ptr<Impl> impl_;
};
```

Exactly one of `posix_lock.cpp` / `win32_lock.cpp` is compiled per platform (CMake source selection, Section 10.7 — no `#ifdef` bodies). Both implementations must pass the same lock-contention integration test.

---

End of technical implementation guide.
