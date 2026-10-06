#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <string>
#include <string_view>

#include "database/database.hpp"
#include "database/metadata_store.hpp"
#include "database/statement.hpp"
#include "filesystem/platform/platform_lock.hpp"
#include "localvault/error.hpp"
#include "localvault/query_service.hpp"
#include "localvault/repository.hpp"
#include "support/test_filesystem.hpp"

namespace localvault {
namespace {

class QueryServiceTest : public testing::Test {
  protected:
    void SetUp() override {
        Repository::create(root);
        repository = std::make_unique<Repository>(Repository::open(root));
        database = std::make_unique<Database>(root / "repository.db");
        query = std::make_unique<QueryService>(*repository);
    }

    SnapshotId snapshot(std::string_view status = "complete", std::int64_t created = 0) {
        auto insert = database->statement(
            "INSERT INTO snapshots(created_at_ns, source_root, message, status) "
            "VALUES(:created, '/source', 'test snapshot', :status) RETURNING id");
        insert.bind(":created", created);
        insert.bind(":status", status);
        EXPECT_TRUE(insert.step());
        const auto id = insert.column_int64(0);
        EXPECT_FALSE(insert.step());
        return id;
    }

    std::int64_t entry(SnapshotId snapshot_id, std::string_view path, std::int64_t size = 0,
                       std::string_view type = "file") {
        const auto slash = path.rfind('/');
        const auto parent =
            slash == std::string_view::npos ? std::string_view{""} : path.substr(0, slash);
        const auto name = slash == std::string_view::npos ? path : path.substr(slash + 1);
        auto insert = database->statement(
            "INSERT INTO entries(snapshot_id, relative_path, parent_path, name, entry_type, "
            "logical_size, file_hash) VALUES(:snapshot, :path, :parent, :name, :type, :size, "
            ":hash) "
            "RETURNING id");
        insert.bind(":snapshot", snapshot_id);
        insert.bind(":path", path);
        insert.bind(":parent", parent);
        insert.bind(":name", name);
        insert.bind(":type", type);
        insert.bind(":size", size);
        if (type == "file") {
            insert.bind(":hash", std::string(64, 'f'));
        } else {
            insert.bind_null(":hash");
        }
        EXPECT_TRUE(insert.step());
        const auto id = insert.column_int64(0);
        EXPECT_FALSE(insert.step());
        return id;
    }

    void chunk(std::string_view hash, std::int64_t raw, std::int64_t stored) {
        auto insert = database->statement(
            "INSERT INTO chunks(hash, raw_size, compressed_size, object_path, created_at_ns) "
            "VALUES(:hash, :raw, :stored, :path, 0)");
        insert.bind(":hash", hash);
        insert.bind(":raw", raw);
        insert.bind(":stored", stored);
        insert.bind(":path", "objects/" + std::string(hash) + ".zst");
        insert.execute();
    }

    void reference(std::int64_t entry_id, std::string_view hash, std::int64_t sequence = 0) {
        auto insert = database->statement(
            "INSERT INTO entry_chunks(entry_id, sequence_number, chunk_hash, raw_offset, "
            "raw_length) SELECT :entry, :sequence, hash, :sequence * raw_size, raw_size "
            "FROM chunks WHERE hash = :hash");
        insert.bind(":entry", entry_id);
        insert.bind(":sequence", sequence);
        insert.bind(":hash", hash);
        insert.execute();
    }

    void warning(SnapshotId id, std::string_view path, std::string_view code,
                 std::string_view message) {
        MetadataStore(*database).insert_warning(id, path, code, message);
    }

