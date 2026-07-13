#include "platform_lock.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "localvault/error.hpp"

namespace localvault {
namespace {

[[noreturn]] void throw_filesystem_error(const std::filesystem::path& path,
                                         const std::string& operation, DWORD error_number) {
    throw LocalVaultError(ErrorCode::filesystem_error,
                          operation + " (Windows error " + std::to_string(error_number) + ")",
                          path);
}

void write_diagnostics(HANDLE handle, const std::filesystem::path& path) {
    const auto start_time = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
    const std::string diagnostics =
        "pid=" + std::to_string(static_cast<std::uint64_t>(::GetCurrentProcessId())) +
        "\nacquired_at_unix_seconds=" + std::to_string(start_time) + "\n";

    LARGE_INTEGER beginning{};
    if (::SetFilePointerEx(handle, beginning, nullptr, FILE_BEGIN) == 0) {
        throw_filesystem_error(path, "failed to seek repository lock file", ::GetLastError());
    }
    if (::SetEndOfFile(handle) == 0) {
        throw_filesystem_error(path, "failed to truncate repository lock file", ::GetLastError());
    }

    std::size_t written = 0;
    while (written < diagnostics.size()) {
        const std::size_t remaining = diagnostics.size() - written;
        const DWORD chunk_size = static_cast<DWORD>(
            (std::min)(remaining, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD chunk_written = 0;
        if (::WriteFile(handle, diagnostics.data() + written, chunk_size, &chunk_written,
                        nullptr) == 0) {
            throw_filesystem_error(path, "failed to write repository lock diagnostics",
                                   ::GetLastError());
        }
        if (chunk_written == 0) {
            throw LocalVaultError(ErrorCode::filesystem_error,
                                  "failed to write repository lock diagnostics: no bytes written",
                                  path);
        }
        written += static_cast<std::size_t>(chunk_written);
    }
}

} // namespace

struct RepositoryLock::Impl {
    explicit Impl(HANDLE file_handle) noexcept : handle(file_handle) {}

    ~Impl() {
        if (handle != INVALID_HANDLE_VALUE) {
            (void)::UnlockFileEx(handle, 0, (std::numeric_limits<DWORD>::max)(),
                                 (std::numeric_limits<DWORD>::max)(), &overlapped);
            (void)::CloseHandle(handle);
        }
    }

    HANDLE handle{INVALID_HANDLE_VALUE};
    OVERLAPPED overlapped{};
};

RepositoryLock::RepositoryLock(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

RepositoryLock RepositoryLock::acquire_exclusive(const std::filesystem::path& lock_file) {
    const HANDLE handle = ::CreateFileW(lock_file.c_str(), GENERIC_READ | GENERIC_WRITE,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw_filesystem_error(lock_file, "failed to open repository lock file", ::GetLastError());
    }

    auto impl = std::make_unique<Impl>(handle);
    if (::LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0,
                     (std::numeric_limits<DWORD>::max)(), (std::numeric_limits<DWORD>::max)(),
                     &impl->overlapped) == 0) {
        const DWORD error_number = ::GetLastError();
        if (error_number == ERROR_LOCK_VIOLATION) {
            throw LocalVaultError(ErrorCode::repository_busy,
                                  "repository is already locked by another process", lock_file);
        }
        throw_filesystem_error(lock_file, "failed to acquire repository lock", error_number);
    }

    write_diagnostics(handle, lock_file);
    return RepositoryLock(std::move(impl));
}

RepositoryLock::~RepositoryLock() = default;
RepositoryLock::RepositoryLock(RepositoryLock&&) noexcept = default;
RepositoryLock& RepositoryLock::operator=(RepositoryLock&&) noexcept = default;

} // namespace localvault
