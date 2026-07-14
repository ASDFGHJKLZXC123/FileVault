#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
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

struct FileScannerOptions {
    bool include_hidden{true};
    bool one_file_system{false};
    std::optional<std::filesystem::path> ignore_file;
};

enum class ScanEntryKind {
    ordinary,
    symbolic_link,
    junction,
    volume_mount_point,
    unsupported_reparse_point,
};

struct ScanEntryFacts {
    ScanEntryKind kind{ScanEntryKind::ordinary};
    bool hidden{false};
    bool cloud_placeholder{false};
    bool filesystem_boundary{false};
};

enum class ScanEntryAction {
    capture,
    capture_as_symbolic_link,
    skip,
    warn_and_skip,
};

struct ScanEntryDecision {
    ScanEntryAction action{ScanEntryAction::capture};
    std::string_view warning_code;
};

[[nodiscard]] ScanEntryDecision decide_scan_entry(const ScanEntryFacts& facts,
                                                  const FileScannerOptions& options) noexcept;

class FileScanner final {
  public:
    using EntryCallback = std::function<void(ScannedEntry)>;
    using WarningCallback = std::function<void(ScanWarning)>;

    [[nodiscard]] ScanResult scan(const std::filesystem::path& source_root,
                                  const FileScannerOptions& options = {}) const;
    void scan_streaming(const std::filesystem::path& source_root, std::stop_token stop_token,
                        const EntryCallback& on_entry, const WarningCallback& on_warning,
                        const FileScannerOptions& options = {}) const;
};

} // namespace localvault
