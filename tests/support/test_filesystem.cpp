#include "support/test_filesystem.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <limits>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace localvault::test {
namespace {

constexpr std::size_t kBufferSize = 64U * 1024U;

[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view value) {
    std::u8string utf8;
    utf8.reserve(value.size());
    for (const char character : value) {
        utf8.push_back(static_cast<char8_t>(character));
    }
    return std::filesystem::path(utf8);
}

[[nodiscard]] std::runtime_error file_error(std::string_view action,
                                            const std::filesystem::path& path) {
    return std::runtime_error(std::string(action) + ": " + path.string());
}

void create_parent_directories(const std::filesystem::path& path) {
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }
}

void write_bytes(const std::filesystem::path& path, std::span<const std::byte> contents) {
    create_parent_directories(path);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw file_error("cannot create test file", path);
    }
    std::size_t offset = 0;
    while (offset < contents.size()) {
        const std::size_t count = std::min(kBufferSize, contents.size() - offset);
        output.write(reinterpret_cast<const char*>(contents.data() + offset),
                     static_cast<std::streamsize>(count));
        if (!output) {
            throw file_error("cannot write test file", path);
        }
        offset += count;
    }
}

[[nodiscard]] std::set<std::filesystem::path> tree_entries(const std::filesystem::path& root) {
    std::set<std::filesystem::path> entries;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        entries.insert(entry.path().lexically_relative(root));
    }
    return entries;
}

[[nodiscard]] const char* type_name(std::filesystem::file_type type) {
    switch (type) {
    case std::filesystem::file_type::regular:
        return "regular file";
    case std::filesystem::file_type::directory:
        return "directory";
    case std::filesystem::file_type::symlink:
        return "symbolic link";
    case std::filesystem::file_type::not_found:
        return "missing entry";
    default:
        return "other entry";
    }
}

void expect_entry_metadata_equal(const std::filesystem::path& source,
                                 const std::filesystem::path& restored,
                                 std::filesystem::file_type type, const MetadataPolicy& policy) {
    if (type != std::filesystem::file_type::regular &&
        type != std::filesystem::file_type::directory) {
        return;
    }
    if (policy.compare_modification_time &&
        std::filesystem::last_write_time(source) != std::filesystem::last_write_time(restored)) {
        ADD_FAILURE() << "modification times differ for " << source;
    }
    if (policy.posix_mode == PosixModePolicy::require_equal) {
        const auto source_mode =
            std::filesystem::symlink_status(source).permissions() & std::filesystem::perms::mask;
        const auto restored_mode =
            std::filesystem::symlink_status(restored).permissions() & std::filesystem::perms::mask;
        if (source_mode != restored_mode) {
            ADD_FAILURE() << "POSIX modes differ for " << source;
        }
    }
}

} // namespace

TemporaryDirectory::TemporaryDirectory() {
    static std::atomic<std::uint64_t> sequence{0};
    std::random_device random;
    const std::filesystem::path temporary_root = std::filesystem::temp_directory_path();
    for (int attempt = 0; attempt < 100; ++attempt) {
        const std::string name = "localvault-test-" + std::to_string(random()) + "-" +
                                 std::to_string(sequence.fetch_add(1));
        std::error_code error;
        if (std::filesystem::create_directory(temporary_root / name, error)) {
            path_ = temporary_root / name;
            return;
        }
        if (error) {
            throw std::filesystem::filesystem_error("cannot create temporary test directory",
                                                    temporary_root / name, error);
        }
    }
    throw std::runtime_error("cannot create a unique temporary test directory");
}

TemporaryDirectory::~TemporaryDirectory() noexcept {
    remove();
}

TemporaryDirectory::TemporaryDirectory(TemporaryDirectory&& other) noexcept
    : path_(std::move(other.path_)), released_(std::exchange(other.released_, true)) {}

