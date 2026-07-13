#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <unistd.h>
#endif

#include "filesystem/platform/platform_lock.hpp"
#include "localvault/error.hpp"

namespace {

int try_contended_lock(const std::filesystem::path& lock_file, std::uint64_t expected_parent_pid) {
#ifdef _WIN32
    const std::uint64_t process_id = ::GetCurrentProcessId();
#else
    const std::uint64_t process_id = static_cast<std::uint64_t>(::getpid());
#endif
    if (process_id == expected_parent_pid) {
        std::cerr << "helper did not run in a distinct process\n";
        return 2;
    }

    try {
        auto lock = localvault::RepositoryLock::acquire_exclusive(lock_file);
        std::cerr << "helper unexpectedly acquired the repository lock\n";
        return 3;
    } catch (const localvault::LocalVaultError& error) {
        if (error.code() == localvault::ErrorCode::repository_busy) {
            return 0;
        }
        std::cerr << "helper received LocalVault error code " << static_cast<int>(error.code())
                  << ": " << error.what() << '\n';
        return 4;
    } catch (const std::exception& error) {
        std::cerr << "helper received unexpected exception: " << error.what() << '\n';
        return 5;
    }
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    if (argc != 3) {
        std::cerr << "usage: lock_contention_helper <lock-file> <parent-pid>\n";
        return 64;
    }
    wchar_t* end = nullptr;
    const std::uint64_t parent_pid = std::wcstoull(argv[2], &end, 10);
    if (end == argv[2] || *end != L'\0') {
        std::cerr << "invalid parent PID\n";
        return 64;
    }
    return try_contended_lock(std::filesystem::path(argv[1]), parent_pid);
}
#else
int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "usage: lock_contention_helper <lock-file> <parent-pid>\n";
        return 64;
    }
    char* end = nullptr;
    const std::uint64_t parent_pid = std::strtoull(argv[2], &end, 10);
    if (end == argv[2] || *end != '\0') {
        std::cerr << "invalid parent PID\n";
        return 64;
    }
    return try_contended_lock(std::filesystem::path(argv[1]), parent_pid);
}
#endif
