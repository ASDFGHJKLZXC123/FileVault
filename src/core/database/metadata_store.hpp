#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "localvault/types.hpp"

namespace localvault {

class Database;
class FailureInjector;
class ObjectStore;
struct ScannedEntry;

struct IncompleteSnapshotInfo {
    SnapshotId id{};
    SnapshotStatus status{SnapshotStatus::pending};
};

struct SnapshotTotals {
    std::uint64_t file_count{};
    std::uint64_t directory_count{};
    std::uint64_t symlink_count{};
    ByteCount logical_size{};
    ByteCount new_stored_size{};
    std::uint64_t new_chunk_count{};
    std::uint64_t reused_chunk_count{};
    std::int64_t duration_ms{};
};

struct StoredChunkInfo {
    std::string hash_hex;
    ByteCount raw_size{};
    ByteCount stored_size{};
    std::filesystem::path object_path;
    std::int64_t created_at_ns{};
};

struct ChunkReferenceInfo {
    std::int64_t sequence_number{};
    std::string hash_hex;
    std::filesystem::path object_path;
    ByteCount raw_offset{};
    ByteCount raw_length{};
    ByteCount raw_size{};
    ByteCount stored_size{};
};

class MetadataStore final {
  public:
    explicit MetadataStore(Database& database) noexcept : database_(database) {}

    [[nodiscard]] SnapshotId create_pending_snapshot(std::string_view source_root,
                                                     std::string_view message,
                                                     std::int64_t created_at_ns) const;
    void mark_snapshot_complete(SnapshotId snapshot_id, const SnapshotTotals& totals,
                                std::int64_t completed_at_ns) const;
    void mark_snapshot_incomplete(SnapshotId snapshot_id, SnapshotStatus status,
                                  std::string_view failure_message,
                                  std::int64_t completed_at_ns) const;
    [[nodiscard]] SnapshotSummary require_complete_snapshot(SnapshotId snapshot_id) const;
    [[nodiscard]] std::vector<SnapshotSummary> list_complete_snapshots() const;

    void transition_snapshot_to_deleting(SnapshotId snapshot_id,
                                         FailureInjector& failure_injector) const;
    void delete_deleting_snapshot(SnapshotId snapshot_id, FailureInjector& failure_injector,
                                  std::size_t entry_batch_limit = 10'000) const;
    [[nodiscard]] std::vector<IncompleteSnapshotInfo> list_incomplete_snapshots() const;
    void mark_stale_pending_snapshot_failed(SnapshotId snapshot_id,
                                            std::string_view failure_message,
                                            std::int64_t completed_at_ns,
                                            FailureInjector& failure_injector) const;
    void clean_incomplete_snapshot(SnapshotId snapshot_id, FailureInjector& failure_injector,
                                   std::size_t entry_batch_limit = 10'000) const;
    void quick_relationship_check() const;

    [[nodiscard]] std::int64_t insert_entry(SnapshotId snapshot_id,
                                            const ScannedEntry& entry) const;
    void set_regular_file_hash(std::int64_t entry_id, std::string_view file_hash_hex) const;
    [[nodiscard]] std::optional<StoredChunkInfo> find_chunk(std::string_view hash_hex) const;
    void insert_entry_chunk(std::int64_t entry_id, std::int64_t sequence_number,
                            std::string_view chunk_hash, ByteCount raw_offset,
                            ByteCount raw_length) const;
    void insert_warning(SnapshotId snapshot_id, std::string_view relative_path,
                        std::string_view warning_code, std::string_view message) const;
    [[nodiscard]] std::vector<EntryInfo> list_entries(SnapshotId snapshot_id) const;
    [[nodiscard]] std::vector<EntryInfo> list_children(SnapshotId snapshot_id,
                                                       std::string_view parent_path) const;
    [[nodiscard]] std::vector<ChunkReferenceInfo> list_entry_chunks(std::int64_t entry_id) const;

  private:
    friend class ObjectStore;

    void ensure_chunk(const StoredChunkInfo& chunk) const;

    Database& database_;
};

} // namespace localvault
