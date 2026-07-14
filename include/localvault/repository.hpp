#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "localvault/failure_injector.hpp"

namespace localvault {

class Database;

struct RepositoryCreateOptions {
    std::uint64_t chunk_size_bytes{4ULL * 1024ULL * 1024ULL};
    int zstd_level{3};
    bool allow_risky_filesystem{false};
    bool allow_existing_non_empty{false};
};

struct RepositoryInfo {
    std::string repository_uuid;
    std::uint32_t format_version{};
    std::uint64_t chunk_size_bytes{};
    int zstd_level{};
    std::string hash_algorithm;
};

enum class OpenMode { read_only, read_write };

class Repository final {
  public:
    static void create(const std::filesystem::path& root,
                       const RepositoryCreateOptions& options = {});

    static Repository open(const std::filesystem::path& root, OpenMode mode = OpenMode::read_write);

    Repository(Repository&&) noexcept;
    Repository& operator=(Repository&&) noexcept;
    ~Repository();

    Repository(const Repository&) = delete;
    Repository& operator=(const Repository&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const noexcept;
    [[nodiscard]] std::uint32_t format_version() const noexcept;
    [[nodiscard]] const RepositoryInfo& info() const noexcept;

    void set_failure_injector(std::shared_ptr<FailureInjector> injector);

  private:
    class Impl;
    explicit Repository(std::unique_ptr<Impl> impl);
    [[nodiscard]] Database& database() noexcept;
    [[nodiscard]] OpenMode open_mode() const noexcept;
    [[nodiscard]] std::shared_ptr<FailureInjector> failure_injector() const noexcept;
    void recover_after_writer_lock();
    void set_recovery_entry_batch_limit_for_testing(std::size_t entry_batch_limit);
    std::unique_ptr<Impl> impl_;

    friend class SnapshotEngine;
    friend class RestoreEngine;
    friend class DiffEngine;
    friend class IntegrityVerifier;
    friend class GarbageCollector;
    friend class QueryService;
};

} // namespace localvault
