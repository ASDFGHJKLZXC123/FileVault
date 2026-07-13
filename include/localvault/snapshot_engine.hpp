#pragma once

#include "localvault/progress.hpp"
#include "localvault/types.hpp"

#include <cstddef>
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

    SnapshotResult create_snapshot(const std::filesystem::path& source_root,
                                   const SnapshotOptions& options, std::stop_token stop_token = {},
                                   ProgressCallback progress = {});

  private:
    Repository& repository_;
};

} // namespace localvault
