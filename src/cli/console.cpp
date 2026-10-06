#include "console.hpp"

#include "localvault/error.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <io.h>
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <poll.h>
#include <unistd.h>
#endif

namespace localvault::cli {
namespace {

constexpr auto interval = std::chrono::milliseconds(10);
std::atomic<unsigned> interrupts{};
std::atomic_flag active = ATOMIC_FLAG_INIT;
static_assert(std::atomic<unsigned>::is_always_lock_free);

#ifdef _WIN32
BOOL WINAPI on_interrupt(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) {
        return FALSE;
    }
    interrupts.fetch_add(1, std::memory_order_relaxed);
    return TRUE;
}
#else
void on_interrupt(int) {
    interrupts.fetch_add(1, std::memory_order_relaxed);
}
#endif

void check_cancelled(std::stop_token stop) {
    if (stop.stop_requested()) {
        throw LocalVaultError(ErrorCode::cancelled, "cancelled while waiting for input");
    }
}

std::string end_input(std::string answer) {
    if (answer.empty()) {
        throw LocalVaultError(
            ErrorCode::invalid_argument,
            "prompt reached end of input; supply an answer or choose a non-interactive option");
    }
    if (answer.back() == '\r') {
        answer.pop_back();
    }
    return answer;
}

#ifdef _WIN32
void echo_console(const wchar_t* text, DWORD length) {
    DWORD written{};
    WriteConsoleW(GetStdHandle(STD_ERROR_HANDLE), text, length, &written, nullptr);
}

std::string console_answer(HANDLE input, std::stop_token stop) {
    std::wstring answer;
    while (true) {
        check_cancelled(stop);
        const auto waited = WaitForSingleObject(input, 10);
        if (waited == WAIT_TIMEOUT) {
            continue;
        }
        INPUT_RECORD event{};
        DWORD count{};
        if (waited != WAIT_OBJECT_0 || !ReadConsoleInputW(input, &event, 1, &count)) {
            throw LocalVaultError(ErrorCode::filesystem_error, "failed to read console input");
        }
        check_cancelled(stop);
        if (count == 0 || event.EventType != KEY_EVENT || !event.Event.KeyEvent.bKeyDown) {
            continue;
        }
        const auto& key = event.Event.KeyEvent;
        const auto character = key.uChar.UnicodeChar;
        if (character == L'\r' || character == L'\n') {
            echo_console(L"\r\n", 2);
            if (answer.empty()) {
                return {};
            }
            const int size =
                WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, answer.data(),
                                    static_cast<int>(answer.size()), nullptr, 0, nullptr, nullptr);
            if (size == 0) {
                throw LocalVaultError(ErrorCode::invalid_argument,
                                      "prompt input is invalid Unicode");
            }
            std::string utf8(static_cast<std::size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, answer.data(),
                                static_cast<int>(answer.size()), utf8.data(), size, nullptr,
                                nullptr);
            return utf8;
        }
        if (character == 26) {
            throw LocalVaultError(ErrorCode::invalid_argument, "prompt reached end of input");
        }
        for (WORD repeat = 0; repeat < key.wRepeatCount; ++repeat) {
            if (character == L'\b') {
                if (!answer.empty()) {
                    const auto last = answer.back();
                    answer.pop_back();
                    if (last >= 0xdc00 && last <= 0xdfff && !answer.empty()) {
                        answer.pop_back();
                    }
                    echo_console(L"\b \b", 3);
                }
            } else if (character >= L' ') {
                answer.push_back(character);
                echo_console(&character, 1);
            }
        }
    }
}
#endif

} // namespace

struct InterruptHandler::Impl {
    std::stop_source source;
    std::jthread stopper;
    std::jthread monitor;
#ifndef _WIN32
    struct sigaction previous{};
#endif

