#pragma once

#include <filesystem>

namespace localvault {

enum class NoFollowPathType {
    not_found,
    directory,
    indirection,
    other,
};

[[nodiscard]] bool platform_path_is_component_prefix(const std::filesystem::path& prefix,
                                                     const std::filesystem::path& path);

[[nodiscard]] NoFollowPathType inspect_path_no_follow(const std::filesystem::path& path);

} // namespace localvault
