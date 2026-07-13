#include <cerrno>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <gtest/gtest.h>

#include "filesystem/platform/platform_lock.hpp"

namespace {

struct ChildResult {
    bool launched{false};
    int exit_code{-1};
    std::string error;
};

#ifdef _WIN32
std::wstring quote_windows_argument(const std::wstring& argument) {
    std::wstring result{L'"'};
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'"');
            backslashes = 0;
        } else {
            result.append(backslashes, L'\\');
            result.push_back(character);
            backslashes = 0;
        }
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

ChildResult run_helper(const std::filesystem::path& lock_file, std::uint64_t parent_pid) {
    const std::filesystem::path helper_path{LOCALVAULT_LOCK_HELPER_PATH};
    std::wstring command_line = quote_windows_argument(helper_path.wstring()) + L" " +
                                quote_windows_argument(lock_file.wstring()) + L" " +
                                std::to_wstring(parent_pid);

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    PROCESS_INFORMATION process_info{};
    if (::CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                         &startup_info, &process_info) == 0) {
        return {false, -1, "CreateProcessW failed with error " + std::to_string(::GetLastError())};
    }

    const DWORD wait_result = ::WaitForSingleObject(process_info.hProcess, 5'000);
    DWORD exit_code = 0;
    std::string error;
    if (wait_result == WAIT_TIMEOUT) {
        error = "helper timed out; lock acquisition was not non-blocking";
        (void)::TerminateProcess(process_info.hProcess, 125);
        (void)::WaitForSingleObject(process_info.hProcess, INFINITE);
    } else if (wait_result != WAIT_OBJECT_0) {
        error = "waiting for helper failed with result " + std::to_string(wait_result) +
                " and error " + std::to_string(::GetLastError());
        (void)::TerminateProcess(process_info.hProcess, 125);
        (void)::WaitForSingleObject(process_info.hProcess, INFINITE);
    } else if (::GetExitCodeProcess(process_info.hProcess, &exit_code) == 0) {
        error = "GetExitCodeProcess failed with error " + std::to_string(::GetLastError());
    }
    (void)::CloseHandle(process_info.hThread);
    (void)::CloseHandle(process_info.hProcess);
    return {true, error.empty() ? static_cast<int>(exit_code) : -1, std::move(error)};
}

std::uint64_t current_process_id() {
    return ::GetCurrentProcessId();
}
#else
ChildResult run_helper(const std::filesystem::path& lock_file, std::uint64_t parent_pid) {
    const std::string parent_pid_text = std::to_string(parent_pid);
    const pid_t child = ::fork();
    if (child == -1) {
        return {false, -1, "fork failed: " + std::generic_category().message(errno)};
    }
    if (child == 0) {
        (void)::alarm(5);
        ::execl(LOCALVAULT_LOCK_HELPER_PATH, LOCALVAULT_LOCK_HELPER_PATH, lock_file.c_str(),
                parent_pid_text.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    int status = 0;
    pid_t wait_result = 0;
    do {
        wait_result = ::waitpid(child, &status, 0);
    } while (wait_result == -1 && errno == EINTR);
    if (wait_result == -1) {
        return {true, -1, "waitpid failed: " + std::generic_category().message(errno)};
    }
    if (!WIFEXITED(status)) {
        if (WIFSIGNALED(status)) {
            if (WTERMSIG(status) == SIGALRM) {
                return {true, -1, "helper timed out; lock acquisition was not non-blocking"};
            }
            return {true, -1, "helper terminated by signal " + std::to_string(WTERMSIG(status))};
        }
        return {true, -1, "helper terminated without a normal exit"};
    }
    if (WEXITSTATUS(status) == 127) {
        return {false, -1, "helper executable could not be launched (exit 127)"};
    }
    return {true, WEXITSTATUS(status), {}};
}

std::uint64_t current_process_id() {
    return static_cast<std::uint64_t>(::getpid());
}
#endif

class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path() /
                ("localvault-lock-test-" + std::to_string(current_process_id()));
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

TEST(RepositoryLockIntegrationTest, SecondProcessReceivesRepositoryBusy) {
    TemporaryDirectory temporary_directory;
    const auto lock_file = temporary_directory.path() / "repository.lock";

    {
        auto lock = localvault::RepositoryLock::acquire_exclusive(lock_file);
        const ChildResult child = run_helper(lock_file, current_process_id());
        ASSERT_TRUE(child.launched) << child.error;
        ASSERT_TRUE(child.error.empty()) << child.error;
        EXPECT_EQ(child.exit_code, 0)
            << "helper exit code 0 means a distinct process received repository_busy";
    }

    EXPECT_TRUE(std::filesystem::exists(lock_file));
    std::ifstream diagnostics(lock_file);
    const std::string contents((std::istreambuf_iterator<char>(diagnostics)),
                               std::istreambuf_iterator<char>());
    EXPECT_NE(contents.find("pid=" + std::to_string(current_process_id())), std::string::npos);
    EXPECT_NE(contents.find("acquired_at_unix_seconds="), std::string::npos);

    EXPECT_NO_THROW({
        auto reacquired = localvault::RepositoryLock::acquire_exclusive(lock_file);
        (void)reacquired;
    });
}

} // namespace
