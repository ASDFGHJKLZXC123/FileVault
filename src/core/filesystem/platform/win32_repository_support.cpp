#include "repository_support.hpp"

#include <algorithm>
#include <limits>
#include <string_view>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <bcrypt.h>

#include "localvault/error.hpp"

#pragma comment(lib, "bcrypt.lib")

namespace localvault {

std::optional<std::string> platform_filesystem_name(const std::filesystem::path& existing_path) {
    wchar_t volume_path[MAX_PATH]{};
    if (::GetVolumePathNameW(existing_path.c_str(), volume_path, MAX_PATH) == 0) {
        return std::nullopt;
    }
    if (::GetDriveTypeW(volume_path) == DRIVE_REMOTE) {
        return "drive_remote";
    }

    wchar_t filesystem_name[MAX_PATH]{};
    if (::GetVolumeInformationW(volume_path, nullptr, 0, nullptr, nullptr, nullptr, filesystem_name,
                                MAX_PATH) == 0) {
        return std::nullopt;
    }
    std::wstring_view wide_name(filesystem_name);
    std::string name;
    name.reserve(wide_name.size());
    for (const wchar_t character : wide_name) {
        if (character > static_cast<wchar_t>((std::numeric_limits<unsigned char>::max)())) {
            return std::nullopt;
        }
        name.push_back(static_cast<char>(character));
    }
    return name;
}

std::array<std::uint8_t, 16> secure_random_uuid_bytes() {
    std::array<std::uint8_t, 16> bytes{};
    const NTSTATUS result = ::BCryptGenRandom(
        nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (result < 0) {
        throw LocalVaultError(ErrorCode::internal_error,
                              "secure random number generation failed (BCrypt status " +
                                  std::to_string(static_cast<long>(result)) + ")");
    }
    return bytes;
}

void apply_restrictive_repository_permissions(const std::filesystem::path&) {
    // Repository directories inherit the creating user's Windows ACL.
}

void apply_restrictive_file_permissions(const std::filesystem::path&) {
    // Repository files inherit the creating user's Windows ACL.
}

void create_exclusive_file(const std::filesystem::path& path) {
    const HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to exclusively create repository file (Windows error " +
                                  std::to_string(::GetLastError()) + ")",
                              path);
    }
    (void)::CloseHandle(handle);
}

bool repository_storage_is_proven_read_only(const std::filesystem::path& root) noexcept {
    wchar_t volume_path[MAX_PATH]{};
    if (::GetVolumePathNameW(root.c_str(), volume_path, MAX_PATH) == 0) {
        return false;
    }
    DWORD flags = 0;
    if (::GetVolumeInformationW(volume_path, nullptr, 0, nullptr, nullptr, &flags, nullptr, 0) ==
        0) {
        return false;
    }
    return (flags & FILE_READ_ONLY_VOLUME) != 0;
}

void flush_containing_directory(const std::filesystem::path&) {
    // SQLite FULL synchronous mode flushes the database. Windows does not offer a
    // generally usable directory fsync equivalent.
}

} // namespace localvault
