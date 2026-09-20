#pragma once

#include "localvault/types.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace localvault {

class Repository;

enum class DiffKind {
    added,
    removed,
    type_changed,
    content_modified,
    metadata_modified,
    unchanged
};

struct DiffEntry {
    std::filesystem::path relative_path;
    DiffKind kind{};
    std::optional<EntryInfo> before;
    std::optional<EntryInfo> after;
};

struct DiffOptions {
    bool compare_metadata{true};
    bool ignore_directory_mtime{true};
    bool include_unchanged{true};
};

using DiffCallback = std::function<void(const DiffEntry&)>;

struct DiffPage {
    std::vector<DiffEntry> entries;
    // Pass this UTF-8 key as after_path to request the next page with the same options.
    std::optional<std::string> next_cursor;
};

class DiffEngine final {
  public:
    explicit DiffEngine(Repository& repository) noexcept : repository_(repository) {}

    // Callbacks run inside a read transaction and must not reenter this repository connection.
    void diff(SnapshotId before_id, SnapshotId after_id, const DiffOptions& options,
              DiffCallback callback, std::stop_token stop_token = {}) const;

    [[nodiscard]] DiffPage diff_page(SnapshotId before_id, SnapshotId after_id,
                                     const DiffOptions& options = {}, std::size_t page_size = 1000,
                                     std::string_view after_path = {},
                                     std::stop_token stop_token = {}) const;

  private:
    Repository& repository_;
};

} // namespace localvault
