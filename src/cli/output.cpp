#include "commands.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

#include "console.hpp"

namespace localvault::cli {
namespace {

FILE* output_spool() {
#ifdef _WIN32
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("localvault-output-" + std::to_string(GetCurrentProcessId()) + "-" +
                       std::to_string(stamp));
    const auto handle =
        CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                    FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return nullptr;
    }
    const auto descriptor =
        _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDWR | _O_BINARY);
    if (descriptor < 0) {
        CloseHandle(handle);
        return nullptr;
    }
    auto stream = _fdopen(descriptor, "w+b");
    if (!stream) {
        _close(descriptor);
    }
    return stream;
#else
    return std::tmpfile();
#endif
}

} // namespace

std::filesystem::path utf8_path(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string path_text(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

std::filesystem::path Context::repository_path() const {
    if (repository.empty()) {
        throw LocalVaultError(ErrorCode::invalid_argument, "--repo is required for this command");
    }
    return utf8_path(repository);
}

ProgressCallback Context::progress() const {
    if (quiet) {
        return {};
    }
    return [last_phase = OperationPhase::complete, last_time = std::chrono::steady_clock::now(),
            interactive = terminal(stderr), detail = verbose,
            machine = json](const ProgressEvent& event) mutable {
        const auto now = std::chrono::steady_clock::now();
        const bool phase_change = event.phase != last_phase;
        if (!phase_change && (!interactive || now - last_time < std::chrono::milliseconds(100))) {
            return;
        }
        constexpr std::array names{
            "preparing",   "scanning",           "reading",          "hashing",
            "compressing", "writing objects",    "writing metadata", "restoring",
            "verifying",   "garbage collecting", "finalizing",       "complete"};
        if (machine) {
            std::cerr << Json{{"phase", names.at(static_cast<std::size_t>(event.phase))},
                              {"processed_entries", event.processed_entries},
                              {"processed_bytes", event.processed_bytes},
                              {"path", detail ? path_text(event.current_path) : ""}}
                             .dump()
                      << '\n';
        } else {
            std::cerr << (interactive ? "\r" : "")
                      << names.at(static_cast<std::size_t>(event.phase)) << ": "
                      << event.processed_entries << " entries, " << event.processed_bytes
                      << " bytes";
            if (detail && !event.current_path.empty()) {
                std::cerr << " (" << path_text(event.current_path) << ')';
            }
            if (!interactive || event.phase == OperationPhase::complete) {
                std::cerr << '\n';
            }
        }
        std::cerr.flush();
        last_phase = event.phase;
        last_time = now;
    };
}

int Context::finish(Json result, int exit_code) const {
    if (stop.stop_requested()) {
        throw LocalVaultError(ErrorCode::cancelled, "command cancelled");
    }
    const auto output =
        json
            ? Json{{"schema_version", 1}, {"command", command}, {"result", std::move(result)}}.dump(
                  -1, ' ', false, Json::error_handler_t::replace)
            : result.dump(2, ' ', false, Json::error_handler_t::replace);
    if (stop.stop_requested()) {
        throw LocalVaultError(ErrorCode::cancelled, "command cancelled");
    }
    std::cout << output << '\n';
    std::cout.flush();
    if (!std::cout) {
        std::cerr << "localvault: cannot write result output\n";
        return 4;
    }
    return stop.stop_requested() ? 130 : exit_code;
}

int Context::finish_array(std::string_view field, const JsonProducer& produce) const {
    std::unique_ptr<FILE, decltype(&std::fclose)> spool(output_spool(), &std::fclose);
    if (!spool) {
        throw LocalVaultError(ErrorCode::filesystem_error, "cannot create output spool");
    }
    const auto write = [&](std::string_view text) {
        if (std::fwrite(text.data(), 1, text.size(), spool.get()) != text.size()) {
            throw LocalVaultError(ErrorCode::filesystem_error, "cannot write output spool");
        }
    };
    write(json ? "{\"schema_version\":1,\"command\":" + Json(command).dump() + ",\"result\":{"
               : "{");
    write(Json(field).dump() + ":[\n");
    bool first = true;
    produce([&](Json entry) {
        if (stop.stop_requested()) {
            throw LocalVaultError(ErrorCode::cancelled, "command cancelled");
        }
        if (!first) {
            write(",\n");
        }
        first = false;
        write(entry.dump(-1, ' ', false, Json::error_handler_t::replace));
    });
    write(json ? "\n]}}\n" : "\n]}\n");
    if (std::fflush(spool.get()) != 0 || std::fseek(spool.get(), 0, SEEK_SET) != 0) {
        throw LocalVaultError(ErrorCode::filesystem_error, "cannot rewind output spool");
    }
    if (stop.stop_requested()) {
        throw LocalVaultError(ErrorCode::cancelled, "command cancelled");
    }
    std::array<char, 64 * 1024> buffer{};
    while (const auto count = std::fread(buffer.data(), 1, buffer.size(), spool.get())) {
        std::cout.write(buffer.data(), static_cast<std::streamsize>(count));
    }
    std::cout.flush();
    if (std::ferror(spool.get()) || !std::cout) {
        std::cerr << "localvault: cannot copy result output\n";
        return 4;
    }
    return stop.stop_requested() ? 130 : 0;
}

int Context::fail(int exit_code, std::string_view message,
                  const std::filesystem::path& path) const {
    std::cerr << "localvault: " << message;
    if (!path.empty()) {
        std::cerr << ": " << path_text(path);
    }
    std::cerr << '\n';
    if (json) {
        std::cout << Json{{"schema_version", 1},
                          {"command", command},
                          {"error",
                           {{"code", exit_code}, {"message", message}, {"path", path_text(path)}}}}
                         .dump(-1, ' ', false, Json::error_handler_t::replace)
                  << '\n';
    }
    return exit_code;
}

int error_exit(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::invalid_argument:
    case ErrorCode::unsafe_restore_path:
        return 2;
    case ErrorCode::repository_not_found:
    case ErrorCode::invalid_repository:
    case ErrorCode::unsupported_repository_version:
        return 3;
    case ErrorCode::object_missing:
    case ErrorCode::object_corrupt:
        return 5;
    case ErrorCode::partial_success:
        return 6;
    case ErrorCode::repository_busy:
        return 7;
    case ErrorCode::cancelled:
        return 130;
    default:
        return 4;
    }
}

Json snapshot_json(const SnapshotSummary& snapshot) {
    return {{"id", snapshot.id},
            {"created_at_ns", std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  snapshot.created_at.time_since_epoch())
                                  .count()},
            {"source_root", path_text(snapshot.source_root)},
            {"message", snapshot.message},
            {"status", "complete"},
            {"file_count", snapshot.file_count},
            {"directory_count", snapshot.directory_count},
            {"logical_size", snapshot.logical_size},
            {"new_stored_size", snapshot.new_stored_size},
            {"duration_ms", snapshot.duration.count()}};
}

