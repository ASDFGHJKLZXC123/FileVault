#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "database/database.hpp"
#include "database/metadata_store.hpp"
#include "database/migrations.hpp"
#include "database/statement.hpp"
#include "filesystem/file_scanner.hpp"
#include "localvault/error.hpp"
#include "localvault/types.hpp"
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

class StoreFixture : public ::testing::Test {
  protected:
    StoreFixture() : database_(temporary_.path() / "metadata.db"), store_(database_) {
        run_migrations(database_);
        auto insert =
            database_.statement("INSERT INTO snapshots (created_at_ns, source_root, status) "
                                "VALUES (1, '/source', 'pending') RETURNING id");
        EXPECT_TRUE(insert.step());
        snapshot_id_ = insert.column_int64(0);
        EXPECT_FALSE(insert.step());
    }

    [[nodiscard]] ScannedEntry entry(std::string path, EntryType type, ByteCount logical_size = 0,
                                     std::optional<std::string> symlink_target = std::nullopt) {
        const std::size_t separator = path.rfind('/');
        return {
            temporary_.path() / path,
            path,
            separator == std::string::npos ? std::string{} : path.substr(0, separator),
            path.substr(separator == std::string::npos ? 0 : separator + 1),
            type,
            logical_size,
            1'700'000'000'123'456'789LL,
            0644,
            std::move(symlink_target),
        };
    }

    test::TemporaryDirectory temporary_;
    Database database_;
    MetadataStore store_;
    SnapshotId snapshot_id_{};
};

TEST_F(StoreFixture, InsertsAllMetadataWithFileHashNull) {
    const ScannedEntry file = entry("folder/file.txt", EntryType::regular_file, 42);
    const std::int64_t id = store_.insert_entry(snapshot_id_, file);

    const std::vector<EntryInfo> entries = store_.list_entries(snapshot_id_);
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries.front().id, id);
    EXPECT_EQ(entries.front().snapshot_id, snapshot_id_);
    EXPECT_EQ(path_utf8(entries.front().relative_path), "folder/file.txt");
    EXPECT_EQ(entries.front().type, EntryType::regular_file);
    EXPECT_EQ(entries.front().logical_size, 42U);
    EXPECT_EQ(entries.front().modified_time_ns, file.modified_time_ns);
    EXPECT_EQ(entries.front().posix_mode, 0644U);
    EXPECT_FALSE(entries.front().windows_attributes.has_value());
    EXPECT_FALSE(entries.front().file_hash_hex.has_value());
    EXPECT_FALSE(entries.front().symlink_target.has_value());

    auto query = database_.statement(
        "SELECT typeof(file_hash), typeof(windows_attributes) FROM entries WHERE id = :id");
    query.bind(":id", id);
    ASSERT_TRUE(query.step());
    EXPECT_EQ(query.column_text(0), "null");
    EXPECT_EQ(query.column_text(1), "null");
}

TEST_F(StoreFixture, ListsRootChildrenWithoutReturningTheRootRow) {
    EXPECT_GT(store_.insert_entry(snapshot_id_, entry("", EntryType::directory)), 0);
    EXPECT_GT(store_.insert_entry(snapshot_id_, entry("z.txt", EntryType::regular_file, 1)), 0);
    EXPECT_GT(store_.insert_entry(snapshot_id_, entry("folder", EntryType::directory)), 0);
    EXPECT_GT(
        store_.insert_entry(snapshot_id_, entry("folder/child.txt", EntryType::regular_file, 2)),
        0);

    const std::vector<EntryInfo> root_children = store_.list_children(snapshot_id_, "");
    ASSERT_EQ(root_children.size(), 2U);
    EXPECT_EQ(path_utf8(root_children[0].relative_path), "folder");
    EXPECT_EQ(path_utf8(root_children[1].relative_path), "z.txt");

    const std::vector<EntryInfo> nested_children = store_.list_children(snapshot_id_, "folder");
    ASSERT_EQ(nested_children.size(), 1U);
    EXPECT_EQ(path_utf8(nested_children.front().relative_path), "folder/child.txt");
}

TEST_F(StoreFixture, PreservesSymlinkTargetAndDistinctUnicodeIdentities) {
    const std::string nfc = "caf\xC3\xA9";
    const std::string nfd = "cafe\xCC\x81";
    EXPECT_GT(store_.insert_entry(snapshot_id_, entry(nfc, EntryType::regular_file)), 0);
    EXPECT_GT(store_.insert_entry(snapshot_id_, entry(nfd, EntryType::regular_file)), 0);
    EXPECT_GT(store_.insert_entry(
                  snapshot_id_, entry("broken", EntryType::symbolic_link, 0, "../missing target")),
              0);

    const std::vector<EntryInfo> entries = store_.list_entries(snapshot_id_);
    ASSERT_EQ(entries.size(), 3U);
    std::vector<std::string> paths;
    for (const EntryInfo& stored : entries) {
        paths.push_back(path_utf8(stored.relative_path));
        if (path_utf8(stored.relative_path) == "broken") {
            ASSERT_TRUE(stored.symlink_target.has_value());
            EXPECT_EQ(path_utf8(*stored.symlink_target), "../missing target");
        }
    }
    EXPECT_NE(std::ranges::find(paths, nfc), paths.end());
    EXPECT_NE(std::ranges::find(paths, nfd), paths.end());
    EXPECT_NE(nfc, nfd);
}

