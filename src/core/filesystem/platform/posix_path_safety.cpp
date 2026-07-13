#include "path_safety.hpp"

#include <cerrno>
#include <system_error>

#include <sys/stat.h>

#include "localvault/error.hpp"

namespace localvault {

bool platform_path_is_component_prefix(const std::filesystem::path& prefix,
                                       const std::filesystem::path& path) {
    auto prefix_component = prefix.begin();
    auto path_component = path.begin();
    while (prefix_component != prefix.end() && path_component != path.end()) {
        if (*prefix_component != *path_component) {
            return false;
        }
        ++prefix_component;
        ++path_component;
    }
    return prefix_component == prefix.end();
}

NoFollowPathType inspect_path_no_follow(const std::filesystem::path& path) {
    struct stat information{};
    if (::lstat(path.c_str(), &information) != 0) {
        const int error = errno;
        if (error == ENOENT || error == ENOTDIR) {
            return NoFollowPathType::not_found;
        }
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to inspect path without following links: " +
                                  std::generic_category().message(error),
                              path);
    }
    if (S_ISLNK(information.st_mode)) {
        return NoFollowPathType::indirection;
    }
    if (S_ISDIR(information.st_mode)) {
        return NoFollowPathType::directory;
    }
    return NoFollowPathType::other;
}

} // namespace localvault
