#pragma once

#include <filesystem>
#include <string_view>

namespace localvault {

enum class FilesystemClass {
    local,
    network,
    fat,
    unknown,
};

struct FilesystemPolicy {
    bool allowed;
    bool warn;
};

[[nodiscard]] FilesystemClass classify_filesystem_name(std::string_view name);
[[nodiscard]] FilesystemClass
classify_destination_filesystem(const std::filesystem::path& destination);
[[nodiscard]] FilesystemPolicy filesystem_policy(FilesystemClass classification,
                                                 bool allow_risky_filesystem) noexcept;

} // namespace localvault
