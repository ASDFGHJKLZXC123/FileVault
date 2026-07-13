#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdlib>
#else
#include <sys/stat.h>
#endif

#include "database/database.hpp"
#include "database/metadata_store.hpp"
#include "database/statement.hpp"
#include "filesystem/platform/file_metadata.hpp"
#include "filesystem/platform/path_safety.hpp"
#include "localvault/error.hpp"
#include "localvault/repository.hpp"
#include "localvault/restore_engine.hpp"
#include "localvault/snapshot_engine.hpp"
#include "localvault/types.hpp"
#include "support/test_filesystem.hpp"

namespace localvault {
namespace {

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

class RestoreEngineTest : public ::testing::Test {
  protected:
    RestoreEngineTest() {
        Repository::create(repository_root());
        repository_.emplace(Repository::open(repository_root(), OpenMode::read_write));
    }

    [[nodiscard]] std::filesystem::path repository_root() const {
        return temporary_.path() / "repository";
    }

    [[nodiscard]] SnapshotId snapshot(const std::filesystem::path& source) {
        return SnapshotEngine(*repository_)
            .create_snapshot(source, SnapshotOptions{.message = "restore test"})
            .snapshot_id;
    }

    [[nodiscard]] std::filesystem::path destination_path(std::string_view name) const {
        return std::filesystem::weakly_canonical(temporary_.path()) / name;
    }

    test::TemporaryDirectory temporary_;
    std::optional<Repository> repository_;
};

TEST_F(RestoreEngineTest, RestoresBytesEmptyEntriesAndMetadata) {
    const std::filesystem::path source = temporary_.path() / "source";
    test::DatasetBuilder(source)
        .directory("empty-directory")
        .text_file("empty-file", "")
        .text_file("nested/file with spaces.txt", "restored bytes")
        .text_file(".hidden", "hidden");
    const auto source_time = std::chrono::file_clock::now() - std::chrono::hours{24};
    std::filesystem::last_write_time(source / "nested", source_time);
    std::filesystem::last_write_time(source, source_time - std::chrono::seconds{1});
#ifndef _WIN32
    ASSERT_EQ(::chmod((source / "nested/file with spaces.txt").c_str(), 0640), 0);
    ASSERT_EQ(::chmod((source / "nested").c_str(), 0750), 0);
#endif
    const SnapshotId snapshot_id = snapshot(source);
    const std::filesystem::path destination = destination_path("restored");

    const RestoreResult result = RestoreEngine(*repository_)
                                     .restore({
                                         .snapshot_id = snapshot_id,
                                         .destination_root = destination,
                                         .overwrite_policy = OverwritePolicy::never,
                                     });

    EXPECT_EQ(result.restored_files, 3U);
    EXPECT_EQ(result.restored_symlinks, 0U);
    EXPECT_TRUE(result.skipped_entries.empty());
#ifdef _WIN32
    const test::MetadataPolicy policy{};
#else
    const test::MetadataPolicy policy{.compare_modification_time = true,
                                      .posix_mode = test::PosixModePolicy::require_equal};
#endif
    test::expect_tree_equal(source, destination, policy);
    EXPECT_EQ(std::filesystem::file_size(destination / "empty-file"), 0U);
}

TEST_F(RestoreEngineTest, RestoresBrokenSymlinkTextWithoutFollowingTarget) {
    const std::filesystem::path source = temporary_.path() / "source";
    std::filesystem::create_directory(source);
    std::error_code error;
    std::filesystem::create_symlink("missing-target", source / "broken", error);
    if (error) {
        GTEST_SKIP() << "symbolic-link creation is unavailable: " << error.message();
    }
    const SnapshotId snapshot_id = snapshot(source);
    const std::filesystem::path destination = destination_path("restored-link");

    const RestoreResult result = RestoreEngine(*repository_)
                                     .restore({
                                         .snapshot_id = snapshot_id,
                                         .destination_root = destination,
                                     });

    EXPECT_EQ(result.restored_symlinks, 1U);
    EXPECT_TRUE(
        std::filesystem::is_symlink(std::filesystem::symlink_status(destination / "broken")));
    EXPECT_EQ(std::filesystem::read_symlink(destination / "broken"), "missing-target");
}

TEST_F(RestoreEngineTest, NeverPreservesExistingConflictsAndUnrelatedFiles) {
    const std::filesystem::path source = temporary_.path() / "source";
    test::DatasetBuilder(source).text_file("conflict.txt", "snapshot").text_file("new.txt", "new");
    const SnapshotId snapshot_id = snapshot(source);
    const std::filesystem::path destination = destination_path("destination");
    test::DatasetBuilder(destination)
        .text_file("conflict.txt", "existing")
        .text_file("unrelated.txt", "untouched");

    const RestoreResult result = RestoreEngine(*repository_)
                                     .restore({
                                         .snapshot_id = snapshot_id,
                                         .destination_root = destination,
                                         .overwrite_policy = OverwritePolicy::never,
                                     });

    EXPECT_EQ(read_text(destination / "conflict.txt"), "existing");
    EXPECT_EQ(read_text(destination / "new.txt"), "new");
    EXPECT_EQ(read_text(destination / "unrelated.txt"), "untouched");
    ASSERT_EQ(result.skipped_entries.size(), 1U);
    EXPECT_EQ(result.skipped_entries.front().relative_path, "conflict.txt");
}

TEST_F(RestoreEngineTest, NativeNoReplacePublicationPreservesDestinationAndTemporaryContent) {
    const std::filesystem::path destination = destination_path("native-no-replace.txt");
    test::DatasetBuilder(destination.parent_path())
        .text_file(destination.filename().string(), "existing");
    TemporaryOutputFile temporary(destination.parent_path());
    constexpr std::string_view replacement = "replacement";
    temporary.write(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(replacement.data()), replacement.size()));
    temporary.sync();
    const std::filesystem::path temporary_path = temporary.path();

