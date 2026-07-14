#pragma once

#include "localvault/progress.hpp"
#include "localvault/types.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace localvault {

class Repository;

struct SnapshotOptions {
    std::string message{};
    std::size_t worker_count{};
    bool retry_unstable_files{true};
    bool force_rehash{false};
    bool include_hidden{true};
    bool one_file_system{false};
    std::optional<std::filesystem::path> ignore_file{};
};

struct SnapshotResult {
    SnapshotId snapshot_id{};
    std::uint64_t file_count{};
    std::uint64_t directory_count{};
    ByteCount logical_bytes{};
    ByteCount new_stored_bytes{};
    std::uint64_t new_chunks{};
    std::uint64_t reused_chunks{};
    std::vector<SkippedEntry> skipped_entries;
};

class SnapshotEngine final {
  public:
    explicit SnapshotEngine(Repository& repository);

    SnapshotResult create_snapshot(const std::filesystem::path& source_root,
                                   const SnapshotOptions& options, std::stop_token stop_token = {},
                                   ProgressCallback progress = {});

  private:
    friend class SnapshotEngineTestAccess;
    friend class M5PipelineTestAccess;

    static constexpr std::size_t default_metadata_batch_entry_limit = 500;
    static constexpr ByteCount default_metadata_batch_logical_byte_limit =
        64ULL * 1024ULL * 1024ULL;
    static constexpr std::size_t default_job_queue_capacity = 64;
    static constexpr std::size_t default_result_queue_capacity = 32;
    static constexpr std::size_t default_result_queue_byte_capacity = 64U * 1024U * 1024U;

    [[nodiscard]] static unsigned default_worker_count() noexcept;
    [[nodiscard]] static unsigned resolved_worker_count(std::size_t requested) noexcept;

    Repository& repository_;
    std::size_t metadata_batch_entry_limit_{default_metadata_batch_entry_limit};
    ByteCount metadata_batch_logical_byte_limit_{default_metadata_batch_logical_byte_limit};
    std::size_t job_queue_capacity_{default_job_queue_capacity};
    std::size_t result_queue_capacity_{default_result_queue_capacity};
    std::size_t result_queue_byte_capacity_{default_result_queue_byte_capacity};
    std::function<void()> scanner_test_hook_;
    std::function<void()> scanner_entry_test_hook_;
    std::function<void()> worker_test_hook_;
    std::function<void()> writer_test_hook_;
    std::function<void(bool)> object_stripe_test_hook_;
    std::function<void(const std::filesystem::path&, std::size_t)> file_read_test_hook_;
    std::function<std::chrono::steady_clock::time_point()> progress_clock_test_hook_;
};

} // namespace localvault