Json entry_json(const EntryInfo& entry) {
    const auto type = entry.type == EntryType::regular_file ? "file"
                      : entry.type == EntryType::directory  ? "directory"
                                                            : "symlink";
    Json result{{"path", path_text(entry.relative_path)},
                {"type", type},
                {"logical_size", entry.logical_size},
                {"modified_time_ns", entry.modified_time_ns},
                {"posix_mode", entry.posix_mode}};
    result["windows_attributes"] =
        entry.windows_attributes ? Json(*entry.windows_attributes) : Json();
    result["file_hash"] = entry.file_hash_hex ? Json(*entry.file_hash_hex) : Json();
    result["symlink_target"] =
        entry.symlink_target ? Json(path_text(*entry.symlink_target)) : Json();
    return result;
}

Json warnings_json(const std::vector<SkippedEntry>& warnings) {
    Json result = Json::array();
    for (const auto& warning : warnings) {
        result.push_back({{"path", path_text(warning.relative_path)}, {"message", warning.reason}});
        std::cerr << "warning: " << path_text(warning.relative_path) << ": " << warning.reason
                  << '\n';
    }
    return result;
}

Json stats_json(const RepositoryStats& stats) {
    return {{"complete_snapshot_count", stats.complete_snapshot_count},
            {"unique_chunk_count", stats.unique_chunk_count},
            {"logical_bytes", stats.logical_bytes},
            {"unique_raw_bytes", stats.unique_raw_bytes},
            {"stored_bytes", stats.stored_bytes},
            {"deduplication_savings", stats.deduplication_savings},
            {"compression_savings", stats.compression_savings},
            {"total_savings", stats.total_savings}};
}

} // namespace localvault::cli
