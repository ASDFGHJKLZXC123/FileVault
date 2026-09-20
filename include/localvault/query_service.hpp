#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

#include "localvault/types.hpp"

namespace localvault {

class Repository;

struct PageRequest {
    std::uint64_t offset{};
    // Limits outside 1..10,000 and offset + limit above INT64_MAX are rejected.
    std::uint32_t limit{200};
};

template <typename T> struct Page {
    std::vector<T> items;
    std::uint64_t total_count{};
};

class QueryService final {
  public:
    explicit QueryService(Repository& repository);

    [[nodiscard]] Page<SnapshotSummary> list_snapshots(PageRequest page = {}) const;
    [[nodiscard]] SnapshotSummary get_snapshot(SnapshotId id) const;
    [[nodiscard]] Page<EntryInfo> list_children(SnapshotId id, const std::filesystem::path& parent,
                                                PageRequest page = {}) const;
    // Case-sensitive literal substring matching; an empty query matches all entries.
    [[nodiscard]] Page<EntryInfo> search_paths(SnapshotId id, std::string_view query,
                                               PageRequest page = {}) const;
    [[nodiscard]] RepositoryStats repository_stats() const;

  private:
    Repository& repository_;
};

} // namespace localvault
