#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "localvault/types.hpp"

namespace localvault {

struct ScannedEntry {
    std::filesystem::path source_path;
    std::string relative_path;
    std::string parent_path;
    std::string name;
    EntryType type{EntryType::regular_file};
    ByteCount logical_size{};
    std::int64_t modified_time_ns{};
    std::uint32_t posix_mode{};
    std::optional<std::string> symlink_target;
};

struct ScanWarning {
    std::string relative_path;
    std::string code;
    std::string message;
};

struct ScanResult {
    std::vector<ScannedEntry> entries;
    std::vector<ScanWarning> warnings;
};

class FileScanner final {
  public:
    [[nodiscard]] ScanResult scan(const std::filesystem::path& source_root) const;
};

} // namespace localvault
