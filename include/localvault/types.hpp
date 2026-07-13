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

enum class EntryType { regular_file, directory, symbolic_link };

enum class SnapshotStatus { pending, complete, failed, cancelled, deleting };

enum class OverwritePolicy { never, prompt, always };

enum class VerifyMode { quick, full };

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

} // namespace localvault
