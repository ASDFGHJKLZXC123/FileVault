#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "database/database.hpp"
#include "database/metadata_store.hpp"
#include "database/migrations.hpp"
#include "database/statement.hpp"
#include "filesystem/file_scanner.hpp"
#include "localvault/error.hpp"
#include "localvault/failure_injector.hpp"
#include "localvault/types.hpp"
#include "storage/object_store.hpp"
#include "support/test_filesystem.hpp"

namespace localvault {
namespace {

class NoopFailureInjector final : public FailureInjector {
  public:
    void hit(FailurePoint) override {}
};

class ThrowOnOccurrenceInjector final : public FailureInjector {
  public:
    explicit ThrowOnOccurrenceInjector(std::size_t occurrence) : occurrence_(occurrence) {}

    void hit(FailurePoint point) override {
        if (point == FailurePoint::before_metadata_batch_commit && ++hits_ == occurrence_) {
            throw std::runtime_error("injected metadata batch failure");
        }
    }

  private:
    std::size_t occurrence_{};
    std::size_t hits_{};
};

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

TEST_F(StoreFixture, InsertsPendingRegularFileWithHashNull) {
    const ScannedEntry file = entry("folder/file.txt", EntryType::regular_file, 42);
    const std::int64_t id = store_.insert_entry(snapshot_id_, file);

    EXPECT_TRUE(store_.list_entries(snapshot_id_).empty());

    auto query = database_.statement(
        "SELECT id, snapshot_id, relative_path, entry_type, logical_size, modified_time_ns, "
        "posix_mode, typeof(file_hash), typeof(windows_attributes), typeof(symlink_target) "
        "FROM entries WHERE id = :id");
    query.bind(":id", id);
    ASSERT_TRUE(query.step());
    EXPECT_EQ(query.column_int64(0), id);
    EXPECT_EQ(query.column_int64(1), snapshot_id_);
    EXPECT_EQ(query.column_text(2), "folder/file.txt");
    EXPECT_EQ(query.column_text(3), "file");
    EXPECT_EQ(query.column_int64(4), 42);
    EXPECT_EQ(query.column_int64(5), file.modified_time_ns);
    EXPECT_EQ(query.column_int64(6), 0644);
    EXPECT_EQ(query.column_text(7), "null");
    EXPECT_EQ(query.column_text(8), "null");
    EXPECT_EQ(query.column_text(9), "null");
}

TEST_F(StoreFixture, SetsOnlyAnUnhashedRegularEntryToStrictLowercaseBlake3Hex) {
    const std::int64_t file_id =
        store_.insert_entry(snapshot_id_, entry("file.txt", EntryType::regular_file, 3));
    const std::int64_t directory_id =
        store_.insert_entry(snapshot_id_, entry("folder", EntryType::directory));
    const std::string expected_hash(64, 'a');

    EXPECT_THROW(store_.set_regular_file_hash(file_id, std::string(63, 'a')), LocalVaultError);
    EXPECT_THROW(store_.set_regular_file_hash(file_id, std::string(64, 'A')), LocalVaultError);
    EXPECT_THROW(store_.set_regular_file_hash(file_id, std::string(64, 'g')), LocalVaultError);
    EXPECT_THROW(store_.set_regular_file_hash(directory_id, expected_hash), LocalVaultError);
    EXPECT_THROW(store_.set_regular_file_hash(999'999, expected_hash), LocalVaultError);

    store_.set_regular_file_hash(file_id, expected_hash);
    store_.mark_snapshot_complete(snapshot_id_, {}, 2);
    const std::vector<EntryInfo> entries = store_.list_entries(snapshot_id_);
    const auto file = std::ranges::find(entries, file_id, &EntryInfo::id);
    ASSERT_NE(file, entries.end());
    ASSERT_TRUE(file->file_hash_hex.has_value());
    EXPECT_EQ(*file->file_hash_hex, expected_hash);
    EXPECT_THROW(store_.set_regular_file_hash(file_id, expected_hash), LocalVaultError);
}

TEST_F(StoreFixture, CannotCompleteSnapshotWhileARegularFileHashIsNull) {
    const std::int64_t file_id =
        store_.insert_entry(snapshot_id_, entry("file.txt", EntryType::regular_file, 0));
    SnapshotTotals totals;
    totals.file_count = 1;

    EXPECT_THROW(store_.mark_snapshot_complete(snapshot_id_, totals, 2), LocalVaultError);
    store_.set_regular_file_hash(
        file_id, "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");
    store_.mark_snapshot_complete(snapshot_id_, totals, 3);
    EXPECT_EQ(store_.require_complete_snapshot(snapshot_id_).id, snapshot_id_);
}

TEST_F(StoreFixture, ListsRootChildrenWithoutReturningTheRootRow) {
    EXPECT_GT(store_.insert_entry(snapshot_id_, entry("", EntryType::directory)), 0);
    const std::int64_t z_file =
        store_.insert_entry(snapshot_id_, entry("z.txt", EntryType::regular_file, 1));
    EXPECT_GT(store_.insert_entry(snapshot_id_, entry("folder", EntryType::directory)), 0);
    const std::int64_t child_file =
        store_.insert_entry(snapshot_id_, entry("folder/child.txt", EntryType::regular_file, 2));
    store_.set_regular_file_hash(z_file, std::string(64, 'a'));
    store_.set_regular_file_hash(child_file, std::string(64, 'b'));
    store_.mark_snapshot_complete(snapshot_id_, {}, 2);

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
    const std::int64_t nfc_id =
        store_.insert_entry(snapshot_id_, entry(nfc, EntryType::regular_file));
    const std::int64_t nfd_id =
        store_.insert_entry(snapshot_id_, entry(nfd, EntryType::regular_file));
    EXPECT_GT(store_.insert_entry(
                  snapshot_id_, entry("broken", EntryType::symbolic_link, 0, "../missing target")),
              0);
    store_.set_regular_file_hash(nfc_id, std::string(64, 'a'));
    store_.set_regular_file_hash(nfd_id, std::string(64, 'b'));
    store_.mark_snapshot_complete(snapshot_id_, {}, 2);

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

TEST_F(StoreFixture, PublishesOnlyCompleteSnapshotsWithDatabaseDerivedFinalCounters) {
    const SnapshotId complete = store_.create_pending_snapshot("/complete", "message", 10);
    const SnapshotId failed = store_.create_pending_snapshot("/failed", "", 11);
    store_.mark_snapshot_incomplete(failed, SnapshotStatus::failed, "expected failure", 12);

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
    EXPECT_EQ(snapshots.front().file_count, 0U);
    EXPECT_EQ(snapshots.front().directory_count, 0U);
    EXPECT_EQ(snapshots.front().logical_size, 0U);
    EXPECT_EQ(snapshots.front().new_stored_size, 21U);
    EXPECT_EQ(snapshots.front().duration, std::chrono::milliseconds(7));
    EXPECT_EQ(store_.require_complete_snapshot(complete).id, complete);
    EXPECT_THROW((void)store_.require_complete_snapshot(failed), LocalVaultError);
    EXPECT_THROW(store_.mark_snapshot_complete(complete, totals, 14), LocalVaultError);
}

TEST_F(StoreFixture, IncompleteSnapshotCleanupIsTransactionalAndLeavesObjectsForGc) {
    const std::int64_t entry_id =
        store_.insert_entry(snapshot_id_, entry("file.bin", EntryType::regular_file, 1));
    ObjectStore objects(temporary_.path(), store_, 64, 3, std::make_shared<NoopFailureInjector>());
    constexpr std::array raw{std::byte{0x42}};
    const StoredObject object = objects.store(raw);
    store_.insert_entry_chunk(entry_id, 0, object.hash_hex, 0, 1);
    store_.insert_warning(snapshot_id_, "unsupported", "unsupported_type", "skipped");

    store_.mark_snapshot_incomplete(snapshot_id_, SnapshotStatus::cancelled, "cancelled", 12);
    NoopFailureInjector noop;
    store_.clean_incomplete_snapshot(snapshot_id_, noop, 1);

    auto row_counts = database_.statement(
        "SELECT (SELECT COUNT(*) FROM entries WHERE snapshot_id = :snapshot_id), "
        "(SELECT COUNT(*) FROM entry_chunks), "
        "(SELECT COUNT(*) FROM snapshot_warnings WHERE snapshot_id = :snapshot_id), "
        "(SELECT status FROM snapshots WHERE id = :snapshot_id)");
    row_counts.bind(":snapshot_id", snapshot_id_);
    ASSERT_TRUE(row_counts.step());
    EXPECT_EQ(row_counts.column_int64(0), 0);
    EXPECT_EQ(row_counts.column_int64(1), 0);
    EXPECT_EQ(row_counts.column_int64(2), 0);
    EXPECT_EQ(row_counts.column_text(3), "cancelled");
    EXPECT_TRUE(std::filesystem::is_regular_file(temporary_.path() / object.relative_path));
    EXPECT_TRUE(store_.find_chunk(object.hash_hex).has_value());
}

TEST_F(StoreFixture, StoresAndListsOrderedChunkRelationships) {
    const std::int64_t entry_id =
        store_.insert_entry(snapshot_id_, entry("file.bin", EntryType::regular_file, 5));
    ObjectStore objects(temporary_.path(), store_, 64, 3, std::make_shared<NoopFailureInjector>());
    constexpr std::array<std::byte, 3> first_raw{std::byte{1}, std::byte{2}, std::byte{3}};
    constexpr std::array<std::byte, 2> second_raw{std::byte{4}, std::byte{5}};
    const StoredObject first = objects.store(first_raw);
    const StoredObject second = objects.store(second_raw);
    EXPECT_FALSE(objects.store(first_raw).newly_stored);
    ASSERT_TRUE(store_.find_chunk(first.hash_hex).has_value());
    EXPECT_FALSE(store_.find_chunk(std::string(64, 'f')).has_value());
    store_.insert_entry_chunk(entry_id, 0, first.hash_hex, 0, 3);
    store_.insert_entry_chunk(entry_id, 1, second.hash_hex, 3, 2);
    store_.set_regular_file_hash(entry_id, std::string(64, 'a'));
    store_.mark_snapshot_complete(snapshot_id_, {}, 2);

    const std::vector<ChunkReferenceInfo> chunks = store_.list_entry_chunks(entry_id);
    ASSERT_EQ(chunks.size(), 2U);
    EXPECT_EQ(chunks[0].sequence_number, 0);
    EXPECT_EQ(chunks[0].hash_hex, first.hash_hex);
    EXPECT_EQ(chunks[0].raw_offset, 0U);
    EXPECT_EQ(chunks[0].raw_length, 3U);
    EXPECT_EQ(chunks[0].raw_size, 3U);
    EXPECT_EQ(chunks[0].stored_size, first.stored_size);
    EXPECT_EQ(chunks[0].object_path, first.relative_path);
    EXPECT_EQ(chunks[0].object_path.extension(), ".zst");
    EXPECT_EQ(chunks[1].sequence_number, 1);
    EXPECT_EQ(chunks[1].raw_offset, 3U);
    EXPECT_EQ(chunks[1].raw_length, 2U);
    EXPECT_EQ(chunks[1].stored_size, second.stored_size);
    EXPECT_EQ(chunks[1].object_path, second.relative_path);

    auto conflict = database_.statement("UPDATE chunks SET raw_size = 2 WHERE hash = :hash");
    conflict.bind(":hash", first.hash_hex);
    conflict.execute();
    ObjectStore cold_objects(temporary_.path(), store_, 64, 3,
                             std::make_shared<NoopFailureInjector>());
    try {
        (void)cold_objects.store(first_raw);
        FAIL() << "conflicting raw size unexpectedly reused";
    } catch (const LocalVaultError& error) {
        EXPECT_EQ(error.code(), ErrorCode::object_corrupt);
    }
}

TEST_F(StoreFixture, BrowseQueriesExposeOnlyCompleteSnapshotMetadata) {
    ObjectStore objects(temporary_.path(), store_, 64, 3, std::make_shared<NoopFailureInjector>());
    constexpr std::array raw{std::byte{0x42}};
    const StoredObject object = objects.store(raw);
    const auto add_file = [&](SnapshotId snapshot_id, std::string path) {
        const std::int64_t entry_id =
            store_.insert_entry(snapshot_id, entry(std::move(path), EntryType::regular_file, 1));
        store_.insert_entry_chunk(entry_id, 0, object.hash_hex, 0, 1);
        store_.set_regular_file_hash(entry_id, std::string(64, 'a'));
        return entry_id;
    };

    const std::int64_t pending_entry = add_file(snapshot_id_, "pending");
    const SnapshotId complete = store_.create_pending_snapshot("/complete", "", 2);
    const std::int64_t complete_entry = add_file(complete, "complete");
    store_.mark_snapshot_complete(complete, {}, 3);

    const SnapshotId failed = store_.create_pending_snapshot("/failed", "", 4);
    const std::int64_t failed_entry = add_file(failed, "failed");
    const SnapshotId cancelled = store_.create_pending_snapshot("/cancelled", "", 5);
    const std::int64_t cancelled_entry = add_file(cancelled, "cancelled");
    auto make_incomplete =
        database_.statement("UPDATE snapshots SET status = :status WHERE id = :snapshot_id");
    make_incomplete.bind(":status", "failed");
    make_incomplete.bind(":snapshot_id", failed);
    make_incomplete.execute();
    make_incomplete.reset();
    make_incomplete.bind(":status", "cancelled");
    make_incomplete.bind(":snapshot_id", cancelled);
    make_incomplete.execute();

    const SnapshotId deleting = store_.create_pending_snapshot("/deleting", "", 6);
    const std::int64_t deleting_entry = add_file(deleting, "deleting");
    store_.mark_snapshot_complete(deleting, {}, 7);
    NoopFailureInjector noop;
    store_.transition_snapshot_to_deleting(deleting, noop);

    ASSERT_EQ(store_.list_entries(complete).size(), 1U);
    ASSERT_EQ(store_.list_children(complete, "").size(), 1U);
    ASSERT_EQ(store_.list_entry_chunks(complete_entry).size(), 1U);
    for (const auto [snapshot, entry_id] :
         std::array{std::pair{snapshot_id_, pending_entry}, std::pair{failed, failed_entry},
                    std::pair{cancelled, cancelled_entry}, std::pair{deleting, deleting_entry}}) {
        EXPECT_TRUE(store_.list_entries(snapshot).empty());
        EXPECT_TRUE(store_.list_children(snapshot, "").empty());
        EXPECT_TRUE(store_.list_entry_chunks(entry_id).empty());
    }
}

TEST_F(StoreFixture, DeletingSnapshotResumesBoundedEntryDeletionAndCascadesFinalMetadata) {
    ObjectStore objects(temporary_.path(), store_, 64, 3, std::make_shared<NoopFailureInjector>());
    constexpr std::array raw{std::byte{0x42}};
    const StoredObject object = objects.store(raw);
    const std::int64_t file_id =
        store_.insert_entry(snapshot_id_, entry("file.bin", EntryType::regular_file, 1));
    store_.insert_entry_chunk(file_id, 0, object.hash_hex, 0, 1);
    store_.set_regular_file_hash(file_id, std::string(64, 'a'));
    for (int index = 0; index < 2; ++index) {
        EXPECT_GT(store_.insert_entry(snapshot_id_, entry("directory-" + std::to_string(index),
                                                          EntryType::directory)),
                  0);
    }
    store_.insert_warning(snapshot_id_, "warning", "test", "warning");
    store_.mark_snapshot_complete(snapshot_id_, {}, 2);
    NoopFailureInjector noop;
    store_.transition_snapshot_to_deleting(snapshot_id_, noop);
    EXPECT_TRUE(store_.list_entries(snapshot_id_).empty());

    ThrowOnOccurrenceInjector interrupt_second_batch(2);
    EXPECT_THROW(store_.delete_deleting_snapshot(snapshot_id_, interrupt_second_batch, 1),
                 std::runtime_error);
    auto residue = database_.statement(
        "SELECT status, (SELECT COUNT(*) FROM entries WHERE snapshot_id = :snapshot_id) "
        "FROM snapshots WHERE id = :snapshot_id");
    residue.bind(":snapshot_id", snapshot_id_);
    ASSERT_TRUE(residue.step());
    EXPECT_EQ(residue.column_text(0), "deleting");
    EXPECT_EQ(residue.column_int64(1), 2);

    store_.delete_deleting_snapshot(snapshot_id_, noop, 1);
    auto counts = database_.statement(
        "SELECT (SELECT COUNT(*) FROM snapshots WHERE id = :snapshot_id), "
        "(SELECT COUNT(*) FROM entries WHERE snapshot_id = :snapshot_id), "
        "(SELECT COUNT(*) FROM snapshot_warnings WHERE snapshot_id = :snapshot_id)");
    counts.bind(":snapshot_id", snapshot_id_);
    ASSERT_TRUE(counts.step());
    EXPECT_EQ(counts.column_int64(0), 0);
    EXPECT_EQ(counts.column_int64(1), 0);
    EXPECT_EQ(counts.column_int64(2), 0);
    EXPECT_TRUE(std::filesystem::is_regular_file(temporary_.path() / object.relative_path));
    EXPECT_TRUE(store_.find_chunk(object.hash_hex).has_value());
}

TEST_F(StoreFixture, DeletingSnapshotDrainsWarningsInBoundedOrderedResumableBatches) {
    for (int index = 0; index < 3; ++index) {
        store_.insert_warning(snapshot_id_, "warning-" + std::to_string(index), "test", "warning");
    }
    store_.mark_snapshot_complete(snapshot_id_, {}, 2);
    NoopFailureInjector noop;
    store_.transition_snapshot_to_deleting(snapshot_id_, noop);

    ThrowOnOccurrenceInjector interrupt_second_warning_batch(3);
    EXPECT_THROW(store_.delete_deleting_snapshot(snapshot_id_, interrupt_second_warning_batch, 1),
                 std::runtime_error);
    auto residue = database_.statement(
        "SELECT status, (SELECT COUNT(*) FROM snapshot_warnings "
        "WHERE snapshot_id = :snapshot_id), "
        "(SELECT COUNT(*) FROM snapshot_warnings WHERE snapshot_id = :snapshot_id "
        "AND relative_path = 'warning-0') FROM snapshots WHERE id = :snapshot_id");
    residue.bind(":snapshot_id", snapshot_id_);
    ASSERT_TRUE(residue.step());
    EXPECT_EQ(residue.column_text(0), "deleting");
    EXPECT_EQ(residue.column_int64(1), 2);
    EXPECT_EQ(residue.column_int64(2), 0);

    EXPECT_NO_THROW(store_.delete_deleting_snapshot(snapshot_id_, noop, 1));
    auto remaining = database_.statement("SELECT COUNT(*) FROM snapshots WHERE id = :snapshot_id");
    remaining.bind(":snapshot_id", snapshot_id_);
    ASSERT_TRUE(remaining.step());
    EXPECT_EQ(remaining.column_int64(0), 0);
}

TEST_F(StoreFixture, InterruptedIncompleteCleanupIsBoundedAndResumable) {
    for (int index = 0; index < 3; ++index) {
        EXPECT_GT(store_.insert_entry(snapshot_id_, entry("directory-" + std::to_string(index),
                                                          EntryType::directory)),
                  0);
    }
    store_.insert_warning(snapshot_id_, "warning", "test", "warning");
    NoopFailureInjector noop;
    store_.mark_stale_pending_snapshot_failed(snapshot_id_, "recovered", 2, noop);

    ThrowOnOccurrenceInjector interrupt_second_batch(2);
    EXPECT_THROW(store_.clean_incomplete_snapshot(snapshot_id_, interrupt_second_batch, 1),
                 std::runtime_error);
    auto residue =
        database_.statement("SELECT status, failure_message, "
                            "(SELECT COUNT(*) FROM entries WHERE snapshot_id = :snapshot_id) "
                            "FROM snapshots WHERE id = :snapshot_id");
    residue.bind(":snapshot_id", snapshot_id_);
    ASSERT_TRUE(residue.step());
    EXPECT_EQ(residue.column_text(0), "failed");
    EXPECT_EQ(residue.column_text(1), "recovered");
    EXPECT_EQ(residue.column_int64(2), 2);

    store_.clean_incomplete_snapshot(snapshot_id_, noop, 1);
    EXPECT_NO_THROW(store_.quick_relationship_check());
    auto counts = database_.statement(
        "SELECT (SELECT COUNT(*) FROM entries WHERE snapshot_id = :snapshot_id), "
        "(SELECT COUNT(*) FROM snapshot_warnings WHERE snapshot_id = :snapshot_id), "
        "(SELECT status FROM snapshots WHERE id = :snapshot_id)");
    counts.bind(":snapshot_id", snapshot_id_);
    ASSERT_TRUE(counts.step());
    EXPECT_EQ(counts.column_int64(0), 0);
    EXPECT_EQ(counts.column_int64(1), 0);
    EXPECT_EQ(counts.column_text(2), "failed");
}

TEST_F(StoreFixture, QuickRelationshipCheckRejectsForeignKeyCorruption) {
    store_.mark_snapshot_incomplete(snapshot_id_, SnapshotStatus::failed, "test", 2);
    EXPECT_NO_THROW(store_.quick_relationship_check());
    database_.execute("PRAGMA foreign_keys = OFF");
    database_.execute(
        "INSERT INTO entries (snapshot_id, relative_path, parent_path, name, entry_type) "
        "VALUES (999999, 'orphan', '', 'orphan', 'directory')");
    database_.execute("PRAGMA foreign_keys = ON");
    EXPECT_THROW(store_.quick_relationship_check(), LocalVaultError);
}

} // namespace
} // namespace localvault
