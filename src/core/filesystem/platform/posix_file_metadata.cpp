#include "file_metadata.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/stdio.h>
#else
#include <linux/fs.h>
#include <sys/syscall.h>
#endif

#include "localvault/error.hpp"
#include "repository_support.hpp"

namespace localvault {
namespace {

[[nodiscard]] std::int64_t nanoseconds_since_epoch(std::int64_t seconds, std::int64_t nanoseconds,
                                                   const std::filesystem::path& path) {
    constexpr std::int64_t nanoseconds_per_second = 1'000'000'000;
    constexpr std::int64_t maximum = (std::numeric_limits<std::int64_t>::max)();
    constexpr std::int64_t minimum = (std::numeric_limits<std::int64_t>::min)();
    if (nanoseconds < 0 || nanoseconds >= nanoseconds_per_second ||
        seconds > maximum / nanoseconds_per_second || seconds < minimum / nanoseconds_per_second) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "file modification time is outside the supported range", path);
    }
    const std::int64_t whole_seconds = seconds * nanoseconds_per_second;
    if (whole_seconds > maximum - nanoseconds) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "file modification time is outside the supported range", path);
    }
    return whole_seconds + nanoseconds;
}

void validate_posix_mode(std::uint32_t posix_mode, const std::filesystem::path& path) {
    constexpr std::uint32_t valid_mode_mask = static_cast<std::uint32_t>(
        S_IFMT | S_ISUID | S_ISGID | S_ISVTX | S_IRWXU | S_IRWXG | S_IRWXO);
    if ((posix_mode & ~valid_mode_mask) != 0U) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "saved POSIX mode contains unsupported bits", path);
    }
}

[[nodiscard]] timespec restored_modification_time(std::int64_t modified_time_ns,
                                                  const std::filesystem::path& path) {
    constexpr std::int64_t nanoseconds_per_second = 1'000'000'000;
    std::int64_t seconds = modified_time_ns / nanoseconds_per_second;
    std::int64_t nanoseconds = modified_time_ns % nanoseconds_per_second;
    if (nanoseconds < 0) {
        --seconds;
        nanoseconds += nanoseconds_per_second;
    }
    timespec result{};
    result.tv_sec = static_cast<time_t>(seconds);
    result.tv_nsec = static_cast<long>(nanoseconds);
    if (static_cast<std::int64_t>(result.tv_sec) != seconds) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "restored modification time is outside the platform range", path);
    }
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
    Impl(int descriptor, std::filesystem::path temporary_path)
        : fd(descriptor), path(std::move(temporary_path)) {}

    ~Impl() noexcept {
        if (fd != -1) {
            (void)::close(fd);
        }
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    int fd{-1};
    std::filesystem::path path;
};

TemporaryOutputFile::TemporaryOutputFile(const std::filesystem::path& directory) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        const std::filesystem::path candidate = directory / random_temporary_name();
        const int descriptor =
            ::open(candidate.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (descriptor != -1) {
            impl_ = std::make_unique<Impl>(descriptor, candidate);
            return;
        }
        if (errno != EEXIST) {
            throw LocalVaultError(ErrorCode::filesystem_error,
                                  "failed to create restore temporary file: " +
                                      std::generic_category().message(errno),
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
        const std::size_t count = std::min(maximum_write, bytes.size() - offset);
        const ssize_t written = ::write(impl_->fd, bytes.data() + offset, count);
        if (written == -1) {
            if (errno == EINTR) {
                continue;
            }
            throw LocalVaultError(ErrorCode::filesystem_error,
                                  "failed to write restore temporary file: " +
                                      std::generic_category().message(errno),
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

void TemporaryOutputFile::apply_metadata(std::int64_t modified_time_ns, std::uint32_t posix_mode) {
    validate_posix_mode(posix_mode, impl_->path);
    if (posix_mode != 0U && ::fchmod(impl_->fd, static_cast<mode_t>(posix_mode & 07777U)) != 0) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to restore POSIX permissions: " +
                                  std::generic_category().message(errno),
                              impl_->path);
    }
    struct timespec times[2]{};
    times[0].tv_nsec = UTIME_OMIT;
    times[1] = restored_modification_time(modified_time_ns, impl_->path);
    if (::futimens(impl_->fd, times) != 0) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to restore modification time: " +
                                  std::generic_category().message(errno),
                              impl_->path);
    }
}

void TemporaryOutputFile::sync() {
#if defined(__APPLE__)
    const int result = ::fcntl(impl_->fd, F_FULLFSYNC);
#else
    const int result = ::fsync(impl_->fd);
#endif
    if (result != 0) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to sync restored temporary file: " +
                                  std::generic_category().message(errno),
                              impl_->path);
    }
}

