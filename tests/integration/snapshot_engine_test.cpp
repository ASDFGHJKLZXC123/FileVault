#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "database/database.hpp"
#include "database/metadata_store.hpp"
#include "database/statement.hpp"
#include "localvault/error.hpp"
#include "localvault/repository.hpp"
#include "localvault/snapshot_engine.hpp"
#include "storage/object_store.hpp"
#include "support/test_filesystem.hpp"

namespace localvault {
namespace {

[[nodiscard]] std::string path_utf8(const std::filesystem::path& path) {
    const std::u8string encoded = path.generic_u8string();
    std::string result;
    result.reserve(encoded.size());
    for (const char8_t character : encoded) {
        result.push_back(static_cast<char>(character));
    }
    return result;
}

[[nodiscard]] const EntryInfo* find_entry(const std::vector<EntryInfo>& entries,
                                          std::string_view relative_path) {
    const auto entry = std::ranges::find(entries, relative_path, [](const EntryInfo& value) {
        return path_utf8(value.relative_path);
    });
    return entry == entries.end() ? nullptr : &*entry;
}

[[nodiscard]] std::uint64_t count_raw_objects(const std::filesystem::path& repository_root) {
    std::uint64_t count = 0;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(repository_root / "objects")) {
        if (entry.is_regular_file() && entry.path().extension() == ".raw") {
            ++count;
        }
    }
    return count;
}

[[nodiscard]] std::string snapshot_status(Database& database, SnapshotId snapshot_id) {
    auto query = database.statement("SELECT status FROM snapshots WHERE id = :id");
    query.bind(":id", snapshot_id);
    if (!query.step()) {
        return {};
    }
    return query.column_text(0);
}

TEST(SnapshotEngineTest, StoresCompleteFixtureWithOrderedVerifiedRawChunks) {
    test::TemporaryDirectory temporary;
    const std::filesystem::path source = temporary.path() / "source";
    const std::filesystem::path repository_root = temporary.path() / "repository";
    const std::string nfc_name = "nfc-caf\xC3\xA9.txt";
    const std::string nfd_name = "nfd-cafe\xCC\x81.txt";
    test::DatasetBuilder(source)
        .directory("empty directory")
        .text_file("empty file", "")
        .text_file("nested/small.txt", "small text")
        .repeated_file("exact chunk.bin", 8, std::byte{0x11})
        .repeated_file("nested/large.bin", 21, std::byte{0x22})
        .text_file("file with spaces.txt", "spaced")
        .text_file(nfc_name, "nfc")
        .text_file(nfd_name, "nfd")
        .text_file(".hidden", "hidden");
    std::string on_disk_nfc_name;
    std::string on_disk_nfd_name;
    for (const auto& entry : std::filesystem::directory_iterator(source)) {
        const std::string name = path_utf8(entry.path().filename());
        if (name.starts_with("nfc-")) {
            on_disk_nfc_name = name;
        } else if (name.starts_with("nfd-")) {
            on_disk_nfd_name = name;
        }
    }
    ASSERT_FALSE(on_disk_nfc_name.empty());
    ASSERT_FALSE(on_disk_nfd_name.empty());
    EXPECT_NE(on_disk_nfc_name, on_disk_nfd_name);

    bool symlink_created = false;
    try {
        test::DatasetBuilder(source).symlink("broken link", "missing target");
        symlink_created = true;
    } catch (const std::filesystem::filesystem_error&) {
#ifndef _WIN32
        throw;
#endif
    }
#ifndef _WIN32
    ASSERT_EQ(::mkfifo((source / "unsupported fifo").c_str(), 0600), 0);
#endif

    RepositoryCreateOptions create_options;
    Repository::create(repository_root, create_options);
    Repository repository = Repository::open(repository_root, OpenMode::read_write);
    std::vector<OperationPhase> phases;
    SnapshotOptions snapshot_options;
    snapshot_options.message = "fixture";
    const SnapshotResult result =
        SnapshotEngine(repository)
            .create_snapshot(source, snapshot_options, {},
                             [&](const ProgressEvent& event) { phases.push_back(event.phase); });

    EXPECT_EQ(result.file_count, 8U);
    EXPECT_EQ(result.directory_count, 3U);
    EXPECT_EQ(result.logical_bytes, 8U + 21U + 10U + 6U + 3U + 3U + 6U);
    EXPECT_FALSE(phases.empty());
    EXPECT_EQ(phases.front(), OperationPhase::preparing);
    EXPECT_EQ(phases.back(), OperationPhase::complete);
#ifndef _WIN32
    ASSERT_EQ(result.skipped_entries.size(), 1U);
    EXPECT_EQ(path_utf8(result.skipped_entries.front().relative_path), "unsupported fifo");
#else
    EXPECT_TRUE(result.skipped_entries.empty());
#endif

    Database database(repository_root / "repository.db");
    MetadataStore metadata(database);
    const std::vector<SnapshotSummary> snapshots = metadata.list_complete_snapshots();
    ASSERT_EQ(snapshots.size(), 1U);
    EXPECT_EQ(snapshots.front().id, result.snapshot_id);
    EXPECT_EQ(snapshots.front().message, "fixture");
    EXPECT_EQ(snapshots.front().source_root, std::filesystem::absolute(source).lexically_normal());

    const std::vector<EntryInfo> entries = metadata.list_entries(result.snapshot_id);
    EXPECT_EQ(entries.size(), 11U + (symlink_created ? 1U : 0U));
    ASSERT_NE(find_entry(entries, ""), nullptr);
    ASSERT_NE(find_entry(entries, "empty directory"), nullptr);
    ASSERT_NE(find_entry(entries, on_disk_nfc_name), nullptr);
    ASSERT_NE(find_entry(entries, on_disk_nfd_name), nullptr);
    const EntryInfo* empty_file = find_entry(entries, "empty file");
    ASSERT_NE(empty_file, nullptr);
    EXPECT_TRUE(metadata.list_entry_chunks(empty_file->id).empty());
    EXPECT_FALSE(empty_file->file_hash_hex.has_value());
    if (symlink_created) {
        const EntryInfo* link = find_entry(entries, "broken link");
        ASSERT_NE(link, nullptr);
        ASSERT_TRUE(link->symlink_target.has_value());
        EXPECT_EQ(path_utf8(*link->symlink_target), "missing target");
    }

    ObjectStore objects(repository_root);
    std::uint64_t total_references = 0;
    for (const EntryInfo& entry : entries) {
        if (entry.type != EntryType::regular_file) {
            continue;
        }
        EXPECT_FALSE(entry.file_hash_hex.has_value());
        const std::vector<ChunkReferenceInfo> chunks = metadata.list_entry_chunks(entry.id);
        std::vector<std::byte> reconstructed;
        ByteCount expected_offset = 0;
        for (std::size_t index = 0; index < chunks.size(); ++index) {
            const ChunkReferenceInfo& chunk = chunks[index];
            EXPECT_EQ(chunk.sequence_number, static_cast<std::int64_t>(index));
            EXPECT_EQ(chunk.raw_offset, expected_offset);
            EXPECT_LE(chunk.raw_length, create_options.chunk_size_bytes);
            EXPECT_EQ(chunk.raw_length, chunk.raw_size);
            EXPECT_EQ(chunk.raw_size, chunk.stored_size);
            EXPECT_EQ(chunk.hash_hex.size(), 64U);
            EXPECT_EQ(chunk.object_path.extension(), ".raw");
            const auto raw = objects.read_verified(chunk.hash_hex, chunk.object_path,
                                                   chunk.raw_size, chunk.stored_size);
            reconstructed.insert(reconstructed.end(), raw.begin(), raw.end());
            expected_offset += chunk.raw_length;
            ++total_references;
        }
        EXPECT_EQ(reconstructed, test::read_all_bytes(source / entry.relative_path));
    }
    EXPECT_EQ(total_references, result.new_chunks + result.reused_chunks);
    EXPECT_EQ(count_raw_objects(repository_root), result.new_chunks);

    auto warning_count = database.statement(
        "SELECT COUNT(*) FROM snapshot_warnings WHERE snapshot_id = :snapshot_id");
    warning_count.bind(":snapshot_id", result.snapshot_id);
    ASSERT_TRUE(warning_count.step());
#ifndef _WIN32
    EXPECT_EQ(warning_count.column_int64(0), 1);
#else
    EXPECT_EQ(warning_count.column_int64(0), 0);
#endif
}

TEST(SnapshotEngineTest, RejectsReadOnlyInvalidAndContainedSources) {
    test::TemporaryDirectory temporary;
    const std::filesystem::path source = temporary.path() / "source";
    const std::filesystem::path repository_root = temporary.path() / "repository";
    test::DatasetBuilder(source).text_file("file.txt", "contents");
    Repository::create(repository_root);

    Repository read_only = Repository::open(repository_root, OpenMode::read_only);
    EXPECT_THROW((void)SnapshotEngine(read_only).create_snapshot(source, {}), LocalVaultError);
    Repository read_write = Repository::open(repository_root, OpenMode::read_write);
    EXPECT_THROW(
        (void)SnapshotEngine(read_write).create_snapshot(temporary.path() / "missing-source", {}),
        LocalVaultError);

    const std::filesystem::path source_inside_repository = repository_root / "source";
    std::filesystem::create_directory(source_inside_repository);
    EXPECT_THROW((void)SnapshotEngine(read_write).create_snapshot(source_inside_repository, {}),
                 LocalVaultError);
#ifdef _WIN32
    EXPECT_THROW((void)SnapshotEngine(read_write)
                     .create_snapshot(temporary.path() / "REPOSITORY" / "source", {}),
                 LocalVaultError);
#endif

    const std::filesystem::path outer_source = temporary.path() / "outer-source";
    const std::filesystem::path nested_repository_root = outer_source / "nested-repository";
    std::filesystem::create_directory(outer_source);
    Repository::create(nested_repository_root);
    Repository nested_repository = Repository::open(nested_repository_root, OpenMode::read_write);
    EXPECT_THROW((void)SnapshotEngine(nested_repository).create_snapshot(outer_source, {}),
                 LocalVaultError);
}

TEST(SnapshotEngineTest, FatalProgressFailureMarksSnapshotFailedAndHidesItFromListings) {
    test::TemporaryDirectory temporary;
    const std::filesystem::path source = temporary.path() / "source";
    const std::filesystem::path repository_root = temporary.path() / "repository";
    test::DatasetBuilder(source).text_file("file.txt", "contents");
    Repository::create(repository_root);
    Repository repository = Repository::open(repository_root, OpenMode::read_write);

    EXPECT_THROW((void)SnapshotEngine(repository)
                     .create_snapshot(source, {}, {},
                                      [](const ProgressEvent& event) {
                                          if (event.phase == OperationPhase::scanning) {
                                              throw std::runtime_error("injected progress failure");
                                          }
                                      }),
                 LocalVaultError);

    Database database(repository_root / "repository.db");
    MetadataStore metadata(database);
    auto latest = database.statement("SELECT id FROM snapshots ORDER BY id DESC LIMIT 1");
    ASSERT_TRUE(latest.step());
    const SnapshotId failed_id = latest.column_int64(0);
    EXPECT_EQ(snapshot_status(database, failed_id), "failed");
    const SnapshotId pending_id = metadata.create_pending_snapshot("/pending", "", std::int64_t{1});
    EXPECT_EQ(snapshot_status(database, pending_id), "pending");
    EXPECT_TRUE(metadata.list_complete_snapshots().empty());
}

TEST(SnapshotEngineTest, CancellationAfterPendingRowMarksFailureAndThrowsCancelled) {
    test::TemporaryDirectory temporary;
    const std::filesystem::path source = temporary.path() / "source";
    const std::filesystem::path repository_root = temporary.path() / "repository";
    test::DatasetBuilder(source).repeated_file("file.bin", 32, std::byte{0x44});
    Repository::create(repository_root);
    Repository repository = Repository::open(repository_root, OpenMode::read_write);
    std::stop_source stop;

    try {
        (void)SnapshotEngine(repository)
            .create_snapshot(source, {}, stop.get_token(), [&](const ProgressEvent& event) {
                if (event.phase == OperationPhase::scanning) {
                    stop.request_stop();
                }
            });
        FAIL() << "cancelled snapshot unexpectedly succeeded";
    } catch (const LocalVaultError& error) {
        EXPECT_EQ(error.code(), ErrorCode::cancelled);
    }

    Database database(repository_root / "repository.db");
    auto latest = database.statement("SELECT id, status FROM snapshots ORDER BY id DESC LIMIT 1");
    ASSERT_TRUE(latest.step());
    EXPECT_EQ(latest.column_text(1), "failed");
}

} // namespace
} // namespace localvault
