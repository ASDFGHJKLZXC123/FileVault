#include "filesystem/file_scanner.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "filesystem/ignore_rules.hpp"
#include "filesystem/platform/file_metadata.hpp"
#include "localvault/error.hpp"

namespace localvault {

ScanEntryDecision decide_scan_entry(const ScanEntryFacts& facts,
                                    const FileScannerOptions& options) noexcept {
    if (!options.include_hidden && facts.hidden) {
        return {ScanEntryAction::skip, {}};
    }
    if (facts.cloud_placeholder) {
        return {ScanEntryAction::warn_and_skip, "cloud_placeholder"};
    }
    if (facts.kind == ScanEntryKind::volume_mount_point) {
        return {ScanEntryAction::warn_and_skip, "volume_mount_point"};
    }
    if (facts.kind == ScanEntryKind::unsupported_reparse_point) {
        return {ScanEntryAction::warn_and_skip, "unsupported_reparse_point"};
    }
    if (options.one_file_system && facts.filesystem_boundary) {
        return {ScanEntryAction::warn_and_skip, "filesystem_boundary"};
    }
    if (facts.kind == ScanEntryKind::symbolic_link || facts.kind == ScanEntryKind::junction) {
        return {ScanEntryAction::capture_as_symbolic_link, {}};
    }
    return {ScanEntryAction::capture, {}};
}

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

void check_cancelled(std::stop_token stop_token, const std::filesystem::path& path) {
    if (stop_token.stop_requested()) {
        throw LocalVaultError(ErrorCode::cancelled, "snapshot cancelled", path);
    }
}

void add_warning(const FileScanner::WarningCallback& on_warning, std::string relative_path,
                 std::string code, std::string message) {
    on_warning({std::move(relative_path), std::move(code), std::move(message)});
}

[[nodiscard]] std::optional<ScannedEntry>
scan_entry(const std::filesystem::path& source_path, std::string relative_path,
           const std::filesystem::file_status& status, bool capture_as_symbolic_link,
           const FileScanner::WarningCallback& on_warning) {
    EntryType type{};
    if (capture_as_symbolic_link || std::filesystem::is_symlink(status)) {
        type = EntryType::symbolic_link;
    } else if (std::filesystem::is_regular_file(status)) {
        type = EntryType::regular_file;
    } else if (std::filesystem::is_directory(status)) {
        type = EntryType::directory;
    } else {
        add_warning(on_warning, std::move(relative_path), "unsupported_file_type",
                    "special or unknown filesystem entry was skipped");
        return std::nullopt;
    }

    PlatformFileMetadata metadata;
    try {
        metadata = read_platform_file_metadata_no_follow(source_path);
    } catch (const LocalVaultError& error) {
        add_warning(on_warning, std::move(relative_path), "metadata_unavailable", error.what());
        return std::nullopt;
    }

    std::optional<std::string> symlink_target;
    if (type == EntryType::symbolic_link) {
        std::error_code error;
        const std::filesystem::path target = std::filesystem::read_symlink(source_path, error);
        if (error) {
            add_warning(on_warning, std::move(relative_path), "symlink_target_unavailable",
                        "failed to read symbolic-link target: " + error.message());
            return std::nullopt;
        }
        symlink_target = path_to_utf8(target, false);
        if (!symlink_target.has_value()) {
            add_warning(on_warning, std::move(relative_path), "unsupported_path_encoding",
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

[[nodiscard]] ScanEntryKind scan_entry_kind(const std::filesystem::file_status& status,
                                            const ScannerPlatformMetadata& metadata) noexcept {
    switch (metadata.reparse_kind) {
    case ScannerReparseKind::symbolic_link:
        return ScanEntryKind::symbolic_link;
    case ScannerReparseKind::junction:
        return ScanEntryKind::junction;
    case ScannerReparseKind::volume_mount_point:
        return ScanEntryKind::volume_mount_point;
    case ScannerReparseKind::other:
        return ScanEntryKind::unsupported_reparse_point;
    case ScannerReparseKind::none:
        return std::filesystem::is_symlink(status) ? ScanEntryKind::symbolic_link
                                                   : ScanEntryKind::ordinary;
    }
    return ScanEntryKind::ordinary;
}

[[nodiscard]] std::string_view warning_message(std::string_view code) noexcept {
    if (code == "cloud_placeholder") {
        return "cloud placeholder was skipped to avoid downloading content";
    }
    if (code == "volume_mount_point") {
        return "Windows volume mount point was skipped";
    }
    if (code == "unsupported_reparse_point") {
        return "unsupported Windows reparse point was skipped";
    }
    return "filesystem boundary was skipped";
}

void scan_directory(const std::filesystem::path& source_directory,
                    const std::filesystem::path& relative_directory, std::stop_token stop_token,
                    std::uint64_t root_filesystem_identity, const IgnoreRules& ignore_rules,
                    const FileScannerOptions& options, const FileScanner::EntryCallback& on_entry,
                    const FileScanner::WarningCallback& on_warning) {
    check_cancelled(stop_token, source_directory);
    std::error_code error;
    std::filesystem::directory_iterator iterator(source_directory, error);
    if (error) {
        const std::string relative = path_to_utf8(relative_directory, true).value_or(std::string{});
        add_warning(on_warning, relative, "directory_enumeration_failed",
                    "failed to enumerate directory: " + error.message());
        return;
    }

    const std::filesystem::directory_iterator end;
    while (iterator != end) {
        check_cancelled(stop_token, source_directory);
        const std::filesystem::path source_path = iterator->path();
        const std::filesystem::path relative_native_path =
            relative_directory / source_path.filename();
        const std::optional<std::string> relative = path_to_utf8(relative_native_path, true);
        if (!relative.has_value()) {
            add_warning(on_warning, {}, "unsupported_path_encoding",
                        "filesystem entry path is not valid UTF-8 and was skipped");
        } else {
            std::error_code status_error;
            const std::filesystem::file_status status =
                std::filesystem::symlink_status(source_path, status_error);
            if (status_error) {
                add_warning(on_warning, *relative, "filesystem_inspection_failed",
                            "failed to inspect filesystem entry: " + status_error.message());
            } else {
                std::optional<ScannerPlatformMetadata> platform;
                try {
                    platform = read_scanner_platform_metadata_no_follow(source_path);
                } catch (const LocalVaultError& metadata_error) {
                    add_warning(on_warning, *relative, "metadata_unavailable",
                                metadata_error.what());
                }
                if (platform.has_value()) {
                    const ScanEntryKind kind = scan_entry_kind(status, *platform);
                    const bool directory_like = std::filesystem::is_directory(status) ||
                                                kind == ScanEntryKind::junction ||
                                                kind == ScanEntryKind::volume_mount_point;
                    if (!ignore_rules.match(*relative, directory_like).ignored) {
                        const ScanEntryDecision decision = decide_scan_entry(
                            {
                                kind,
                                platform->hidden,
                                platform->cloud_placeholder,
                                platform->filesystem_identity != root_filesystem_identity,
                            },
                            options);
                        if (decision.action == ScanEntryAction::warn_and_skip) {
                            add_warning(on_warning, *relative, std::string(decision.warning_code),
                                        std::string(warning_message(decision.warning_code)));
                        } else if (decision.action != ScanEntryAction::skip) {
                            std::optional<ScannedEntry> entry = scan_entry(
                                source_path, *relative, status,
                                decision.action == ScanEntryAction::capture_as_symbolic_link,
                                on_warning);
                            if (entry.has_value()) {
                                const bool is_directory = entry->type == EntryType::directory;
                                on_entry(std::move(*entry));
                                if (is_directory) {
                                    scan_directory(source_path, relative_native_path, stop_token,
                                                   root_filesystem_identity, ignore_rules, options,
                                                   on_entry, on_warning);
                                }
                            }
                        }
                    }
                }
            }
        }

        iterator.increment(error);
        if (error) {
            const std::string directory_relative_path =
                path_to_utf8(relative_directory, true).value_or(std::string{});
            add_warning(on_warning, directory_relative_path, "directory_enumeration_failed",
                        "failed while enumerating directory: " + error.message());
            break;
        }
    }
}

} // namespace

ScanResult FileScanner::scan(const std::filesystem::path& source_root,
                             const FileScannerOptions& options) const {
    ScanResult result;
    scan_streaming(
        source_root, {},
        [&result](ScannedEntry entry) { result.entries.push_back(std::move(entry)); },
        [&result](ScanWarning warning) { result.warnings.push_back(std::move(warning)); }, options);
    std::ranges::sort(result.entries, {}, &ScannedEntry::relative_path);
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

void FileScanner::scan_streaming(const std::filesystem::path& source_root,
                                 std::stop_token stop_token, const EntryCallback& on_entry,
                                 const WarningCallback& on_warning,
                                 const FileScannerOptions& options) const {
    if (source_root.empty()) {
        throw LocalVaultError(ErrorCode::invalid_argument, "source root path must not be empty");
    }
    if (!on_entry || !on_warning) {
        throw LocalVaultError(ErrorCode::invalid_argument,
                              "streaming scan requires entry and warning callbacks");
    }
    check_cancelled(stop_token, source_root);
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

    const ScannerPlatformMetadata root_platform =
        read_scanner_platform_metadata_no_follow(normalized);
    if (root_platform.reparse_kind != ScannerReparseKind::none || root_platform.cloud_placeholder) {
        throw LocalVaultError(ErrorCode::invalid_argument,
                              "source root must not be a reparse point or cloud placeholder",
                              normalized);
    }
    const IgnoreRules ignore_rules = IgnoreRules::load(normalized, options.ignore_file,
                                                       scanner_paths_are_case_sensitive(normalized)
                                                           ? IgnoreCaseSensitivity::sensitive
                                                           : IgnoreCaseSensitivity::insensitive);

    const std::optional<ScannedEntry> root =
        scan_entry(normalized, {}, root_status, false, on_warning);
    if (!root.has_value()) {
        throw LocalVaultError(ErrorCode::filesystem_error, "failed to capture source-root metadata",
                              normalized);
    }
    on_entry(*root);
    scan_directory(normalized, {}, stop_token, root_platform.filesystem_identity, ignore_rules,
                   options, on_entry, on_warning);
}

} // namespace localvault