    test::TemporaryDirectory temporary;
    const std::filesystem::path root = temporary.path() / "repository";
    std::unique_ptr<Repository> repository;
    std::unique_ptr<Database> database;
    std::unique_ptr<QueryService> query;
    const std::string hash_a = std::string(64, 'a');
    const std::string hash_b = std::string(64, 'b');
};

TEST_F(QueryServiceTest, EmptyRepositoryHasZeroStatisticsAndNoSnapshots) {
    const auto stats = query->repository_stats();
    EXPECT_EQ(stats.complete_snapshot_count, 0U);
    EXPECT_EQ(stats.unique_chunk_count, 0U);
    EXPECT_EQ(stats.logical_bytes, 0U);
    EXPECT_EQ(stats.unique_raw_bytes, 0U);
    EXPECT_EQ(stats.stored_bytes, 0U);
    EXPECT_DOUBLE_EQ(stats.deduplication_savings, 0.0);
    EXPECT_DOUBLE_EQ(stats.compression_savings, 0.0);
    EXPECT_DOUBLE_EQ(stats.total_savings, 0.0);
    const auto page = query->list_snapshots();
    EXPECT_EQ(page.total_count, 0U);
    EXPECT_TRUE(page.items.empty());
}

TEST_F(QueryServiceTest, SingleSnapshotStatisticsUseFilesAndReferencedChunksOnly) {
    const auto id = snapshot();
    entry(id, "", 900, "directory");
    entry(id, "link", 800, "symlink");
    const auto file = entry(id, "file", 100);
    chunk(hash_a, 100, 40);
    reference(file, hash_a);
    const auto stats = query->repository_stats();
    EXPECT_EQ(stats.complete_snapshot_count, 1U);
    EXPECT_EQ(stats.unique_chunk_count, 1U);
    EXPECT_EQ(stats.logical_bytes, 100U);
    EXPECT_EQ(stats.unique_raw_bytes, 100U);
    EXPECT_EQ(stats.stored_bytes, 40U);
    EXPECT_DOUBLE_EQ(stats.deduplication_savings, 0.0);
    EXPECT_DOUBLE_EQ(stats.compression_savings, 0.6);
    EXPECT_DOUBLE_EQ(stats.total_savings, 0.6);
    const auto selected = query->snapshot_stats(id);
    EXPECT_EQ(selected.complete_snapshot_count, 1U);
    EXPECT_EQ(selected.logical_bytes, 100U);
    EXPECT_EQ(selected.unique_chunk_count, 1U);
    EXPECT_EQ(selected.unique_raw_bytes, 100U);
    EXPECT_EQ(selected.stored_bytes, 40U);
    EXPECT_DOUBLE_EQ(selected.deduplication_savings, 0.0);
    EXPECT_DOUBLE_EQ(selected.compression_savings, 0.6);
    EXPECT_DOUBLE_EQ(selected.total_savings, 0.6);
}

TEST_F(QueryServiceTest, DuplicateChunksWithinSnapshotCountUniqueBytesOnce) {
    const auto id = snapshot();
    const auto file = entry(id, "double", 200);
    const auto copy = entry(id, "copy", 100);
    chunk(hash_a, 100, 40);
    reference(file, hash_a);
    reference(file, hash_a, 1);
    reference(copy, hash_a);
    const auto stats = query->repository_stats();
    EXPECT_EQ(stats.unique_chunk_count, 1U);
    EXPECT_EQ(stats.logical_bytes, 300U);
    EXPECT_EQ(stats.unique_raw_bytes, 100U);
    EXPECT_EQ(stats.stored_bytes, 40U);
    EXPECT_DOUBLE_EQ(stats.deduplication_savings, 1.0 - 100.0 / 300.0);
    EXPECT_DOUBLE_EQ(stats.total_savings, 1.0 - 40.0 / 300.0);
    const auto selected = query->snapshot_stats(id);
    EXPECT_EQ(selected.logical_bytes, 300U);
    EXPECT_EQ(selected.unique_chunk_count, 1U);
    EXPECT_EQ(selected.unique_raw_bytes, 100U);
    EXPECT_EQ(selected.stored_bytes, 40U);
    EXPECT_DOUBLE_EQ(selected.deduplication_savings, 1.0 - 100.0 / 300.0);
    EXPECT_DOUBLE_EQ(selected.compression_savings, 0.6);
    EXPECT_DOUBLE_EQ(selected.total_savings, 1.0 - 40.0 / 300.0);
}

TEST_F(QueryServiceTest, ChunksSharedAcrossSnapshotsCountUniqueBytesOnce) {
    const auto first = snapshot();
    const auto second = snapshot();
    chunk(hash_a, 100, 40);
    chunk(hash_b, 50, 30);
    reference(entry(first, "shared", 100), hash_a);
    reference(entry(second, "shared", 100), hash_a);
    reference(entry(second, "unique", 50), hash_b);
    const auto stats = query->repository_stats();
    EXPECT_EQ(stats.complete_snapshot_count, 2U);
    EXPECT_EQ(stats.unique_chunk_count, 2U);
    EXPECT_EQ(stats.logical_bytes, 250U);
    EXPECT_EQ(stats.unique_raw_bytes, 150U);
    EXPECT_EQ(stats.stored_bytes, 70U);
    EXPECT_DOUBLE_EQ(stats.deduplication_savings, 1.0 - 150.0 / 250.0);
    EXPECT_DOUBLE_EQ(stats.compression_savings, 1.0 - 70.0 / 150.0);
    const auto first_stats = query->snapshot_stats(first);
    EXPECT_EQ(first_stats.complete_snapshot_count, 1U);
    EXPECT_EQ(first_stats.logical_bytes, 100U);
    EXPECT_EQ(first_stats.unique_chunk_count, 1U);
    EXPECT_EQ(first_stats.unique_raw_bytes, 100U);
    EXPECT_EQ(first_stats.stored_bytes, 40U);
    const auto second_stats = query->snapshot_stats(second);
    EXPECT_EQ(second_stats.complete_snapshot_count, 1U);
    EXPECT_EQ(second_stats.logical_bytes, 150U);
    EXPECT_EQ(second_stats.unique_chunk_count, 2U);
    EXPECT_EQ(second_stats.unique_raw_bytes, 150U);
    EXPECT_EQ(second_stats.stored_bytes, 70U);
}

TEST_F(QueryServiceTest, IncompleteSnapshotsAndUnreferencedChunksAreExcluded) {
    chunk(hash_a, 100, 40);
    chunk(hash_b, 200, 80); // An unreferenced database row does not consume retained bytes.
    for (const auto status : {"pending", "failed", "cancelled", "deleting"}) {
        const auto id = snapshot(status);
        reference(entry(id, "hidden", 100), hash_a);
        EXPECT_THROW((void)query->get_snapshot(id), LocalVaultError);
        EXPECT_THROW((void)query->list_children(id, ""), LocalVaultError);
        EXPECT_THROW((void)query->search_paths(id, "hidden"), LocalVaultError);
        EXPECT_THROW((void)query->snapshot_stats(id), LocalVaultError);
        EXPECT_THROW((void)query->list_warnings(id), LocalVaultError);
    }
    const auto stats = query->repository_stats();
    EXPECT_EQ(stats.complete_snapshot_count, 0U);
    EXPECT_EQ(stats.unique_chunk_count, 0U);
    EXPECT_EQ(stats.logical_bytes, 0U);
    EXPECT_EQ(stats.unique_raw_bytes, 0U);
    EXPECT_EQ(stats.stored_bytes, 0U);
    EXPECT_EQ(query->list_snapshots().total_count, 0U);
}

TEST_F(QueryServiceTest, EmptyFilesHaveZeroDenominatorSavings) {
    const auto id = snapshot();
    entry(id, "empty", 0);
    const auto stats = query->repository_stats();
    EXPECT_EQ(stats.complete_snapshot_count, 1U);
    EXPECT_EQ(stats.unique_chunk_count, 0U);
    EXPECT_DOUBLE_EQ(stats.deduplication_savings, 0.0);
    EXPECT_DOUBLE_EQ(stats.compression_savings, 0.0);
    EXPECT_DOUBLE_EQ(stats.total_savings, 0.0);
    const auto selected = query->snapshot_stats(id);
    EXPECT_EQ(selected.complete_snapshot_count, 1U);
    EXPECT_EQ(selected.unique_chunk_count, 0U);
    EXPECT_EQ(selected.logical_bytes, 0U);
    EXPECT_EQ(selected.unique_raw_bytes, 0U);
    EXPECT_EQ(selected.stored_bytes, 0U);
    EXPECT_DOUBLE_EQ(selected.deduplication_savings, 0.0);
    EXPECT_DOUBLE_EQ(selected.compression_savings, 0.0);
    EXPECT_DOUBLE_EQ(selected.total_savings, 0.0);
}

TEST_F(QueryServiceTest, IncompressibleContentKeepsNegativeSavings) {
    chunk(hash_a, 100, 125);
    reference(entry(snapshot(), "random", 100), hash_a);
    const auto stats = query->repository_stats();
    EXPECT_DOUBLE_EQ(stats.compression_savings, -0.25);
    EXPECT_DOUBLE_EQ(stats.total_savings, -0.25);
}

TEST_F(QueryServiceTest, UnsignedStatisticsTotalsAllowSignedRangeAndRejectOverflow) {
    constexpr auto maximum = (std::numeric_limits<std::int64_t>::max)();
    const auto id = snapshot();
    entry(id, "one", maximum);
    entry(id, "two", maximum);
    EXPECT_EQ(query->repository_stats().logical_bytes, static_cast<std::uint64_t>(maximum) * 2U);
    EXPECT_EQ(query->snapshot_stats(id).logical_bytes, static_cast<std::uint64_t>(maximum) * 2U);
    entry(id, "three", 2);
    EXPECT_THROW((void)query->snapshot_stats(id), LocalVaultError);
    try {
        (void)query->repository_stats();
        FAIL() << "unsigned aggregate overflow must fail";
    } catch (const LocalVaultError& error) {
        EXPECT_EQ(error.code(), ErrorCode::database_error);
    }
}

TEST_F(QueryServiceTest, NegativeStoredLogicalSizesAreRejected) {
    const auto id = snapshot();
    entry(id, "negative", -1);
    EXPECT_THROW((void)query->snapshot_stats(id), LocalVaultError);
    try {
        (void)query->repository_stats();
        FAIL() << "negative sizes must fail";
    } catch (const LocalVaultError& error) {
        EXPECT_EQ(error.code(), ErrorCode::database_error);
    }
}

TEST_F(QueryServiceTest, ChunkStatisticsUseCheckedUnsignedAggregates) {
    constexpr auto maximum = (std::numeric_limits<std::int64_t>::max)();
    const auto id = snapshot();
    chunk(hash_a, maximum, maximum);
    chunk(hash_b, maximum, maximum);
    reference(entry(id, "one"), hash_a);
    reference(entry(id, "two"), hash_b);
    const auto stats = query->repository_stats();
    EXPECT_EQ(stats.unique_raw_bytes, static_cast<std::uint64_t>(maximum) * 2U);
    EXPECT_EQ(stats.stored_bytes, static_cast<std::uint64_t>(maximum) * 2U);
    const auto selected = query->snapshot_stats(id);
    EXPECT_EQ(selected.unique_raw_bytes, static_cast<std::uint64_t>(maximum) * 2U);
    EXPECT_EQ(selected.stored_bytes, static_cast<std::uint64_t>(maximum) * 2U);
    const std::string hash_c(64, 'c');
    chunk(hash_c, 2, 2);
    reference(entry(id, "three"), hash_c);
    EXPECT_THROW((void)query->repository_stats(), LocalVaultError);
    EXPECT_THROW((void)query->snapshot_stats(id), LocalVaultError);
}

TEST_F(QueryServiceTest, WarningsAreSnapshotScopedAndPagedWithDeterministicOrdering) {
    const auto id = snapshot();
    warning(snapshot(), "other", "unreadable", "another snapshot");
    EXPECT_EQ(query->list_warnings(id).total_count, 0U);
    warning(id, "zulu", "unreadable", "last path");
    warning(id, "Alpha", "z_code", "last code");
    warning(id, "Alpha", "a_code", "first duplicate");
    warning(id, "Alpha", "a_code", "second duplicate");
    const auto page = query->list_warnings(id, {1, 2});
    EXPECT_EQ(page.total_count, 4U);
    ASSERT_EQ(page.items.size(), 2U);
    EXPECT_EQ(page.items.front().relative_path, "Alpha");
    EXPECT_EQ(page.items.front().code, "a_code");
    EXPECT_EQ(page.items.front().message, "second duplicate");
    EXPECT_EQ(page.items.back().code, "z_code");
    EXPECT_EQ(page.items.back().message, "last code");
    EXPECT_EQ(query->list_warnings(id, {0, 1}).items.front().message, "first duplicate");
    EXPECT_EQ(query->list_warnings(id, {3, 1}).items.front().relative_path, "zulu");
    const auto beyond = query->list_warnings(id, {4, 1});
    EXPECT_EQ(beyond.total_count, 4U);
    EXPECT_TRUE(beyond.items.empty());
}

TEST_F(QueryServiceTest, SnapshotPagesAreNewestFirstWithStableIdTieBreakAndMetadata) {
    const auto oldest = snapshot("complete", 100);
    const auto newer = snapshot("complete", 200);
    const auto newest = snapshot("complete", 200);
    database->execute("UPDATE snapshots SET file_count = 3, directory_count = 2, "
                      "logical_size = 99, new_stored_size = 45, duration_ms = 12");
    const auto page = query->list_snapshots({1, 1});
    EXPECT_EQ(page.total_count, 3U);
    ASSERT_EQ(page.items.size(), 1U);
    EXPECT_EQ(page.items.front().id, newer);
    EXPECT_EQ(query->list_snapshots({0, 1}).items.front().id, newest);
    EXPECT_EQ(query->list_snapshots({2, 1}).items.front().id, oldest);
    const auto beyond = query->list_snapshots({99, 1});
    EXPECT_EQ(beyond.total_count, 3U);
    EXPECT_TRUE(beyond.items.empty());
    const auto info = query->get_snapshot(newest);
    EXPECT_EQ(info.message, "test snapshot");
    EXPECT_EQ(info.source_root.generic_string(), "/source");
    EXPECT_EQ(info.status, SnapshotStatus::complete);
    EXPECT_EQ(info.file_count, 3U);
    EXPECT_EQ(info.directory_count, 2U);
    EXPECT_EQ(info.logical_size, 99U);
    EXPECT_EQ(info.new_stored_size, 45U);
    EXPECT_EQ(info.duration.count(), 12);
}

TEST_F(QueryServiceTest, ChildrenArePagedDirectDescendantsAndExcludeRootSentinel) {
    const auto id = snapshot();
    entry(id, "", 0, "directory");
    entry(id, "zulu", 10);
    entry(id, "folder", 0, "directory");
    entry(id, "folder/beta", 2);
    entry(id, "folder/Alpha", 3);
    const auto page = query->list_children(id, ".", {0, 1});
    EXPECT_EQ(page.total_count, 2U);
    ASSERT_EQ(page.items.size(), 1U);
    EXPECT_EQ(page.items.front().relative_path, "folder");
    EXPECT_EQ(page.items.front().type, EntryType::directory);
    const auto child = query->list_children(id, "./folder/", {1, 1});
    EXPECT_EQ(child.total_count, 2U);
    ASSERT_EQ(child.items.size(), 1U);
    EXPECT_EQ(child.items.front().relative_path.generic_string(), "folder/beta");
    EXPECT_EQ(child.items.front().logical_size, 2U);
    EXPECT_EQ(child.items.front().file_hash_hex, std::string(64, 'f'));
    EXPECT_EQ(query->list_children(id, "missing").total_count, 0U);
}

TEST_F(QueryServiceTest, SearchIsLiteralCaseSensitiveAndPagedAcrossRelativePaths) {
    const auto id = snapshot();
    entry(id, "", 0, "directory");
    entry(id, "folder", 0, "directory");
    entry(id, "folder/a%_b", 10);
    entry(id, "folder/axb", 20);
    entry(id, "folder/Alpha", 30);
    const auto literal = query->search_paths(id, "%_");
    EXPECT_EQ(literal.total_count, 1U);
    ASSERT_EQ(literal.items.size(), 1U);
    EXPECT_EQ(literal.items.front().relative_path.generic_string(), "folder/a%_b");
    const auto page = query->search_paths(id, "folder/", {1, 1});
    EXPECT_EQ(page.total_count, 3U);
    ASSERT_EQ(page.items.size(), 1U);
    EXPECT_EQ(page.items.front().relative_path.generic_string(), "folder/a%_b");
    EXPECT_EQ(query->search_paths(id, "alpha").total_count, 0U);
    EXPECT_EQ(query->search_paths(id, "").total_count, 4U);
    EXPECT_EQ(query->search_paths(id, std::string_view{}).total_count, 4U);
    EXPECT_EQ(query->search_paths(id, "' OR 1=1 --").total_count, 0U);
    EXPECT_THROW((void)query->search_paths(id, std::string_view("a\0b", 3)), LocalVaultError);
}

TEST_F(QueryServiceTest, UnsafePathsInvalidIdsAndInvalidPagesAreRejected) {
    const auto id = snapshot();
    for (const auto parent : {"..", "folder/../other", "/absolute", "C:relative"}) {
        EXPECT_THROW((void)query->list_children(id, parent), LocalVaultError);
    }
    EXPECT_THROW((void)query->get_snapshot(-1), LocalVaultError);
    EXPECT_THROW((void)query->get_snapshot(999), LocalVaultError);
    EXPECT_THROW((void)query->list_children(999, ""), LocalVaultError);
    EXPECT_THROW((void)query->search_paths(999, ""), LocalVaultError);
    EXPECT_THROW((void)query->snapshot_stats(-1), LocalVaultError);
    EXPECT_THROW((void)query->snapshot_stats(999), LocalVaultError);
    EXPECT_THROW((void)query->list_warnings(-1), LocalVaultError);
    EXPECT_THROW((void)query->list_warnings(999), LocalVaultError);
    for (const auto page :
         {PageRequest{0, 0}, PageRequest{0, 10'001},
          PageRequest{(std::numeric_limits<std::uint64_t>::max)(), 1},
          PageRequest{static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()), 1}}) {
        EXPECT_THROW((void)query->list_snapshots(page), LocalVaultError);
        EXPECT_THROW((void)query->list_children(id, "", page), LocalVaultError);
        EXPECT_THROW((void)query->search_paths(id, "", page), LocalVaultError);
        EXPECT_THROW((void)query->list_warnings(id, page), LocalVaultError);
    }
}

TEST_F(QueryServiceTest, ReadOnlyQueriesWorkWhileWriterLockIsHeldWithoutReadingObjects) {
    const auto id = snapshot();
    chunk(hash_a, 100, 40);
    reference(entry(id, "file", 100), hash_a);
    auto read_only = Repository::open(root, OpenMode::read_only);
    auto lock = RepositoryLock::acquire_exclusive(root / "repository.lock");
    const QueryService reader(read_only);
    EXPECT_EQ(reader.list_snapshots().total_count, 1U);
    EXPECT_EQ(reader.get_snapshot(id).id, id);
    EXPECT_EQ(reader.list_children(id, "").total_count, 1U);
    EXPECT_EQ(reader.search_paths(id, "file").total_count, 1U);
    EXPECT_EQ(reader.repository_stats().stored_bytes, 40U);
    EXPECT_EQ(reader.snapshot_stats(id).stored_bytes, 40U);
    EXPECT_EQ(reader.list_warnings(id).total_count, 0U);
    EXPECT_FALSE(std::filesystem::exists(root / ("objects/" + hash_a + ".zst")));
}

} // namespace
} // namespace localvault