TemporaryDirectory& TemporaryDirectory::operator=(TemporaryDirectory&& other) noexcept {
    if (this != &other) {
        remove();
        path_ = std::move(other.path_);
        released_ = std::exchange(other.released_, true);
    }
    return *this;
}

const std::filesystem::path& TemporaryDirectory::path() const noexcept {
    return path_;
}

void TemporaryDirectory::release() noexcept {
    released_ = true;
}

void TemporaryDirectory::remove() noexcept {
    if (released_ || path_.empty()) {
        return;
    }
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
}

DatasetBuilder::DatasetBuilder(std::filesystem::path root) : root_(std::move(root)) {
    std::filesystem::create_directories(root_);
}

DatasetBuilder& DatasetBuilder::directory(std::string_view relative_path) {
    std::filesystem::create_directories(destination(relative_path));
    return *this;
}

DatasetBuilder& DatasetBuilder::text_file(std::string_view relative_path,
                                          std::string_view contents) {
    const auto* data = reinterpret_cast<const std::byte*>(contents.data());
    write_bytes(destination(relative_path), std::span<const std::byte>(data, contents.size()));
    return *this;
}

DatasetBuilder& DatasetBuilder::binary_file(std::string_view relative_path,
                                            std::span<const std::byte> contents) {
    write_bytes(destination(relative_path), contents);
    return *this;
}

DatasetBuilder& DatasetBuilder::repeated_file(std::string_view relative_path, std::size_t size,
                                              std::byte value) {
    const std::filesystem::path path = destination(relative_path);
    create_parent_directories(path);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw file_error("cannot create repeated test file", path);
    }
    std::array<char, kBufferSize> buffer{};
    buffer.fill(static_cast<char>(std::to_integer<unsigned char>(value)));
    std::size_t remaining = size;
    while (remaining > 0) {
        const std::size_t count = std::min(buffer.size(), remaining);
        output.write(buffer.data(), static_cast<std::streamsize>(count));
        if (!output) {
            throw file_error("cannot write repeated test file", path);
        }
        remaining -= count;
    }
    return *this;
}

DatasetBuilder& DatasetBuilder::symlink(std::string_view relative_path, std::string_view target) {
    const std::filesystem::path path = destination(relative_path);
    create_parent_directories(path);
    std::filesystem::create_symlink(path_from_utf8(target), path);
    return *this;
}

std::filesystem::path DatasetBuilder::destination(std::string_view relative_path) const {
    return root_ / path_from_utf8(relative_path);
}

std::vector<std::byte> read_all_bytes(const std::filesystem::path& path) {
    const std::uintmax_t file_size = std::filesystem::file_size(path);
    if (file_size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        throw file_error("test file is too large to read into memory", path);
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(file_size));
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw file_error("cannot open test file", path);
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const std::size_t count = std::min(kBufferSize, bytes.size() - offset);
        input.read(reinterpret_cast<char*>(bytes.data() + offset),
                   static_cast<std::streamsize>(count));
        if (input.gcount() != static_cast<std::streamsize>(count)) {
            throw file_error("cannot read complete test file", path);
        }
        offset += count;
    }
    return bytes;
}

