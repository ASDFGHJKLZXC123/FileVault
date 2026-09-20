#pragma once

#include <cstddef>
#include <stop_token>

#include "localvault/progress.hpp"
#include "localvault/types.hpp"

namespace localvault {

class Repository;

struct GarbageCollectionOptions {
    bool dry_run{true};
    std::size_t batch_size{1'000};
};

struct GarbageCollectionResult {
    std::uint64_t unreferenced_chunks{};
    std::uint64_t orphan_objects{};
    std::uint64_t stale_temporary_files{};
    std::uint64_t stale_snapshots{};
    std::uint64_t ignored_paths{};
    ByteCount reclaimable_bytes{};
    std::uint64_t removed_objects{};
    ByteCount reclaimed_bytes{};
};

class GarbageCollector final {
  public:
    explicit GarbageCollector(Repository& repository) noexcept;

    // Retains content objects; explicit collect() is required to reclaim them.
    void delete_snapshot(SnapshotId id, std::stop_token stop_token = {});
    GarbageCollectionResult collect(const GarbageCollectionOptions& options = {},
                                    std::stop_token stop_token = {},
                                    ProgressCallback progress = {});

  private:
    Repository& repository_;
};

} // namespace localvault
