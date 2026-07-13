#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "filesystem/file_scanner.hpp"
#include "localvault/error.hpp"
#include "localvault/types.hpp"
#include "support/test_filesystem.hpp"

namespace localvault {
namespace {

[[nodiscard]] const ScannedEntry* find_entry(const ScanResult& result, std::string_view path) {
    const auto iterator = std::ranges::find(result.entries, path, &ScannedEntry::relative_path);
    return iterator == result.entries.end() ? nullptr : &*iterator;
}

[[nodiscard]] std::vector<std::string> scanned_paths(const ScanResult& result) {
    std::vector<std::string> paths;
    paths.reserve(result.entries.size());
    for (const ScannedEntry& entry : result.entries) {
        paths.push_back(entry.relative_path);
    }
    return paths;
}

[[nodiscard]] std::string path_utf8(const std::filesystem::path& path) {
    const std::u8string encoded = path.generic_u8string();
    std::string result;
    result.reserve(encoded.size());
    for (const char8_t character : encoded) {
        result.push_back(static_cast<char>(character));
    }
    return result;
}

TEST(FileScannerTest, EmitsRootFilesDirectoriesHiddenAndExactUnicodePaths) {
    test::TemporaryDirectory temporary;
    const std::filesystem::path source = temporary.path() / "source";
    const std::string unicode_name = "caf\xC3\xA9.txt";
    test::DatasetBuilder(source)
        .directory("empty")
        .text_file("nested/file with spaces.txt", "contents")
        .text_file(".hidden", "hidden")
        .text_file(unicode_name, "Unicode");

    std::string on_disk_unicode_name;
    for (const std::filesystem::directory_entry& candidate :
         std::filesystem::directory_iterator(source)) {
        if (candidate.is_regular_file() && candidate.path().filename() != ".hidden") {
            on_disk_unicode_name = path_utf8(candidate.path().filename());
        }
    }
    ASSERT_FALSE(on_disk_unicode_name.empty());

    const auto requested_time = std::chrono::file_clock::now() + std::chrono::nanoseconds{123};
    std::filesystem::last_write_time(source / "nested/file with spaces.txt", requested_time);
    const auto actual_file_time =
        std::filesystem::last_write_time(source / "nested/file with spaces.txt");
    const auto system_time = std::chrono::file_clock::to_sys(actual_file_time);
    const auto round_trip = std::chrono::file_clock::from_sys(system_time);
    const std::int64_t expected_time =
        std::chrono::duration_cast<std::chrono::nanoseconds>(system_time.time_since_epoch())
            .count() +
        std::chrono::duration_cast<std::chrono::nanoseconds>(actual_file_time - round_trip).count();

    const ScanResult result = FileScanner{}.scan(source);

    ASSERT_FALSE(result.entries.empty());
    EXPECT_TRUE(result.warnings.empty());
    const ScannedEntry& root = result.entries.front();
    EXPECT_EQ(root.source_path, std::filesystem::absolute(source).lexically_normal());
    EXPECT_EQ(root.relative_path, "");
    EXPECT_EQ(root.parent_path, "");
    EXPECT_EQ(root.name, "");
    EXPECT_EQ(root.type, EntryType::directory);

    const ScannedEntry* file = find_entry(result, "nested/file with spaces.txt");
    ASSERT_NE(file, nullptr);
    EXPECT_EQ(file->parent_path, "nested");
    EXPECT_EQ(file->name, "file with spaces.txt");
    EXPECT_EQ(file->type, EntryType::regular_file);
    EXPECT_EQ(file->logical_size, 8U);
    EXPECT_EQ(file->modified_time_ns, expected_time);
    EXPECT_FALSE(file->symlink_target.has_value());
#ifdef _WIN32
    EXPECT_EQ(file->posix_mode, 0U);
#else
    EXPECT_NE(file->posix_mode, 0U);
#endif

    EXPECT_NE(find_entry(result, ".hidden"), nullptr);
    EXPECT_NE(find_entry(result, "empty"), nullptr);
    EXPECT_NE(find_entry(result, on_disk_unicode_name), nullptr);

    const std::vector<std::string> first_paths = scanned_paths(result);
    const std::vector<std::string> second_paths = scanned_paths(FileScanner{}.scan(source));
    EXPECT_EQ(first_paths, second_paths);
}

TEST(FileScannerTest, CapturesBrokenAndDirectorySymlinksWithoutFollowingThem) {
    test::TemporaryDirectory temporary;
    const std::filesystem::path source = temporary.path() / "source";
    test::DatasetBuilder(source).text_file("real/inside.txt", "content");
    std::error_code error;
    std::filesystem::create_symlink("missing-target", source / "broken", error);
    if (error) {
        GTEST_SKIP() << "symbolic-link creation is unavailable: " << error.message();
    }
    std::filesystem::create_directory_symlink("real", source / "linked-directory", error);
    if (error) {
        GTEST_SKIP() << "directory symbolic-link creation is unavailable: " << error.message();
    }

    const ScanResult result = FileScanner{}.scan(source);

    EXPECT_TRUE(result.warnings.empty());
    const ScannedEntry* broken = find_entry(result, "broken");
    ASSERT_NE(broken, nullptr);
    EXPECT_EQ(broken->type, EntryType::symbolic_link);
    ASSERT_TRUE(broken->symlink_target.has_value());
    EXPECT_EQ(*broken->symlink_target, "missing-target");
    EXPECT_GT(broken->modified_time_ns, 0);

    const ScannedEntry* directory_link = find_entry(result, "linked-directory");
    ASSERT_NE(directory_link, nullptr);
    EXPECT_EQ(directory_link->type, EntryType::symbolic_link);
    EXPECT_EQ(directory_link->symlink_target, "real");
    EXPECT_EQ(find_entry(result, "linked-directory/inside.txt"), nullptr);
    EXPECT_NE(find_entry(result, "real/inside.txt"), nullptr);
}

#ifndef _WIN32
TEST(FileScannerTest, WarnsAndSkipsSpecialEntriesAndInvalidUtf8WhereSupported) {
    test::TemporaryDirectory temporary;
    const std::filesystem::path source = temporary.path() / "source";
    std::filesystem::create_directory(source);
    ASSERT_EQ(::mkfifo((source / "named-pipe").c_str(), 0600), 0);
#if defined(__linux__)
    const std::filesystem::path invalid_name = source / std::filesystem::path("bad-\xFF");
    std::ofstream invalid_file(invalid_name);
    ASSERT_TRUE(invalid_file);
    invalid_file << "unrepresentable";
    invalid_file.close();
#endif

    const ScanResult result = FileScanner{}.scan(source);

    EXPECT_EQ(find_entry(result, "named-pipe"), nullptr);
#if defined(__linux__)
    constexpr std::size_t expected_warning_count = 2;
#else
    constexpr std::size_t expected_warning_count = 1;
#endif
    EXPECT_EQ(result.warnings.size(), expected_warning_count);
    EXPECT_NE(std::ranges::find(result.warnings, "unsupported_file_type", &ScanWarning::code),
              result.warnings.end());
#if defined(__linux__)
    EXPECT_NE(std::ranges::find(result.warnings, "unsupported_path_encoding", &ScanWarning::code),
              result.warnings.end());
#endif
}
#endif

TEST(FileScannerTest, RejectsASymlinkAsTheSourceRoot) {
    test::TemporaryDirectory temporary;
    const std::filesystem::path directory = temporary.path() / "directory";
    std::filesystem::create_directory(directory);
    const std::filesystem::path link = temporary.path() / "source-link";
    std::error_code error;
    std::filesystem::create_directory_symlink(directory, link, error);
    if (error) {
        GTEST_SKIP() << "symbolic-link creation is unavailable: " << error.message();
    }

    try {
        (void)FileScanner{}.scan(link);
        FAIL() << "a symbolic-link source root should be rejected";
    } catch (const LocalVaultError& caught) {
        EXPECT_EQ(caught.code(), ErrorCode::invalid_argument);
    }
}

} // namespace
} // namespace localvault
