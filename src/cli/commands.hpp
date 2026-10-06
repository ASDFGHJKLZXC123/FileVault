#pragma once

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>

#include "localvault/error.hpp"
#include "localvault/progress.hpp"
#include "localvault/types.hpp"

namespace localvault::cli {

using Json = nlohmann::json;
using Dispatcher = std::function<int()>;
using JsonConsumer = std::function<void(Json)>;
using JsonProducer = std::function<void(const JsonConsumer&)>;

struct Context {
    std::string repository;
    std::string command{"localvault"};
    bool json{};
    bool verbose{};
    bool quiet{};
    bool no_color{};
    std::stop_token stop;

    [[nodiscard]] std::filesystem::path repository_path() const;
    [[nodiscard]] ProgressCallback progress() const;
    int finish(Json result, int exit_code = 0) const;
    int finish_array(std::string_view field, const JsonProducer& produce) const;
    int fail(int exit_code, std::string_view message, const std::filesystem::path& path = {}) const;
};

[[nodiscard]] std::filesystem::path utf8_path(std::string_view text);
[[nodiscard]] std::string path_text(const std::filesystem::path& path);
[[nodiscard]] int error_exit(ErrorCode code) noexcept;
[[nodiscard]] Json snapshot_json(const SnapshotSummary& snapshot);
[[nodiscard]] Json entry_json(const EntryInfo& entry);
[[nodiscard]] Json warnings_json(const std::vector<SkippedEntry>& warnings);
[[nodiscard]] Json stats_json(const RepositoryStats& stats);

void register_basic_commands(CLI::App& app, Context& context, Dispatcher& dispatch);
void register_maintenance_commands(CLI::App& app, Context& context, Dispatcher& dispatch);

} // namespace localvault::cli
