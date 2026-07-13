#include "file_metadata.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "localvault/error.hpp"
#include "repository_support.hpp"

namespace localvault {
namespace {

class FileHandle final {
  public:
    explicit FileHandle(HANDLE handle) noexcept : handle_(handle) {}

    ~FileHandle() noexcept {
        if (handle_ != INVALID_HANDLE_VALUE) {
            (void)::CloseHandle(handle_);
        }
    }

    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

  private:
    HANDLE handle_;
};

[[nodiscard]] std::int64_t unix_time_nanoseconds(const FILETIME& time,
                                                 const std::filesystem::path& path) {
    constexpr std::uint64_t windows_to_unix_epoch_ticks = 116'444'736'000'000'000ULL;
    constexpr std::uint64_t nanoseconds_per_tick = 100;
    const std::uint64_t ticks = (static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) |
                                static_cast<std::uint64_t>(time.dwLowDateTime);
    constexpr std::uint64_t maximum =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
    if (ticks >= windows_to_unix_epoch_ticks) {
        const std::uint64_t unix_ticks = ticks - windows_to_unix_epoch_ticks;
        if (unix_ticks > maximum / nanoseconds_per_tick) {
            throw LocalVaultError(ErrorCode::filesystem_error,
                                  "file modification time is outside the supported range", path);
        }
        return static_cast<std::int64_t>(unix_ticks * nanoseconds_per_tick);
    }

    const std::uint64_t ticks_before_epoch = windows_to_unix_epoch_ticks - ticks;
    constexpr std::uint64_t minimum_magnitude = maximum + 1U;
    if (ticks_before_epoch > minimum_magnitude / nanoseconds_per_tick) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "file modification time is outside the supported range", path);
    }
    const std::uint64_t magnitude = ticks_before_epoch * nanoseconds_per_tick;
    if (magnitude == minimum_magnitude) {
        return (std::numeric_limits<std::int64_t>::min)();
    }
    return -static_cast<std::int64_t>(magnitude);
}

[[nodiscard]] FILETIME restored_filetime(std::int64_t modified_time_ns) {
    constexpr std::uint64_t windows_to_unix_epoch_ticks = 116'444'736'000'000'000ULL;
    constexpr std::int64_t nanoseconds_per_tick = 100;
    const std::int64_t unix_ticks = modified_time_ns / nanoseconds_per_tick;
    std::uint64_t windows_ticks = windows_to_unix_epoch_ticks;
    if (unix_ticks >= 0) {
        windows_ticks += static_cast<std::uint64_t>(unix_ticks);
    } else {
        windows_ticks -= static_cast<std::uint64_t>(-(unix_ticks + 1)) + 1U;
    }
    FILETIME result{};
    result.dwLowDateTime = static_cast<DWORD>(windows_ticks & 0xFFFFFFFFULL);
    result.dwHighDateTime = static_cast<DWORD>(windows_ticks >> 32U);
    return result;
}

[[nodiscard]] std::string random_temporary_name() {
    const std::array<std::uint8_t, 16> bytes = secure_random_uuid_bytes();
    constexpr std::string_view hexadecimal = "0123456789abcdef";
    std::string name = ".localvault-restore-";
    name.reserve(name.size() + bytes.size() * 2U + 4U);
    for (const std::uint8_t byte : bytes) {
        name.push_back(hexadecimal[byte >> 4U]);
        name.push_back(hexadecimal[byte & 0x0FU]);
    }
    name += ".tmp";
    return name;
}

} // namespace

struct TemporaryOutputFile::Impl {
    Impl(HANDLE file_handle, std::filesystem::path temporary_path)
        : handle(file_handle), path(std::move(temporary_path)) {}

    ~Impl() noexcept {
        if (handle != INVALID_HANDLE_VALUE) {
            (void)::CloseHandle(handle);
        }
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    HANDLE handle{INVALID_HANDLE_VALUE};
    std::filesystem::path path;
};

TemporaryOutputFile::TemporaryOutputFile(const std::filesystem::path& directory) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        const std::filesystem::path candidate = directory / random_temporary_name();
        const HANDLE handle =
            ::CreateFileW(candidate.c_str(), GENERIC_WRITE | FILE_WRITE_ATTRIBUTES,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                          CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            impl_ = std::make_unique<Impl>(handle, candidate);
            return;
        }
        const DWORD error = ::GetLastError();
        if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS) {
            throw LocalVaultError(ErrorCode::filesystem_error,
                                  "failed to create restore temporary file (Windows error " +
                                      std::to_string(error) + ")",
                                  candidate);
        }
    }
    throw LocalVaultError(ErrorCode::filesystem_error,
                          "failed to choose a unique restore temporary file", directory);
}

TemporaryOutputFile::~TemporaryOutputFile() noexcept = default;
TemporaryOutputFile::TemporaryOutputFile(TemporaryOutputFile&&) noexcept = default;
TemporaryOutputFile& TemporaryOutputFile::operator=(TemporaryOutputFile&&) noexcept = default;

