#include "platform_lock.hpp"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/types.h>
#include <unistd.h>

#include "localvault/error.hpp"

namespace localvault {
namespace {

[[noreturn]] void throw_filesystem_error(const std::filesystem::path& path,
                                         const std::string& operation, int error_number) {
    throw LocalVaultError(ErrorCode::filesystem_error,
                          operation + ": " + std::generic_category().message(error_number), path);
}

void write_diagnostics(int fd, const std::filesystem::path& path) {
    const auto start_time = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
    const std::string diagnostics = "pid=" + std::to_string(static_cast<std::int64_t>(::getpid())) +
                                    "\nacquired_at_unix_seconds=" + std::to_string(start_time) +
                                    "\n";

    if (::ftruncate(fd, 0) == -1) {
        throw_filesystem_error(path, "failed to truncate repository lock file", errno);
    }

    std::size_t written = 0;
    while (written < diagnostics.size()) {
        const ssize_t result = ::pwrite(fd, diagnostics.data() + written,
                                        diagnostics.size() - written, static_cast<off_t>(written));
        if (result == -1) {
            if (errno == EINTR) {
                continue;
            }
            throw_filesystem_error(path, "failed to write repository lock diagnostics", errno);
        }
        if (result == 0) {
            throw LocalVaultError(ErrorCode::filesystem_error,
                                  "failed to write repository lock diagnostics: no bytes written",
                                  path);
        }
        written += static_cast<std::size_t>(result);
    }
}

} // namespace

struct RepositoryLock::Impl {
    explicit Impl(int descriptor) noexcept : fd(descriptor) {}

    ~Impl() {
        if (fd != -1) {
            (void)::flock(fd, LOCK_UN);
            (void)::close(fd);
        }
    }

    int fd{-1};
};

RepositoryLock::RepositoryLock(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

RepositoryLock RepositoryLock::acquire_exclusive(const std::filesystem::path& lock_file) {
    const int fd = ::open(lock_file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd == -1) {
        throw_filesystem_error(lock_file, "failed to open repository lock file", errno);
    }

    auto impl = std::make_unique<Impl>(fd);
    if (::flock(fd, LOCK_EX | LOCK_NB) == -1) {
        const int error_number = errno;
        if (error_number == EWOULDBLOCK || error_number == EAGAIN) {
            throw LocalVaultError(ErrorCode::repository_busy,
                                  "repository is already locked by another process", lock_file);
        }
        throw_filesystem_error(lock_file, "failed to acquire repository lock", error_number);
    }

    write_diagnostics(fd, lock_file);
    return RepositoryLock(std::move(impl));
}

RepositoryLock::~RepositoryLock() = default;
RepositoryLock::RepositoryLock(RepositoryLock&&) noexcept = default;
RepositoryLock& RepositoryLock::operator=(RepositoryLock&&) noexcept = default;

} // namespace localvault