void expect_file_bytes_equal(const std::filesystem::path& first,
                             const std::filesystem::path& second) {
    const std::uintmax_t first_size = std::filesystem::file_size(first);
    const std::uintmax_t second_size = std::filesystem::file_size(second);
    if (first_size != second_size) {
        ADD_FAILURE() << "file sizes differ: " << first << " and " << second;
        return;
    }

    std::ifstream first_input(first, std::ios::binary);
    std::ifstream second_input(second, std::ios::binary);
    if (!first_input || !second_input) {
        ADD_FAILURE() << "cannot open files for comparison: " << first << " and " << second;
        return;
    }
    std::array<char, kBufferSize> first_buffer{};
    std::array<char, kBufferSize> second_buffer{};
    std::uintmax_t offset = 0;
    while (offset < first_size) {
        const std::uintmax_t remaining = first_size - offset;
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uintmax_t>(remaining, static_cast<std::uintmax_t>(kBufferSize)));
        first_input.read(first_buffer.data(), static_cast<std::streamsize>(count));
        second_input.read(second_buffer.data(), static_cast<std::streamsize>(count));
        if (first_input.gcount() != static_cast<std::streamsize>(count) ||
            second_input.gcount() != static_cast<std::streamsize>(count)) {
            ADD_FAILURE() << "cannot read files for comparison: " << first << " and " << second;
            return;
        }
        if (!std::equal(first_buffer.begin(), first_buffer.begin() + count,
                        second_buffer.begin())) {
            ADD_FAILURE() << "file bytes differ at or after offset " << offset << ": " << first
                          << " and " << second;
            return;
        }
        offset += count;
    }
}

void expect_tree_equal(const std::filesystem::path& source, const std::filesystem::path& restored,
                       MetadataPolicy metadata_policy) {
#ifdef _WIN32
    if (metadata_policy.posix_mode == PosixModePolicy::require_equal) {
        ADD_FAILURE() << "POSIX mode comparison is unavailable on Windows";
        return;
    }
#endif
    if (std::filesystem::symlink_status(source).type() != std::filesystem::file_type::directory ||
        std::filesystem::symlink_status(restored).type() != std::filesystem::file_type::directory) {
        ADD_FAILURE() << "both tree roots must be directories";
        return;
    }

    const auto source_entries = tree_entries(source);
    const auto restored_entries = tree_entries(restored);
    if (source_entries != restored_entries) {
        ADD_FAILURE() << "tree entry sets differ: " << source << " and " << restored;
        return;
    }

    expect_entry_metadata_equal(source, restored, std::filesystem::file_type::directory,
                                metadata_policy);
    for (const auto& relative_path : source_entries) {
        const std::filesystem::path source_path = source / relative_path;
        const std::filesystem::path restored_path = restored / relative_path;
        const auto source_type = std::filesystem::symlink_status(source_path).type();
        const auto restored_type = std::filesystem::symlink_status(restored_path).type();
        if (source_type != restored_type) {
            ADD_FAILURE() << "entry types differ for " << relative_path << ": "
                          << type_name(source_type) << " and " << type_name(restored_type);
            continue;
        }
        if (source_type == std::filesystem::file_type::regular) {
            expect_file_bytes_equal(source_path, restored_path);
        } else if (source_type == std::filesystem::file_type::symlink) {
            const auto source_target = std::filesystem::read_symlink(source_path);
            const auto restored_target = std::filesystem::read_symlink(restored_path);
            if (source_target != restored_target) {
                ADD_FAILURE() << "symbolic-link targets differ for " << relative_path;
            }
        }
        expect_entry_metadata_equal(source_path, restored_path, source_type, metadata_policy);
    }
}

void corrupt_byte(const std::filesystem::path& path, std::uintmax_t offset) {
    if (offset >= std::filesystem::file_size(path) ||
        offset > static_cast<std::uintmax_t>(std::numeric_limits<std::streamoff>::max())) {
        throw std::out_of_range("test corruption offset is outside the file");
    }
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file) {
        throw file_error("cannot open test file for corruption", path);
    }
    file.seekg(static_cast<std::streamoff>(offset));
    char value{};
    file.read(&value, 1);
    if (!file) {
        throw file_error("cannot read test byte for corruption", path);
    }
    const auto changed = static_cast<unsigned char>(value) ^ 1U;
    value = static_cast<char>(changed);
    file.seekp(static_cast<std::streamoff>(offset));
    file.write(&value, 1);
    file.flush();
    if (!file) {
        throw file_error("cannot write corrupted test byte", path);
    }
}

void truncate_file(const std::filesystem::path& path, std::uintmax_t size) {
    std::filesystem::resize_file(path, size);
}

} // namespace localvault::test