    EXPECT_EQ(temporary.publish(destination, false), RestorePublishResult::destination_exists);
    EXPECT_EQ(read_text(destination), "existing");
    EXPECT_EQ(read_text(temporary_path), "replacement");
}

TEST_F(RestoreEngineTest, RestoresOneFileOrOneDirectoryRecursively) {
    const std::filesystem::path source = temporary_.path() / "source";
    test::DatasetBuilder(source)
        .text_file("folder/one.txt", "one")
        .text_file("folder/nested/two.txt", "two")
        .text_file("other.txt", "other");
    const SnapshotId snapshot_id = snapshot(source);

    const std::filesystem::path file_destination = destination_path("file-selection");
    (void)RestoreEngine(*repository_)
        .restore({
            .snapshot_id = snapshot_id,
            .relative_paths = {"folder/one.txt"},
            .destination_root = file_destination,
        });
    EXPECT_TRUE(std::filesystem::is_regular_file(file_destination / "folder/one.txt"));
    EXPECT_FALSE(std::filesystem::exists(file_destination / "folder/nested"));
    EXPECT_FALSE(std::filesystem::exists(file_destination / "other.txt"));

    const std::filesystem::path directory_destination = destination_path("dir-selection");
    (void)RestoreEngine(*repository_)
        .restore({
            .snapshot_id = snapshot_id,
            .relative_paths = {"folder"},
            .destination_root = directory_destination,
        });
    EXPECT_EQ(read_text(directory_destination / "folder/one.txt"), "one");
    EXPECT_EQ(read_text(directory_destination / "folder/nested/two.txt"), "two");
    EXPECT_FALSE(std::filesystem::exists(directory_destination / "other.txt"));
}

TEST_F(RestoreEngineTest, RejectsIncompleteSnapshotBeforeWritingDestination) {
    Database database(repository_root() / "repository.db");
    const SnapshotId pending =
        MetadataStore(database).create_pending_snapshot("/source", "pending", 1);
    const std::filesystem::path destination = destination_path("not-created");

    EXPECT_THROW((void)RestoreEngine(*repository_)
                     .restore({
                         .snapshot_id = pending,
                         .destination_root = destination,
                     }),
                 LocalVaultError);
    EXPECT_FALSE(std::filesystem::exists(destination));
}

TEST_F(RestoreEngineTest, RejectsReadOnlyRepositoryBeforeWritingDestination) {
    const std::filesystem::path destination = destination_path("not-created-read-only");
    repository_.reset();
    Repository read_only = Repository::open(repository_root(), OpenMode::read_only);

    EXPECT_THROW((void)RestoreEngine(read_only).restore({
                     .snapshot_id = 1,
                     .destination_root = destination,
                 }),
                 LocalVaultError);
    EXPECT_FALSE(std::filesystem::exists(destination));
}

TEST_F(RestoreEngineTest, RejectsTraversalMetadataWithoutWritingOutsideDestination) {
    const std::filesystem::path source = temporary_.path() / "source";
    test::DatasetBuilder(source).text_file("safe.txt", "safe");
    const SnapshotId snapshot_id = snapshot(source);
    {
        Database database(repository_root() / "repository.db");
        auto update = database.statement(
            "UPDATE entries SET relative_path = '../escape', parent_path = '..', name = 'escape' "
            "WHERE snapshot_id = :snapshot_id AND relative_path = 'safe.txt'");
        update.bind(":snapshot_id", snapshot_id);
        update.execute();
    }
    const std::filesystem::path destination = destination_path("traversal-destination");

    EXPECT_THROW((void)RestoreEngine(*repository_)
                     .restore({
                         .snapshot_id = snapshot_id,
                         .destination_root = destination,
                     }),
                 LocalVaultError);
    EXPECT_FALSE(std::filesystem::exists(temporary_.path() / "escape"));
    EXPECT_FALSE(std::filesystem::exists(destination));
}

TEST_F(RestoreEngineTest, RejectsDestinationBelowSymlinkWithoutMutatingItsTarget) {
    const std::filesystem::path source = temporary_.path() / "source";
    test::DatasetBuilder(source).text_file("file.txt", "snapshot");
    const SnapshotId snapshot_id = snapshot(source);
    const std::filesystem::path target = destination_path("symlink-target");
    std::filesystem::create_directory(target);
    const std::filesystem::path link = destination_path("destination-link");
    std::error_code error;
    std::filesystem::create_directory_symlink(target, link, error);
    if (error) {
        GTEST_SKIP() << "directory symbolic-link creation is unavailable: " << error.message();
    }

    EXPECT_THROW((void)RestoreEngine(*repository_)
                     .restore({
                         .snapshot_id = snapshot_id,
                         .destination_root = link / "below-link",
                     }),
                 LocalVaultError);
    EXPECT_TRUE(std::filesystem::is_empty(target));
}

#ifdef _WIN32
TEST_F(RestoreEngineTest, RejectsDifferentlyCasedRepositoryDestinationAlias) {
    EXPECT_TRUE(platform_path_is_component_prefix(repository_root(),
                                                  temporary_.path() / "REPOSITORY" / "child"));
    EXPECT_THROW((void)RestoreEngine(*repository_)
                     .restore({
                         .snapshot_id = 1,
                         .destination_root = temporary_.path() / "REPOSITORY" / "destination",
                     }),
                 LocalVaultError);
    EXPECT_FALSE(std::filesystem::exists(repository_root() / "destination"));
}

TEST_F(RestoreEngineTest, RejectsDestinationBelowWindowsJunction) {
    const std::filesystem::path source = temporary_.path() / "junction-source";
    test::DatasetBuilder(source).text_file("file.txt", "snapshot");
    const SnapshotId snapshot_id = snapshot(source);
    const std::filesystem::path target = destination_path("junction-target");
    const std::filesystem::path junction = destination_path("destination-junction");
    std::filesystem::create_directory(target);
    const std::wstring command = L"cmd.exe /d /c mklink /j \"" + junction.wstring() + L"\" \"" +
                                 target.wstring() + L"\" >nul";
    ASSERT_EQ(::_wsystem(command.c_str()), 0) << "failed to create a test junction";
    const DWORD attributes = ::GetFileAttributesW(junction.c_str());
    ASSERT_NE(attributes, INVALID_FILE_ATTRIBUTES);
    ASSERT_NE(attributes & FILE_ATTRIBUTE_REPARSE_POINT, 0U);
    EXPECT_EQ(inspect_path_no_follow(junction), NoFollowPathType::indirection);

    EXPECT_THROW((void)RestoreEngine(*repository_)
                     .restore({
                         .snapshot_id = snapshot_id,
                         .destination_root = junction / "below-junction",
                     }),
                 LocalVaultError);
    std::error_code error;
    std::filesystem::remove(junction, error);
    ASSERT_FALSE(error) << error.message();
    EXPECT_TRUE(std::filesystem::is_empty(target));
}
#endif

TEST_F(RestoreEngineTest, RejectsDestinationWhenRepositorySharesSymlinkAncestor) {
    const std::filesystem::path target = destination_path("shared-symlink-target");
    std::filesystem::create_directory(target);
    const std::filesystem::path link = destination_path("shared-symlink");
    std::error_code error;
    std::filesystem::create_directory_symlink(target, link, error);
    if (error) {
        GTEST_SKIP() << "directory symbolic-link creation is unavailable: " << error.message();
    }
    Repository::create(link / "repository");
    Repository shared_repository = Repository::open(link / "repository", OpenMode::read_write);

    EXPECT_THROW((void)RestoreEngine(shared_repository)
                     .restore({
                         .snapshot_id = 1,
                         .destination_root = link / "destination",
                     }),
                 LocalVaultError);
    EXPECT_FALSE(std::filesystem::exists(target / "destination"));
}

TEST_F(RestoreEngineTest, CorruptObjectCannotReplaceExistingDestination) {
    const std::filesystem::path source = temporary_.path() / "source";
    test::DatasetBuilder(source).text_file("file.txt", "snapshot bytes");
    const SnapshotId snapshot_id = snapshot(source);
    std::filesystem::path object_path;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(repository_root() / "objects")) {
        if (entry.is_regular_file()) {
            object_path = entry.path();
        }
    }
    ASSERT_FALSE(object_path.empty());
    test::corrupt_byte(object_path, 0);
    const std::filesystem::path destination = destination_path("corrupt-destination");
    test::DatasetBuilder(destination).text_file("file.txt", "existing bytes");

