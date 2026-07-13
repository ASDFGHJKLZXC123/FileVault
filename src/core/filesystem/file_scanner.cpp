#include "filesystem/file_scanner.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "filesystem/platform/file_metadata.hpp"
#include "localvault/error.hpp"

namespace localvault {
namespace {

[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept {
    const auto byte = [&text](std::size_t index) {
        return static_cast<unsigned char>(text[index]);
    };
    for (std::size_t index = 0; index < text.size();) {
        const unsigned char first = byte(index);
        if (first <= 0x7FU) {
            ++index;
            continue;
        }

        std::size_t length = 0;
        if (first >= 0xC2U && first <= 0xDFU) {
            length = 2;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            length = 3;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            length = 4;
        } else {
            return false;
        }
        if (index + length > text.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset < length; ++offset) {
            if ((byte(index + offset) & 0xC0U) != 0x80U) {
                return false;
            }
        }
        if ((first == 0xE0U && byte(index + 1) < 0xA0U) ||
            (first == 0xEDU && byte(index + 1) > 0x9FU) ||
            (first == 0xF0U && byte(index + 1) < 0x90U) ||
            (first == 0xF4U && byte(index + 1) > 0x8FU)) {
            return false;
        }
        index += length;
    }
    return true;
}

[[nodiscard]] std::optional<std::string> path_to_utf8(const std::filesystem::path& path,
                                                      bool generic) {
    try {
        const std::u8string encoded = generic ? path.generic_u8string() : path.u8string();
        std::string result;
        result.reserve(encoded.size());
        for (const char8_t character : encoded) {
            result.push_back(static_cast<char>(character));
        }
        if (!is_valid_utf8(result)) {
            return std::nullopt;
        }
        return result;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

[[nodiscard]] std::string parent_path(std::string_view relative_path) {
    const std::size_t separator = relative_path.rfind('/');
    return separator == std::string_view::npos ? std::string{}
                                               : std::string(relative_path.substr(0, separator));
}

[[nodiscard]] std::string entry_name(std::string_view relative_path) {
    const std::size_t separator = relative_path.rfind('/');
    return std::string(
        relative_path.substr(separator == std::string_view::npos ? 0 : separator + 1));
}

void add_warning(ScanResult& result, std::string relative_path, std::string code,
                 std::string message) {
    result.warnings.push_back({std::move(relative_path), std::move(code), std::move(message)});
}

[[nodiscard]] std::optional<ScannedEntry> scan_entry(const std::filesystem::path& source_path,
                                                     std::string relative_path,
                                                     const std::filesystem::file_status& status,
                                                     ScanResult& result) {
    EntryType type{};
    if (std::filesystem::is_symlink(status)) {
        type = EntryType::symbolic_link;
    } else if (std::filesystem::is_regular_file(status)) {
        type = EntryType::regular_file;
    } else if (std::filesystem::is_directory(status)) {
        type = EntryType::directory;
    } else {
        add_warning(result, std::move(relative_path), "unsupported_file_type",
                    "special or unknown filesystem entry was skipped");
        return std::nullopt;
    }

    PlatformFileMetadata metadata;
    try {
        metadata = read_platform_file_metadata_no_follow(source_path);
    } catch (const LocalVaultError& error) {
        add_warning(result, std::move(relative_path), "metadata_unavailable", error.what());
        return std::nullopt;
    }

    std::optional<std::string> symlink_target;
    if (type == EntryType::symbolic_link) {
        std::error_code error;
        const std::filesystem::path target = std::filesystem::read_symlink(source_path, error);
        if (error) {
            add_warning(result, std::move(relative_path), "symlink_target_unavailable",
                        "failed to read symbolic-link target: " + error.message());
            return std::nullopt;
        }
        symlink_target = path_to_utf8(target, false);
        if (!symlink_target.has_value()) {
            add_warning(result, std::move(relative_path), "unsupported_path_encoding",
                        "symbolic-link target is not valid UTF-8 and was skipped");
            return std::nullopt;
        }
    }

    return ScannedEntry{
        source_path,
        relative_path,
        parent_path(relative_path),
        entry_name(relative_path),
        type,
        type == EntryType::regular_file ? metadata.logical_size : 0,
        metadata.modified_time_ns,
        metadata.posix_mode,
        std::move(symlink_target),
    };
}

struct Candidate {
    std::filesystem::path source_path;
    std::filesystem::path relative_native_path;
    std::string relative_path;
    std::filesystem::file_status status;
};

void scan_directory(const std::filesystem::path& source_directory,
                    const std::filesystem::path& relative_directory, ScanResult& result) {
    std::error_code error;
    std::filesystem::directory_iterator iterator(source_directory, error);
    if (error) {
        const std::string relative = path_to_utf8(relative_directory, true).value_or(std::string{});
        add_warning(result, relative, "directory_enumeration_failed",
                    "failed to enumerate directory: " + error.message());
        return;
    }

    std::vector<Candidate> candidates;
    const std::filesystem::directory_iterator end;
    while (iterator != end) {
        const std::filesystem::path source_path = iterator->path();
        const std::filesystem::path relative_native_path =
            relative_directory / source_path.filename();
        const std::optional<std::string> relative = path_to_utf8(relative_native_path, true);
        if (!relative.has_value()) {
            add_warning(result, {}, "unsupported_path_encoding",
                        "filesystem entry path is not valid UTF-8 and was skipped");
        } else {
            std::error_code status_error;
            const std::filesystem::file_status status =
                std::filesystem::symlink_status(source_path, status_error);
            if (status_error) {
                add_warning(result, *relative, "filesystem_inspection_failed",
                            "failed to inspect filesystem entry: " + status_error.message());
            } else {
                candidates.push_back({source_path, relative_native_path, *relative, status});
            }
        }

        iterator.increment(error);
        if (error) {
            const std::string directory_relative_path =
                path_to_utf8(relative_directory, true).value_or(std::string{});
            add_warning(result, directory_relative_path, "directory_enumeration_failed",
                        "failed while enumerating directory: " + error.message());
            break;
        }
    }

    std::ranges::sort(candidates, {}, &Candidate::relative_path);
    for (Candidate& candidate : candidates) {
        const std::optional<ScannedEntry> entry =
            scan_entry(candidate.source_path, candidate.relative_path, candidate.status, result);
        if (!entry.has_value()) {
            continue;
        }
        result.entries.push_back(*entry);
        if (entry->type == EntryType::directory) {
            scan_directory(candidate.source_path, candidate.relative_native_path, result);
        }
    }
}

} // namespace

ScanResult FileScanner::scan(const std::filesystem::path& source_root) const {
    if (source_root.empty()) {
        throw LocalVaultError(ErrorCode::invalid_argument, "source root path must not be empty");
    }
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(source_root, error);
    if (error) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to make source path absolute: " + error.message(),
                              source_root);
    }
    const std::filesystem::path normalized = absolute.lexically_normal();
    const std::filesystem::file_status root_status =
        std::filesystem::symlink_status(normalized, error);
    if (error) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to inspect source root: " + error.message(), normalized);
    }
    if (!std::filesystem::is_directory(root_status) || std::filesystem::is_symlink(root_status)) {
        throw LocalVaultError(ErrorCode::invalid_argument,
                              "source root must be a directory and not a symbolic link",
                              normalized);
    }

    ScanResult result;
    const std::optional<ScannedEntry> root = scan_entry(normalized, {}, root_status, result);
    if (!root.has_value()) {
        throw LocalVaultError(ErrorCode::filesystem_error, "failed to capture source-root metadata",
                              normalized);
    }
    result.entries.push_back(*root);
    scan_directory(normalized, {}, result);
    std::ranges::sort(result.warnings, [](const ScanWarning& left, const ScanWarning& right) {
        if (left.relative_path != right.relative_path) {
            return left.relative_path < right.relative_path;
        }
        if (left.code != right.code) {
            return left.code < right.code;
        }
        return left.message < right.message;
    });
    return result;
}

} // namespace localvault