const std::filesystem::path& TemporaryOutputFile::path() const noexcept {
    return impl_->path;
}

void TemporaryOutputFile::write(std::span<const std::byte> bytes) {
    std::size_t offset = 0;
    constexpr std::size_t maximum_write = 64U * 1024U;
    while (offset < bytes.size()) {
        const std::size_t count = (std::min)(maximum_write, bytes.size() - offset);
        DWORD written = 0;
        if (::WriteFile(impl_->handle, bytes.data() + offset, static_cast<DWORD>(count), &written,
                        nullptr) == 0) {
            throw LocalVaultError(ErrorCode::filesystem_error,
                                  "failed to write restore temporary file (Windows error " +
                                      std::to_string(::GetLastError()) + ")",
                                  impl_->path);
        }
        if (written == 0) {
            throw LocalVaultError(ErrorCode::filesystem_error,
                                  "failed to write restore temporary file: no bytes written",
                                  impl_->path);
        }
        offset += static_cast<std::size_t>(written);
    }
}

void TemporaryOutputFile::apply_metadata(std::int64_t modified_time_ns, std::uint32_t) {
    FILETIME modified_time = restored_filetime(modified_time_ns);
    if (::SetFileTime(impl_->handle, nullptr, nullptr, &modified_time) == 0) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to restore modification time (Windows error " +
                                  std::to_string(::GetLastError()) + ")",
                              impl_->path);
    }
}

void TemporaryOutputFile::sync() {
    if (::FlushFileBuffers(impl_->handle) == 0) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to sync restored temporary file (Windows error " +
                                  std::to_string(::GetLastError()) + ")",
                              impl_->path);
    }
}

RestorePublishResult TemporaryOutputFile::publish(const std::filesystem::path& destination_path,
                                                  bool replace_existing) {
    return publish_restored_path(impl_->path, destination_path, replace_existing);
}

PlatformFileMetadata read_platform_file_metadata_no_follow(const std::filesystem::path& path) {
    FileHandle handle(::CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to read file metadata without following reparse points "
                              "(Windows error " +
                                  std::to_string(::GetLastError()) + ")",
                              path);
    }

    BY_HANDLE_FILE_INFORMATION information{};
    if (::GetFileInformationByHandle(handle.get(), &information) == 0) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to query file metadata (Windows error " +
                                  std::to_string(::GetLastError()) + ")",
                              path);
    }
    const std::uint64_t size = (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32U) |
                               static_cast<std::uint64_t>(information.nFileSizeLow);
    return {size, unix_time_nanoseconds(information.ftLastWriteTime, path), 0};
}

void apply_restored_metadata(const std::filesystem::path& path, std::int64_t modified_time_ns,
                             std::uint32_t) {
    FileHandle handle(::CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to open restored path for metadata (Windows error " +
                                  std::to_string(::GetLastError()) + ")",
                              path);
    }

    FILETIME modified_time = restored_filetime(modified_time_ns);
    if (::SetFileTime(handle.get(), nullptr, nullptr, &modified_time) == 0) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to restore modification time (Windows error " +
                                  std::to_string(::GetLastError()) + ")",
                              path);
    }
}

bool create_restored_symlink(const std::filesystem::path& target,
                             const std::filesystem::path& link_path) {
    constexpr DWORD allow_unprivileged_create = 0x2U;
    if (::CreateSymbolicLinkW(link_path.c_str(), target.c_str(), allow_unprivileged_create) != 0) {
        return true;
    }
    DWORD error = ::GetLastError();
    if (error == ERROR_INVALID_PARAMETER &&
        ::CreateSymbolicLinkW(link_path.c_str(), target.c_str(), 0) != 0) {
        return true;
    }
    error = ::GetLastError();
    if (error == ERROR_PRIVILEGE_NOT_HELD) {
        return false;
    }
    throw LocalVaultError(ErrorCode::filesystem_error,
                          "failed to create restored symbolic link (Windows error " +
                              std::to_string(error) + ")",
                          link_path);
}

RestorePublishResult publish_restored_path(const std::filesystem::path& temporary_path,
                                           const std::filesystem::path& destination_path,
                                           bool replace_existing) {
    DWORD flags = MOVEFILE_WRITE_THROUGH;
    if (replace_existing) {
        flags |= MOVEFILE_REPLACE_EXISTING;
    }
    if (::MoveFileExW(temporary_path.c_str(), destination_path.c_str(), flags) != 0) {
        return RestorePublishResult::published;
    }
    const DWORD error = ::GetLastError();
    if (!replace_existing && (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)) {
        return RestorePublishResult::destination_exists;
    }
    throw LocalVaultError(ErrorCode::filesystem_error,
                          "failed to publish restored path (Windows error " +
                              std::to_string(error) + ")",
                          destination_path);
}

} // namespace localvault
