#include "filesystem/filesystem_classifier.hpp"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <optional>
#include <string>

#include "filesystem/platform/repository_support.hpp"

namespace localvault {
namespace {

[[nodiscard]] std::string lowercase(std::string_view value) {
    std::string result(value);
    std::ranges::transform(result, result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

[[nodiscard]] bool is_one_of(std::string_view value,
                             std::initializer_list<std::string_view> choices) {
    return std::ranges::find(choices, value) != choices.end();
}

[[nodiscard]] std::filesystem::path nearest_existing_ancestor(std::filesystem::path path) {
    std::error_code error;
    while (!path.empty()) {
        if (std::filesystem::exists(path, error)) {
            return path;
        }
        error.clear();
        const std::filesystem::path parent = path.parent_path();
        if (parent == path) {
            break;
        }
        path = parent;
    }
    return {};
}

} // namespace

FilesystemClass classify_filesystem_name(std::string_view name) {
    const std::string normalized = lowercase(name);
    if (is_one_of(normalized, {"nfs", "nfs4", "smb", "smb2", "smb3", "smbfs", "cifs", "9p", "afs",
                               "coda", "ncp", "ncpfs", "davfs", "davfs2", "drive_remote"}) ||
        normalized.find("sshfs") != std::string::npos ||
        is_one_of(normalized, {"fuse.rclone", "fuse.s3fs", "fuse.gcsfuse"})) {
        return FilesystemClass::network;
    }
    if (is_one_of(normalized,
                  {"fat", "fat12", "fat16", "fat32", "vfat", "msdos", "msdosfs", "exfat"})) {
        return FilesystemClass::fat;
    }
    if (is_one_of(normalized,
                  {"ext2", "ext3", "ext4", "xfs", "btrfs", "zfs", "apfs", "hfs", "hfsplus", "ufs",
                   "ntfs", "refs", "tmpfs", "ramfs", "overlay", "aufs"})) {
        return FilesystemClass::local;
    }
    return FilesystemClass::unknown;
}

FilesystemClass classify_destination_filesystem(const std::filesystem::path& destination) {
    const std::filesystem::path existing = nearest_existing_ancestor(destination);
    if (existing.empty()) {
        return FilesystemClass::unknown;
    }
    const std::optional<std::string> name = platform_filesystem_name(existing);
    return name.has_value() ? classify_filesystem_name(*name) : FilesystemClass::unknown;
}

FilesystemPolicy filesystem_policy(FilesystemClass classification,
                                   bool allow_risky_filesystem) noexcept {
    if (classification == FilesystemClass::network) {
        return {.allowed = allow_risky_filesystem, .warn = false};
    }
    if (classification == FilesystemClass::fat) {
        return {.allowed = true, .warn = true};
    }
    return {.allowed = true, .warn = false};
}

} // namespace localvault
