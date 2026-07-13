#pragma once

#include "localvault/progress.hpp"
#include "localvault/types.hpp"

#include <filesystem>
#include <functional>
#include <stop_token>
#include <vector>

namespace localvault {

class Repository;

enum class ConflictDecision { skip, replace, cancel };

using ConflictResolver = std::function<ConflictDecision(const std::filesystem::path& destination,
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

    RestoreResult restore(const RestoreRequest& request, std::stop_token stop_token = {},
                          ProgressCallback progress = {});

  private:
    Repository& repository_;
};

} // namespace localvault