    EXPECT_THROW((void)RestoreEngine(*repository_)
                     .restore({
                         .snapshot_id = snapshot_id,
                         .destination_root = destination,
                         .overwrite_policy = OverwritePolicy::always,
                     }),
                 LocalVaultError);
    EXPECT_EQ(read_text(destination / "file.txt"), "existing bytes");
}

TEST_F(RestoreEngineTest, RejectsMalformedChunkLayoutsBeforePublication) {
    const std::filesystem::path source = temporary_.path() / "source";
    test::DatasetBuilder(source).text_file("file.txt", "four");
    const SnapshotId snapshot_id = snapshot(source);
    Database database(repository_root() / "repository.db");
    auto id_query = database.statement(
        "SELECT id FROM entries WHERE snapshot_id = :snapshot_id AND relative_path = 'file.txt'");
    id_query.bind(":snapshot_id", snapshot_id);
    ASSERT_TRUE(id_query.step());
    const std::int64_t entry_id = id_query.column_int64(0);
    ASSERT_FALSE(id_query.step());

    const auto update = [&](std::string_view sql, std::int64_t value) {
        auto statement = database.statement(sql);
        statement.bind(":entry_id", entry_id);
        statement.bind(":value", value);
        statement.execute();
    };
    const auto expect_rejected = [&](std::string_view suffix) {
        const std::filesystem::path destination = destination_path(suffix);
        EXPECT_THROW((void)RestoreEngine(*repository_)
                         .restore({
                             .snapshot_id = snapshot_id,
                             .destination_root = destination,
                         }),
                     LocalVaultError);
        EXPECT_FALSE(std::filesystem::exists(destination / "file.txt"));
        std::error_code ignored;
        std::filesystem::remove_all(destination, ignored);
    };

    update("UPDATE entry_chunks SET sequence_number = :value WHERE entry_id = :entry_id", 1);
    expect_rejected("invalid-sequence");
    update("UPDATE entry_chunks SET sequence_number = :value WHERE entry_id = :entry_id", 0);

    update("UPDATE entry_chunks SET raw_offset = :value WHERE entry_id = :entry_id", 1);
    expect_rejected("invalid-offset");
    update("UPDATE entry_chunks SET raw_offset = :value WHERE entry_id = :entry_id", 0);

    const std::int64_t oversized =
        static_cast<std::int64_t>(repository_->info().chunk_size_bytes + 1U);
    update("UPDATE entry_chunks SET raw_length = :value WHERE entry_id = :entry_id", oversized);
    update("UPDATE chunks SET raw_size = :value WHERE hash = (SELECT chunk_hash FROM entry_chunks "
           "WHERE entry_id = :entry_id)",
           oversized);
    expect_rejected("oversized-chunk");
    update("UPDATE entry_chunks SET raw_length = :value WHERE entry_id = :entry_id", 4);
    update("UPDATE chunks SET raw_size = :value WHERE hash = (SELECT chunk_hash FROM entry_chunks "
           "WHERE entry_id = :entry_id)",
           4);

    update("UPDATE entries SET logical_size = :value WHERE id = :entry_id", 5);
    expect_rejected("logical-size-mismatch");
}

