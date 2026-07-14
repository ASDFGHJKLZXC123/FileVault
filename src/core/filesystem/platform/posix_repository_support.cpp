#include "repository_support.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/mount.h>
#else
#include <sys/random.h>
#include <sys/vfs.h>
#endif

#include "localvault/error.hpp"

namespace localvault {
namespace {

[[noreturn]] void throw_filesystem_error(const std::filesystem::path& path,
                                         const std::string& operation, int error_number) {
    throw LocalVaultError(ErrorCode::filesystem_error,
                          operation + ": " + std::generic_category().message(error_number), path);
}

#if defined(__linux__)
[[nodiscard]] std::string decode_mount_path(std::string_view encoded) {
    std::string decoded;
    decoded.reserve(encoded.size());
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        if (encoded[index] == '\\' && index + 3 < encoded.size() && encoded[index + 1] >= '0' &&
            encoded[index + 1] <= '7' && encoded[index + 2] >= '0' && encoded[index + 2] <= '7' &&
            encoded[index + 3] >= '0' && encoded[index + 3] <= '7') {
            const int value = (encoded[index + 1] - '0') * 64 + (encoded[index + 2] - '0') * 8 +
                              (encoded[index + 3] - '0');
            decoded.push_back(static_cast<char>(value));
            index += 3;
        } else {
            decoded.push_back(encoded[index]);
        }
    }
    return decoded;
}

[[nodiscard]] bool is_path_within(std::string_view path, std::string_view mount_point) {
    return path == mount_point || mount_point == "/" ||
           (path.size() > mount_point.size() && path.starts_with(mount_point) &&
            path[mount_point.size()] == '/');
}

[[nodiscard]] std::optional<std::string>
linux_mount_type(const std::filesystem::path& existing_path) {
    std::error_code error;
    const std::filesystem::path canonical = std::filesystem::canonical(existing_path, error);
    if (error) {
        return std::nullopt;
    }
    const std::string path = canonical.string();
    std::ifstream mounts("/proc/self/mountinfo");
    std::string line;
    std::size_t best_length = 0;
    std::optional<std::string> best_type;
    while (std::getline(mounts, line)) {
        const std::size_t separator = line.find(" - ");
        if (separator == std::string::npos) {
            continue;
        }
        std::istringstream left(line.substr(0, separator));
        std::string field;
        std::string mount_point;
        for (int field_index = 1; field_index <= 5 && left >> field; ++field_index) {
            if (field_index == 5) {
                mount_point = decode_mount_path(field);
            }
        }
        std::istringstream right(line.substr(separator + 3));
        std::string filesystem_type;
        right >> filesystem_type;
        if (!mount_point.empty() && is_path_within(path, mount_point) &&
            mount_point.size() >= best_length) {
            best_length = mount_point.size();
            best_type = std::move(filesystem_type);
        }
    }
    return best_type;
}
#endif

} // namespace

std::optional<std::string> platform_filesystem_name(const std::filesystem::path& existing_path) {
#if defined(__APPLE__)
    struct statfs information{};
    if (::statfs(existing_path.c_str(), &information) != 0) {
        return std::nullopt;
    }
    return std::string(information.f_fstypename);
#else
    if (const std::optional<std::string> mount_type = linux_mount_type(existing_path);
        mount_type.has_value()) {
        return mount_type;
    }
    struct statfs information{};
    if (::statfs(existing_path.c_str(), &information) != 0) {
        return std::nullopt;
    }
    switch (static_cast<unsigned long>(information.f_type)) {
    case 0x6969UL:
        return "nfs";
    case 0xFF534D42UL:
    case 0xFE534D42UL:
        return "cifs";
    case 0x4D44UL:
        return "fat";
    case 0x2011BAB0UL:
        return "exfat";
    default:
        return std::nullopt;
    }
#endif
}

std::array<std::uint8_t, 16> secure_random_uuid_bytes() {
    std::array<std::uint8_t, 16> bytes{};
#if defined(__APPLE__)
    ::arc4random_buf(bytes.data(), bytes.size());
#else
    if (::getentropy(bytes.data(), bytes.size()) != 0) {
        throw LocalVaultError(ErrorCode::internal_error,
                              "secure random number generation failed: " +
                                  std::generic_category().message(errno));
    }
#endif
    return bytes;
}

void apply_restrictive_repository_permissions(const std::filesystem::path& root) {
    if (::chmod(root.c_str(), S_IRWXU) != 0) {
        throw_filesystem_error(root, "failed to set repository permissions", errno);
    }
}

void apply_restrictive_file_permissions(const std::filesystem::path& path) {
    if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
        throw_filesystem_error(path, "failed to set repository file permissions", errno);
    }
}

void create_exclusive_file(const std::filesystem::path& path) {
    const int descriptor =
        ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (descriptor == -1) {
        throw_filesystem_error(path, "failed to exclusively create repository file", errno);
    }
    (void)::close(descriptor);
}

bool repository_storage_is_proven_read_only(const std::filesystem::path& root) noexcept {
    struct statvfs filesystem_information{};
    return ::statvfs(root.c_str(), &filesystem_information) == 0 &&
           (filesystem_information.f_flag & ST_RDONLY) != 0U;
}

bool platform_is_sharing_violation(const std::error_code&) noexcept {
    return false;
}

void sync_existing_regular_file(const std::filesystem::path& path) {
    const int descriptor = ::open(path.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor == -1) {
        throw_filesystem_error(path, "failed to open existing object for flushing", errno);
    }

    struct stat information{};
    if (::fstat(descriptor, &information) != 0) {
        const int error_number = errno;
        (void)::close(descriptor);
        throw_filesystem_error(path, "failed to inspect existing object for flushing",
                               error_number);
    }
    if (!S_ISREG(information.st_mode)) {
        (void)::close(descriptor);
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "existing object to flush is not a regular file", path);
    }

#if defined(__APPLE__)
    const int result = ::fcntl(descriptor, F_FULLFSYNC);
#else
    const int result = ::fsync(descriptor);
#endif
    if (result != 0) {
        const int error_number = errno;
        (void)::close(descriptor);
        throw_filesystem_error(path, "failed to flush existing object", error_number);
    }
    if (::close(descriptor) != 0) {
        throw_filesystem_error(path, "failed to close flushed existing object", errno);
    }
}

void flush_containing_directory(const std::filesystem::path& path) {
    const std::filesystem::path directory = path.parent_path();
    const int descriptor = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor == -1) {
        throw_filesystem_error(directory, "failed to open repository directory for flushing",
                               errno);
    }
    if (::fsync(descriptor) != 0) {
        const int error_number = errno;
        (void)::close(descriptor);
        throw_filesystem_error(directory, "failed to flush repository directory", error_number);
    }
    if (::close(descriptor) != 0) {
        throw_filesystem_error(directory, "failed to close flushed repository directory", errno);
    }
}

} // namespace localvault
