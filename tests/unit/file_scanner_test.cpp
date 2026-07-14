#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <sys/stat.h>
#endif

#include "filesystem/file_scanner.hpp"
#include "filesystem/platform/file_metadata.hpp"
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

void write_text(const std::filesystem::path& path, std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output) << path;
    output << contents;
    ASSERT_TRUE(output) << path;
}

#ifdef _WIN32
[[nodiscard]] bool create_junction(const std::filesystem::path& junction,
                                   const std::filesystem::path& target) {
    std::wstring command =
        L"cmd.exe /D /C mklink /J \"" + junction.native() + L"\" \"" + target.native() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = static_cast<DWORD>(sizeof(startup));
    PROCESS_INFORMATION process{};
    if (::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                         nullptr, nullptr, &startup, &process) == 0) {
        return false;
    }
    const DWORD wait_result = ::WaitForSingleObject(process.hProcess, 30'000);
    DWORD exit_code = 1;
    const bool completed =
        wait_result == WAIT_OBJECT_0 && ::GetExitCodeProcess(process.hProcess, &exit_code) != 0;
    (void)::CloseHandle(process.hThread);
    (void)::CloseHandle(process.hProcess);
    return completed && exit_code == 0;
}
#endif

TEST(FileScannerDecisionTest, FakedPlatformAttributesChooseSafeActions) {
    const FileScannerOptions options;
    EXPECT_EQ(decide_scan_entry({.kind = ScanEntryKind::ordinary}, options).action,
              ScanEntryAction::capture);
    EXPECT_EQ(decide_scan_entry({.kind = ScanEntryKind::symbolic_link}, options).action,
              ScanEntryAction::capture_as_symbolic_link);
    EXPECT_EQ(decide_scan_entry({.kind = ScanEntryKind::junction}, options).action,
              ScanEntryAction::capture_as_symbolic_link);

    const ScanEntryDecision mount =
        decide_scan_entry({.kind = ScanEntryKind::volume_mount_point}, options);
    EXPECT_EQ(mount.action, ScanEntryAction::warn_and_skip);
    EXPECT_EQ(mount.warning_code, "volume_mount_point");

    const ScanEntryDecision cloud = decide_scan_entry({.cloud_placeholder = true}, options);
    EXPECT_EQ(cloud.action, ScanEntryAction::warn_and_skip);
    EXPECT_EQ(cloud.warning_code, "cloud_placeholder");
}

