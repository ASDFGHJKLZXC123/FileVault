#include "path_safety.hpp"

#include <limits>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "localvault/error.hpp"

namespace localvault {
namespace {

[[nodiscard]] bool components_equal(const std::filesystem::path& left,
                                    const std::filesystem::path& right) {
    const std::wstring& left_text = left.native();
    const std::wstring& right_text = right.native();
    constexpr std::size_t maximum_length =
        static_cast<std::size_t>((std::numeric_limits<int>::max)());
    if (left_text.size() > maximum_length || right_text.size() > maximum_length) {
        return false;
    }
    return ::CompareStringOrdinal(left_text.data(), static_cast<int>(left_text.size()),
                                  right_text.data(), static_cast<int>(right_text.size()),
                                  TRUE) == CSTR_EQUAL;
}

} // namespace

bool platform_path_is_component_prefix(const std::filesystem::path& prefix,
                                       const std::filesystem::path& path) {
    auto prefix_component = prefix.begin();
    auto path_component = path.begin();
    while (prefix_component != prefix.end() && path_component != path.end()) {
        if (!components_equal(*prefix_component, *path_component)) {
            return false;
        }
        ++prefix_component;
        ++path_component;
    }
    return prefix_component == prefix.end();
}

NoFollowPathType inspect_path_no_follow(const std::filesystem::path& path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return NoFollowPathType::not_found;
        }
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to inspect path without following reparse points (Windows "
                              "error " +
                                  std::to_string(error) + ")",
                              path);
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        return NoFollowPathType::indirection;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
        return NoFollowPathType::directory;
    }
    return NoFollowPathType::other;
}

} // namespace localvault