#ifndef _WIN32
TEST_F(RestoreEngineTest, MetadataErrorsWarnWithoutSuppressingRestoredContent) {
    const std::filesystem::path source = temporary_.path() / "source";
    test::DatasetBuilder(source)
        .text_file("bad-file.txt", "bad file bytes")
        .text_file("bad-directory/inside.txt", "inside bytes")
        .text_file("good.txt", "good bytes");
    const SnapshotId snapshot_id = snapshot(source);
    {
        Database database(repository_root() / "repository.db");
        auto update = database.statement(
            "UPDATE entries SET posix_mode = 4294967295 WHERE snapshot_id = :snapshot_id "
            "AND relative_path IN ('bad-file.txt', 'bad-directory')");
        update.bind(":snapshot_id", snapshot_id);
        update.execute();
    }
    const std::filesystem::path destination = destination_path("metadata-errors");

    const RestoreResult result = RestoreEngine(*repository_)
                                     .restore({
                                         .snapshot_id = snapshot_id,
                                         .destination_root = destination,
                                     });

    EXPECT_EQ(read_text(destination / "bad-file.txt"), "bad file bytes");
    EXPECT_EQ(read_text(destination / "bad-directory/inside.txt"), "inside bytes");
    EXPECT_EQ(read_text(destination / "good.txt"), "good bytes");
    EXPECT_EQ(result.restored_files, 3U);
    ASSERT_EQ(result.skipped_entries.size(), 2U);
    bool saw_file_warning = false;
    bool saw_directory_warning = false;
    for (const SkippedEntry& warning : result.skipped_entries) {
        EXPECT_NE(warning.reason.find("metadata not fully restored"), std::string::npos);
        saw_file_warning = saw_file_warning || warning.relative_path == "bad-file.txt";
        saw_directory_warning = saw_directory_warning || warning.relative_path == "bad-directory";
    }
    EXPECT_TRUE(saw_file_warning);
    EXPECT_TRUE(saw_directory_warning);
}
#endif

} // namespace
} // namespace localvault
