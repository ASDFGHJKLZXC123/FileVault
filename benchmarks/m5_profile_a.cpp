#include <sys/resource.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "localvault/repository.hpp"
#include "localvault/snapshot_engine.hpp"

namespace {

constexpr std::uint64_t expected_file_count = 50'000;
constexpr std::uint64_t rss_ceiling_bytes = 512ULL * 1024ULL * 1024ULL;

[[nodiscard]] std::uint64_t peak_rss_bytes() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        throw std::runtime_error("getrusage failed");
    }
    return static_cast<std::uint64_t>(usage.ru_maxrss);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: localvault_m5_profile_a <dataset-root> <repository-root>\n";
        return 2;
    }

    try {
        const std::filesystem::path source = argv[1];
        const std::filesystem::path repository_root = argv[2];
        localvault::Repository::create(repository_root);
        localvault::Repository repository =
            localvault::Repository::open(repository_root, localvault::OpenMode::read_write);
        const auto started = std::chrono::steady_clock::now();
        const localvault::SnapshotResult result =
            localvault::SnapshotEngine(repository)
                .create_snapshot(source, localvault::SnapshotOptions{.worker_count = 16});
        const auto elapsed = std::chrono::steady_clock::now() - started;
        const std::uint64_t rss = peak_rss_bytes();

        nlohmann::json output{
            {"schema_version", 1},
            {"profile", "many-small"},
            {"file_count", result.file_count},
            {"logical_bytes", result.logical_bytes},
            {"new_chunks", result.new_chunks},
            {"reused_chunks", result.reused_chunks},
            {"snapshot_seconds", std::chrono::duration<double>(elapsed).count()},
            {"peak_rss_bytes", rss},
            {"rss_ceiling_bytes", rss_ceiling_bytes},
            {"worker_count", 16},
        };
        std::cout << output.dump() << '\n';
        if (result.file_count != expected_file_count) {
            std::cerr << "expected exactly 50000 files\n";
            return 1;
        }
        if (rss > rss_ceiling_bytes) {
            std::cerr << "peak RSS exceeded the 512 MiB acceptance ceiling\n";
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "profile-A acceptance failed: " << error.what() << '\n';
        return 1;
    }
}