TEST(FileScannerDecisionTest, HiddenAndFilesystemBoundaryOptionsAreAppliedToFakeFacts) {
    EXPECT_EQ(decide_scan_entry({.hidden = true}, FileScannerOptions{}).action,
              ScanEntryAction::capture);
    EXPECT_EQ(
        decide_scan_entry({.hidden = true}, FileScannerOptions{.include_hidden = false}).action,
        ScanEntryAction::skip);

    const ScanEntryFacts posix_device_boundary{.filesystem_boundary = true};
    const ScanEntryFacts windows_volume_boundary{.filesystem_boundary = true};
    EXPECT_EQ(decide_scan_entry(posix_device_boundary, FileScannerOptions{}).action,
              ScanEntryAction::capture);
    for (const ScanEntryFacts facts : {posix_device_boundary, windows_volume_boundary}) {
        const ScanEntryDecision decision =
            decide_scan_entry(facts, FileScannerOptions{.one_file_system = true});
        EXPECT_EQ(decision.action, ScanEntryAction::warn_and_skip);
        EXPECT_EQ(decision.warning_code, "filesystem_boundary");
    }
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

    const std::filesystem::file_time_type requested_time =
        std::filesystem::file_time_type::clock::now() +
        std::filesystem::file_time_type::duration{1};
    const std::filesystem::path timed_file = source / "nested/file with spaces.txt";
    std::filesystem::last_write_time(timed_file, requested_time);
    const std::int64_t expected_time =
        read_platform_file_metadata_no_follow(timed_file).modified_time_ns;

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

TEST(FileScannerTest, CollectingScanReturnsCanonicalRepositoryRelativePathOrder) {
    test::TemporaryDirectory temporary;
    const std::filesystem::path source = temporary.path() / "source";
    test::DatasetBuilder(source)
        .text_file("zeta.txt", "created first")
        .text_file("middle.txt", "created second")
        .text_file("alpha/deep.txt", "created last");

    EXPECT_EQ(scanned_paths(FileScanner{}.scan(source)),
              (std::vector<std::string>{"", "alpha", "alpha/deep.txt", "middle.txt", "zeta.txt"}));
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

TEST(FileScannerTest, HiddenEntriesAreIncludedByDefaultAndPrunedWhenDisabled) {
    test::TemporaryDirectory temporary;
    const std::filesystem::path source = temporary.path() / "source";
    test::DatasetBuilder(source)
        .text_file("visible.txt", "visible")
        .text_file(".hidden-file", "hidden")
        .text_file(".hidden-directory/inside.txt", "hidden nested");
#ifdef _WIN32
    for (const std::filesystem::path& hidden_path :
         {source / ".hidden-file", source / ".hidden-directory"}) {
        const DWORD attributes = ::GetFileAttributesW(hidden_path.c_str());
        ASSERT_NE(attributes, INVALID_FILE_ATTRIBUTES);
        ASSERT_NE(::SetFileAttributesW(hidden_path.c_str(), attributes | FILE_ATTRIBUTE_HIDDEN), 0);
    }
#endif

    const ScanResult included = FileScanner{}.scan(source);
    EXPECT_NE(find_entry(included, ".hidden-file"), nullptr);
    EXPECT_NE(find_entry(included, ".hidden-directory/inside.txt"), nullptr);

    const ScanResult pruned =
        FileScanner{}.scan(source, FileScannerOptions{.include_hidden = false});
    EXPECT_NE(find_entry(pruned, "visible.txt"), nullptr);
    EXPECT_EQ(find_entry(pruned, ".hidden-file"), nullptr);
    EXPECT_EQ(find_entry(pruned, ".hidden-directory"), nullptr);
    EXPECT_EQ(find_entry(pruned, ".hidden-directory/inside.txt"), nullptr);
    EXPECT_TRUE(pruned.warnings.empty());
}

TEST(FileScannerTest, IgnoreRulesPruneDirectoriesBeforeEnumeratingTheirChildren) {
    test::TemporaryDirectory temporary;
    const std::filesystem::path source = temporary.path() / "source";
    test::DatasetBuilder(source)
        .text_file("kept.txt", "kept")
        .text_file("ignored.txt", "ignored")
        .text_file("nested/drop.tmp", "ignored wildcard")
        .text_file("ignored-directory/child-that-would-not-match.txt", "must not enumerate");
    write_text(source / ".localvaultignore", "ignored.txt\nnested/*.tmp\nignored-directory/\n");

    const ScanResult result = FileScanner{}.scan(source);

    EXPECT_NE(find_entry(result, "kept.txt"), nullptr);
    EXPECT_NE(find_entry(result, ".localvaultignore"), nullptr);
    EXPECT_EQ(find_entry(result, "ignored.txt"), nullptr);
    EXPECT_EQ(find_entry(result, "nested/drop.tmp"), nullptr);
    EXPECT_EQ(find_entry(result, "ignored-directory"), nullptr);
    EXPECT_EQ(find_entry(result, "ignored-directory/child-that-would-not-match.txt"), nullptr);
    EXPECT_TRUE(result.warnings.empty());
}

TEST(FileScannerTest, ExplicitIgnoreFileReplacesRootRules) {
    test::TemporaryDirectory temporary;
    const std::filesystem::path source = temporary.path() / "source";
    const std::filesystem::path replacement = temporary.path() / "replacement.ignore";
    test::DatasetBuilder(source)
        .text_file("root-only.txt", "kept by replacement")
        .text_file("replacement-only.txt", "ignored by replacement");
    write_text(source / ".localvaultignore", "root-only.txt\n");
    write_text(replacement, "replacement-only.txt\n");

    const ScanResult result =
        FileScanner{}.scan(source, FileScannerOptions{.ignore_file = replacement});

    EXPECT_NE(find_entry(result, "root-only.txt"), nullptr);
    EXPECT_EQ(find_entry(result, "replacement-only.txt"), nullptr);
}

#ifdef _WIN32
TEST(FileScannerTest, NativeWindowsJunctionIsCapturedAndNeverTraversed) {
    test::TemporaryDirectory temporary;
    std::filesystem::path source;
    std::string junction_name;
    std::string nested_name;
    char* configured_source = nullptr;
    std::size_t configured_source_size = 0;
    ASSERT_EQ(_dupenv_s(&configured_source, &configured_source_size,
                        "LOCALVAULT_M5_JUNCTION_LOOP_SOURCE"),
              0);
    const std::unique_ptr<char, decltype(&std::free)> configured_source_owner(configured_source,
                                                                              &std::free);
    static_cast<void>(configured_source_owner);
    if (configured_source != nullptr && configured_source[0] != '\0') {
        source = configured_source;
        junction_name = "loop";
        nested_name = "loop/file.txt";
        ASSERT_TRUE(std::filesystem::is_regular_file(source / "file.txt"));
    } else {
        source = temporary.path() / "source";
        junction_name = "junction";
        nested_name = "junction/inside.txt";
    }
    const std::filesystem::path target = temporary.path() / "target";
    if (configured_source == nullptr || configured_source[0] == '\0') {
        test::DatasetBuilder(source).text_file("ordinary.txt", "ordinary");
        test::DatasetBuilder(target).text_file("inside.txt", "must not traverse");
        if (!create_junction(source / junction_name, target)) {
            GTEST_SKIP() << "mklink /J is unavailable in this Windows environment";
        }
    }

    const ScanResult result = FileScanner{}.scan(source);

    const ScannedEntry* entry = find_entry(result, junction_name);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->type, EntryType::symbolic_link);
    EXPECT_TRUE(entry->symlink_target.has_value());
    EXPECT_FALSE(entry->symlink_target->empty());
    EXPECT_EQ(find_entry(result, nested_name), nullptr);
}
#endif

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