TEST_F(StoreFixture, RejectsLogicalSizesOutsideSQLiteIntegerRange) {
    ScannedEntry oversized = entry("oversized", EntryType::regular_file);
    oversized.logical_size = (std::numeric_limits<ByteCount>::max)();

    EXPECT_THROW((void)store_.insert_entry(snapshot_id_, oversized), LocalVaultError);
}

TEST_F(StoreFixture, PublishesOnlyCompleteSnapshotsWithFinalCounters) {
    const SnapshotId complete = store_.create_pending_snapshot("/complete", "message", 10);
    const SnapshotId failed = store_.create_pending_snapshot("/failed", "", 11);
    store_.mark_snapshot_failed(failed, "expected failure", 12);

    SnapshotTotals totals;
    totals.file_count = 2;
    totals.directory_count = 3;
    totals.symlink_count = 1;
    totals.logical_size = 42;
    totals.new_stored_size = 21;
    totals.new_chunk_count = 2;
    totals.duration_ms = 7;
    store_.mark_snapshot_complete(complete, totals, 13);

    const std::vector<SnapshotSummary> snapshots = store_.list_complete_snapshots();
    ASSERT_EQ(snapshots.size(), 1U);
    EXPECT_EQ(snapshots.front().id, complete);
    EXPECT_EQ(path_utf8(snapshots.front().source_root), "/complete");
    EXPECT_EQ(snapshots.front().message, "message");
    EXPECT_EQ(snapshots.front().status, SnapshotStatus::complete);
    EXPECT_EQ(snapshots.front().file_count, 2U);
    EXPECT_EQ(snapshots.front().directory_count, 3U);
    EXPECT_EQ(snapshots.front().logical_size, 42U);
    EXPECT_EQ(snapshots.front().new_stored_size, 21U);
    EXPECT_EQ(snapshots.front().duration, std::chrono::milliseconds(7));
    EXPECT_EQ(store_.require_complete_snapshot(complete).id, complete);
    EXPECT_THROW((void)store_.require_complete_snapshot(failed), LocalVaultError);
    EXPECT_THROW(store_.mark_snapshot_complete(complete, totals, 14), LocalVaultError);
}

TEST_F(StoreFixture, StoresAndListsOrderedChunkRelationships) {
    const std::int64_t entry_id =
        store_.insert_entry(snapshot_id_, entry("file.bin", EntryType::regular_file, 5));
    const StoredChunkInfo first{
        .hash_hex = std::string(64, 'a'),
        .raw_size = 3,
        .stored_size = 3,
        .object_path = "objects/aa/first.raw",
        .created_at_ns = 1,
    };
    const StoredChunkInfo second{
        .hash_hex = std::string(64, 'b'),
        .raw_size = 2,
        .stored_size = 2,
        .object_path = "objects/bb/second.raw",
        .created_at_ns = 2,
    };
    store_.ensure_chunk(first);
    store_.ensure_chunk(second);
    store_.ensure_chunk(first);
    store_.insert_entry_chunk(entry_id, 0, first.hash_hex, 0, 3);
    store_.insert_entry_chunk(entry_id, 1, second.hash_hex, 3, 2);

    const std::vector<ChunkReferenceInfo> chunks = store_.list_entry_chunks(entry_id);
    ASSERT_EQ(chunks.size(), 2U);
    EXPECT_EQ(chunks[0].sequence_number, 0);
    EXPECT_EQ(chunks[0].hash_hex, first.hash_hex);
    EXPECT_EQ(chunks[0].raw_offset, 0U);
    EXPECT_EQ(chunks[0].raw_length, 3U);
    EXPECT_EQ(chunks[0].raw_size, 3U);
    EXPECT_EQ(chunks[0].stored_size, 3U);
    EXPECT_EQ(chunks[1].sequence_number, 1);
    EXPECT_EQ(chunks[1].raw_offset, 3U);
    EXPECT_EQ(chunks[1].raw_length, 2U);

    StoredChunkInfo conflict = first;
    conflict.stored_size = 2;
    EXPECT_THROW(store_.ensure_chunk(conflict), LocalVaultError);
}

} // namespace
} // namespace localvault