    Impl() {
        if (active.test_and_set()) {
            throw LocalVaultError(ErrorCode::internal_error, "interrupt handler is already active");
        }
        interrupts.store(0, std::memory_order_relaxed);
#ifdef _WIN32
        const bool installed = SetConsoleCtrlHandler(on_interrupt, TRUE) != FALSE;
#else
        struct sigaction action{};
        action.sa_handler = on_interrupt;
        action.sa_flags = SA_RESTART;
        sigemptyset(&action.sa_mask);
        const bool installed = sigaction(SIGINT, &action, &previous) == 0;
#endif
        if (!installed) {
            active.clear();
            throw LocalVaultError(ErrorCode::internal_error, "failed to install interrupt handler");
        }
        try {
            // request_stop invokes callbacks synchronously; keep forced exit independent of them.
            stopper = std::jthread([this](std::stop_token shutdown) {
                while (!shutdown.stop_requested()) {
                    if (interrupts.load(std::memory_order_relaxed) != 0) {
                        source.request_stop();
                        return;
                    }
                    std::this_thread::sleep_for(interval);
                }
            });
            monitor = std::jthread([](std::stop_token shutdown) {
                while (!shutdown.stop_requested()) {
                    if (interrupts.load(std::memory_order_relaxed) > 1) {
                        std::_Exit(130);
                    }
                    std::this_thread::sleep_for(interval);
                }
            });
        } catch (...) {
            uninstall();
            throw;
        }
    }

    void uninstall() {
        stopper.request_stop();
        if (stopper.joinable()) {
            stopper.join();
        }
        monitor.request_stop();
        if (monitor.joinable()) {
            monitor.join();
        }
#ifdef _WIN32
        SetConsoleCtrlHandler(on_interrupt, FALSE);
#else
        sigaction(SIGINT, &previous, nullptr);
#endif
        active.clear();
    }

    ~Impl() {
        uninstall();
    }
};

InterruptHandler::InterruptHandler() : impl_(std::make_unique<Impl>()) {}
InterruptHandler::~InterruptHandler() = default;
std::stop_token InterruptHandler::token() const {
    return impl_->source.get_token();
}

bool terminal(FILE* stream) {
    if (stream == nullptr) {
        return false;
    }
#ifdef _WIN32
    const auto descriptor = _fileno(stream);
    return descriptor >= 0 && _isatty(descriptor) != 0;
#else
    const auto descriptor = fileno(stream);
    return descriptor >= 0 && isatty(descriptor) != 0;
#endif
}

std::string read_answer(std::stop_token stop) {
    std::string answer;
#ifdef _WIN32
    const auto descriptor = _fileno(stdin);
    if (descriptor < 0) {
        return end_input({});
    }
    const auto input = reinterpret_cast<HANDLE>(_get_osfhandle(descriptor));
    if (input == INVALID_HANDLE_VALUE) {
        return end_input({});
    }
    DWORD mode{};
    if (GetConsoleMode(input, &mode)) {
        return console_answer(input, stop);
    }
    const auto type = GetFileType(input);
#else
    const auto input = fileno(stdin);
#endif
    while (true) {
        check_cancelled(stop);
        char character{};
#ifdef _WIN32
        if (type == FILE_TYPE_PIPE) {
            DWORD available{};
            if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) {
                if (GetLastError() == ERROR_BROKEN_PIPE) {
                    return end_input(std::move(answer));
                }
                throw LocalVaultError(ErrorCode::filesystem_error,
                                      "failed to inspect prompt input");
            }
            if (available == 0) {
                std::this_thread::sleep_for(interval);
                continue;
            }
        }
        DWORD count{};
        if (!ReadFile(input, &character, 1, &count, nullptr)) {
            if (GetLastError() != ERROR_BROKEN_PIPE) {
                throw LocalVaultError(ErrorCode::filesystem_error, "failed to read prompt input");
            }
            count = 0;
        }
#else
        pollfd ready{input, POLLIN, 0};
        const auto polled = poll(&ready, 1, 10);
        if (polled == 0 || (polled < 0 && errno == EINTR)) {
            continue;
        }
        if (polled < 0 || (ready.revents & (POLLERR | POLLNVAL)) != 0) {
            throw LocalVaultError(ErrorCode::filesystem_error, "failed to inspect prompt input");
        }
        const auto count = read(input, &character, 1);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw LocalVaultError(ErrorCode::filesystem_error, "failed to read prompt input");
        }
#endif
        check_cancelled(stop);
        if (count == 0) {
            return end_input(std::move(answer));
        }
        if (character == '\n') {
            if (!answer.empty() && answer.back() == '\r') {
                answer.pop_back();
            }
            return answer;
        }
        answer.push_back(character);
    }
}

} // namespace localvault::cli
