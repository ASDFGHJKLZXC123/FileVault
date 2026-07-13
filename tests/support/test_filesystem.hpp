#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace localvault::test {

class TemporaryDirectory final {
  public:
    TemporaryDirectory();
    ~TemporaryDirectory() noexcept;

    TemporaryDirectory(TemporaryDirectory&& other) noexcept;
    TemporaryDirectory& operator=(TemporaryDirectory&& other) noexcept;

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    void release() noexcept;

  private:
    void remove() noexcept;

    std::filesystem::path path_;
    bool released_{false};
};

class DatasetBuilder final {
  public:
    explicit DatasetBuilder(std::filesystem::path root);

    DatasetBuilder& directory(std::string_view relative_path);
    DatasetBuilder& text_file(std::string_view relative_path, std::string_view contents);
    DatasetBuilder& binary_file(std::string_view relative_path,
                                std::span<const std::byte> contents);
    DatasetBuilder& repeated_file(std::string_view relative_path, std::size_t size,
                                  std::byte value);
    DatasetBuilder& symlink(std::string_view relative_path, std::string_view target);

  private:
    [[nodiscard]] std::filesystem::path destination(std::string_view relative_path) const;

    std::filesystem::path root_;
};

enum class PosixModePolicy {
    ignore,
    require_equal,
};

struct MetadataPolicy {
    bool compare_modification_time{true};
    PosixModePolicy posix_mode{PosixModePolicy::ignore};
};

[[nodiscard]] std::vector<std::byte> read_all_bytes(const std::filesystem::path& path);
void expect_file_bytes_equal(const std::filesystem::path& first,
                             const std::filesystem::path& second);
void expect_tree_equal(const std::filesystem::path& source, const std::filesystem::path& restored,
                       MetadataPolicy metadata_policy = {});
void corrupt_byte(const std::filesystem::path& path, std::uintmax_t offset);
void truncate_file(const std::filesystem::path& path, std::uintmax_t size);

} // namespace localvault::test
