#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

namespace localvault {

struct PlatformFileMetadata {
    std::uint64_t logical_size{};
    std::int64_t modified_time_ns{};
    std::uint32_t posix_mode{};
};

enum class RestorePublishResult {
    published,
    destination_exists,
};

class TemporaryOutputFile final {
  public:
    explicit TemporaryOutputFile(const std::filesystem::path& directory);
    ~TemporaryOutputFile() noexcept;

    TemporaryOutputFile(TemporaryOutputFile&&) noexcept;
    TemporaryOutputFile& operator=(TemporaryOutputFile&&) noexcept;

    TemporaryOutputFile(const TemporaryOutputFile&) = delete;
    TemporaryOutputFile& operator=(const TemporaryOutputFile&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    void write(std::span<const std::byte> bytes);
    void apply_metadata(std::int64_t modified_time_ns, std::uint32_t posix_mode);
    void sync();
    [[nodiscard]] RestorePublishResult publish(const std::filesystem::path& destination_path,
                                               bool replace_existing);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] PlatformFileMetadata
read_platform_file_metadata_no_follow(const std::filesystem::path& path);
void apply_restored_metadata(const std::filesystem::path& path, std::int64_t modified_time_ns,
                             std::uint32_t posix_mode);
[[nodiscard]] bool create_restored_symlink(const std::filesystem::path& target,
                                           const std::filesystem::path& link_path);
[[nodiscard]] RestorePublishResult
publish_restored_path(const std::filesystem::path& temporary_path,
                      const std::filesystem::path& destination_path, bool replace_existing);

} // namespace localvault