RestorePublishResult TemporaryOutputFile::publish(const std::filesystem::path& destination_path,
                                                  bool replace_existing) {
    return publish_restored_path(impl_->path, destination_path, replace_existing);
}

PlatformFileMetadata read_platform_file_metadata_no_follow(const std::filesystem::path& path) {
    struct stat information{};
    if (::lstat(path.c_str(), &information) != 0) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to read file metadata without following symlinks: " +
                                  std::generic_category().message(errno),
                              path);
    }
    if (information.st_size < 0) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "file size is outside the supported range", path);
    }

#if defined(__APPLE__)
    const std::int64_t seconds = information.st_mtimespec.tv_sec;
    const std::int64_t nanoseconds = information.st_mtimespec.tv_nsec;
#else
    const std::int64_t seconds = information.st_mtim.tv_sec;
    const std::int64_t nanoseconds = information.st_mtim.tv_nsec;
#endif

    return {
        static_cast<std::uint64_t>(information.st_size),
        nanoseconds_since_epoch(seconds, nanoseconds, path),
        static_cast<std::uint32_t>(information.st_mode),
    };
}

void apply_restored_metadata(const std::filesystem::path& path, std::int64_t modified_time_ns,
                             std::uint32_t posix_mode) {
    validate_posix_mode(posix_mode, path);
    if (posix_mode != 0U && ::chmod(path.c_str(), static_cast<mode_t>(posix_mode & 07777U)) != 0) {
        throw LocalVaultError(
            ErrorCode::filesystem_error,
            "failed to restore POSIX permissions: " + std::generic_category().message(errno), path);
    }

    struct timespec times[2]{};
    times[0].tv_nsec = UTIME_OMIT;
    times[1] = restored_modification_time(modified_time_ns, path);
    if (::utimensat(AT_FDCWD, path.c_str(), times, 0) != 0) {
        throw LocalVaultError(
            ErrorCode::filesystem_error,
            "failed to restore modification time: " + std::generic_category().message(errno), path);
    }
}

bool create_restored_symlink(const std::filesystem::path& target,
                             const std::filesystem::path& link_path) {
    std::error_code error;
    std::filesystem::create_symlink(target, link_path, error);
    if (error) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to create restored symbolic link: " + error.message(),
                              link_path);
    }
    return true;
}

RestorePublishResult publish_restored_path(const std::filesystem::path& temporary_path,
                                           const std::filesystem::path& destination_path,
                                           bool replace_existing) {
    if (replace_existing) {
        if (::rename(temporary_path.c_str(), destination_path.c_str()) != 0) {
            throw LocalVaultError(ErrorCode::filesystem_error,
                                  "failed to publish restored path: " +
                                      std::generic_category().message(errno),
                                  destination_path);
        }
        return RestorePublishResult::published;
    }

#if defined(__APPLE__)
    const int result = ::renamex_np(temporary_path.c_str(), destination_path.c_str(), RENAME_EXCL);
#else
    const int result =
        static_cast<int>(::syscall(SYS_renameat2, AT_FDCWD, temporary_path.c_str(), AT_FDCWD,
                                   destination_path.c_str(), RENAME_NOREPLACE));
#endif
    if (result == 0) {
        return RestorePublishResult::published;
    }
    if (errno == EEXIST) {
        return RestorePublishResult::destination_exists;
    }
    throw LocalVaultError(ErrorCode::filesystem_error,
                          "failed to publish restored path without replacement: " +
                              std::generic_category().message(errno),
                          destination_path);
}

} // namespace localvault
