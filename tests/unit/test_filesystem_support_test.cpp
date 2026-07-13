#include <gtest/gtest.h>
#ifdef _WIN32
#include <gtest/gtest-spi.h>
#endif

#include <array>
#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "support/test_filesystem.hpp"

namespace localvault::test {
namespace {

TEST(TemporaryDirectoryTest, MovesCleansRecursivelyAndCanBeReleased) {
    std::filesystem::path automatically_removed;
    {
        TemporaryDirectory original;
        automatically_removed = original.path();
        DatasetBuilder(original.path()).text_file("nested/file.txt", "contents");
        TemporaryDirectory moved(std::move(original));
        EXPECT_EQ(moved.path(), automatically_removed);
        TemporaryDirectory assigned;
        const std::filesystem::path replaced = assigned.path();
        assigned = std::move(moved);
        EXPECT_EQ(assigned.path(), automatically_removed);
        EXPECT_FALSE(std::filesystem::exists(replaced));
    }
    EXPECT_FALSE(std::filesystem::exists(automatically_removed));

    std::filesystem::path released;
    {
        TemporaryDirectory temporary;
        released = temporary.path();
        temporary.release();
    }
    EXPECT_TRUE(std::filesystem::is_directory(released));
    std::filesystem::remove_all(released);
}

TEST(DatasetBuilderTest, CreatesEmptyBinaryRepeatedUnicodeAndSpaceEntries) {
    TemporaryDirectory temporary;
    constexpr std::array<std::byte, 4> binary{std::byte{0x00}, std::byte{0x01}, std::byte{0x00},
                                              std::byte{0xFF}};
    DatasetBuilder(temporary.path())
        .directory("empty directory")
        .text_file("empty file", "")
        .text_file("nested/file with spaces.txt", "hello")
        .binary_file("binary.dat", binary)
        .repeated_file("caf\xC3\xA9/repeated.bin", 70U * 1024U, std::byte{0xA5})
        .text_file("cafe\xCC\x81.txt", "nfd");

    EXPECT_TRUE(std::filesystem::is_empty(temporary.path() / "empty directory"));
    EXPECT_EQ(std::filesystem::file_size(temporary.path() / "empty file"), 0U);
    EXPECT_EQ(read_all_bytes(temporary.path() / "binary.dat"),
              (std::vector<std::byte>(binary.begin(), binary.end())));
    const std::filesystem::path nfc_path =
        temporary.path() / std::filesystem::path(u8"caf\u00E9/repeated.bin");
    const std::filesystem::path nfd_path =
        temporary.path() / std::filesystem::path(u8"cafe\u0301.txt");
    const auto repeated = read_all_bytes(nfc_path);
    ASSERT_EQ(repeated.size(), 70U * 1024U);
    EXPECT_EQ(repeated.front(), std::byte{0xA5});
    EXPECT_EQ(repeated.back(), std::byte{0xA5});
    EXPECT_TRUE(std::filesystem::is_regular_file(nfd_path));
    EXPECT_NE(nfc_path.parent_path().filename(), nfd_path.filename().stem());
}

TEST(FileMutationTest, CorruptsAndTruncatesAtRequestedOffsets) {
    TemporaryDirectory temporary;
    const std::filesystem::path path = temporary.path() / "data.bin";
    DatasetBuilder(temporary.path()).text_file("data.bin", "abcdef");

    corrupt_byte(path, 2);
    const auto corrupted = read_all_bytes(path);
    ASSERT_EQ(corrupted.size(), 6U);
    EXPECT_EQ(corrupted[2], std::byte{static_cast<unsigned char>('c') ^ 1U});
    truncate_file(path, 3);
    EXPECT_EQ(read_all_bytes(path).size(), 3U);
    EXPECT_THROW(corrupt_byte(path, 3), std::out_of_range);
}

TEST(TreeComparisonTest, ComparesEntryTypesStreamingBytesAndMetadata) {
    TemporaryDirectory temporary;
    const std::filesystem::path source = temporary.path() / "source";
    const std::filesystem::path restored = temporary.path() / "restored";
    DatasetBuilder(source)
        .directory("empty")
        .text_file("nested/file with spaces.txt", "same bytes")
        .repeated_file("large.bin", 3U * 64U * 1024U + 17U, std::byte{0x3C});
    DatasetBuilder(restored)
        .directory("empty")
        .text_file("nested/file with spaces.txt", "same bytes")
        .repeated_file("large.bin", 3U * 64U * 1024U + 17U, std::byte{0x3C});

#ifndef _WIN32
    ASSERT_EQ(::chmod((source / "nested/file with spaces.txt").c_str(), 0640), 0);
    ASSERT_EQ(::chmod((restored / "nested/file with spaces.txt").c_str(), 0640), 0);
#endif
    for (const auto& entry : std::filesystem::recursive_directory_iterator(source)) {
        if (!entry.is_symlink()) {
            const auto relative = entry.path().lexically_relative(source);
            std::filesystem::last_write_time(restored / relative,
                                             std::filesystem::last_write_time(entry.path()));
        }
    }
    std::filesystem::last_write_time(restored, std::filesystem::last_write_time(source));

    MetadataPolicy policy;
#ifndef _WIN32
    policy.posix_mode = PosixModePolicy::require_equal;
#endif
    expect_tree_equal(source, restored, policy);
}

TEST(TreeComparisonTest, ComparesBrokenSymlinkTextWithoutFollowingTarget) {
    TemporaryDirectory temporary;
    const std::filesystem::path source = temporary.path() / "source";
    const std::filesystem::path restored = temporary.path() / "restored";
    try {
        DatasetBuilder(source).symlink("broken link", "missing target");
        DatasetBuilder(restored).symlink("broken link", "missing target");
    } catch (const std::filesystem::filesystem_error&) {
#ifdef _WIN32
        GTEST_SKIP() << "creating symbolic links requires Windows developer mode or privilege";
#else
        throw;
#endif
    }

    MetadataPolicy policy;
    policy.compare_modification_time = false;
    expect_tree_equal(source, restored, policy);
}

#ifdef _WIN32
TEST(TreeComparisonTest, RejectsUnavailablePosixModeComparisonOnWindows) {
    TemporaryDirectory temporary;
    const std::filesystem::path source = temporary.path() / "source";
    const std::filesystem::path restored = temporary.path() / "restored";
    DatasetBuilder(source).directory("");
    DatasetBuilder(restored).directory("");
    MetadataPolicy policy;
    policy.posix_mode = PosixModePolicy::require_equal;

    EXPECT_NONFATAL_FAILURE(expect_tree_equal(source, restored, policy),
                            "POSIX mode comparison is unavailable on Windows");
}
#endif

} // namespace
} // namespace localvault::test
