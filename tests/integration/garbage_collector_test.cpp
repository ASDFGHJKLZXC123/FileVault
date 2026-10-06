#include <gtest/gtest.h>
#include <sqlite3.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "database/database.hpp"
#include "database/metadata_store.hpp"
#include "database/statement.hpp"
#include "filesystem/platform/platform_lock.hpp"
#include "localvault/error.hpp"
#include "localvault/garbage_collector.hpp"
#include "localvault/repository.hpp"
#include "localvault/restore_engine.hpp"
#include "localvault/snapshot_engine.hpp"
#include "storage/object_store.hpp"
#include "support/test_filesystem.hpp"

namespace localvault {
namespace {

using TreeContents = std::map<std::string, std::vector<std::byte>>;

TreeContents capture_tree(const std::filesystem::path& root) {
    TreeContents result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        const auto relative = entry.path().lexically_relative(root).generic_string();
        if (entry.is_directory()) {
            result.emplace(relative + "/", std::vector<std::byte>{});
        } else {
            result.emplace(relative, test::read_all_bytes(entry.path()));
        }
    }
    return result;
}

class ThrowOnGcDelete final : public FailureInjector {
  public:
    void hit(FailurePoint point) override {
        if (point == FailurePoint::after_gc_object_delete) {
            throw std::runtime_error("interrupted after GC object deletion");
        }
    }
};

class CancelDeleting final : public FailureInjector {
  public:
    explicit CancelDeleting(std::stop_source& source) : source_(source) {}
    void hit(FailurePoint point) override {
        if (point == FailurePoint::before_metadata_batch_commit && ++commits_ == 1) {
            source_.request_stop();
        }
    }

  private:
    std::stop_source& source_;
    unsigned commits_{};
};

class GarbageCollectorTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Repository::create(root);
        repository = std::make_unique<Repository>(Repository::open(root));
        std::filesystem::create_directory(source);
    }
    SnapshotId snapshot() {
        return SnapshotEngine(*repository).create_snapshot(source, {}).snapshot_id;
    }
    void sql(std::string_view statement) {
        Database database(root / "repository.db");
        database.execute(statement);
    }
    std::int64_t scalar(std::string_view statement) {
        Database database(root / "repository.db");
        auto query = database.statement(statement);
        if (!query.step()) {
            throw std::runtime_error("scalar query returned no row");
        }
        return query.column_int64(0);
    }
    std::filesystem::path first_object() {
        Database database(root / "repository.db");
        auto query = database.statement("SELECT object_path FROM chunks ORDER BY hash LIMIT 1");
        if (!query.step()) {
            throw std::runtime_error("fixture has no chunk");
        }
        return root / query.column_text(0);
    }
    std::filesystem::path orphan() {
        const auto path = root / ObjectStore::object_relative_path(std::string(64, 'a'));
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << "orphan object";
        return path;
    }

    test::TemporaryDirectory temporary;
    std::filesystem::path root{temporary.path() / "repository"};
    std::filesystem::path source{temporary.path() / "source"};
    std::unique_ptr<Repository> repository;
};

TEST_F(GarbageCollectorTest, DeleteThenGcPreservesSharedChunksAndRestoresOlderSnapshot) {
    test::DatasetBuilder(source)
        .text_file("shared.txt", "same retained payload")
        .text_file("copy.txt", "same retained payload");
    const auto older = snapshot();
    test::DatasetBuilder(source).text_file("newer-only.txt", "only newer snapshot owns this");
    const auto newer = snapshot();
    GarbageCollector gc(*repository);
    gc.delete_snapshot(newer);
    EXPECT_EQ(scalar("SELECT COUNT(*) FROM chunks"), 2);
    EXPECT_EQ(scalar("SELECT COUNT(*) FROM snapshots"), 1);
    const auto preview = gc.collect();
    EXPECT_EQ(preview.unreferenced_chunks, 1U);
    EXPECT_EQ(preview.removed_objects, 0U);
    const auto collected = gc.collect({.dry_run = false, .batch_size = 1});
    EXPECT_EQ(collected.unreferenced_chunks, 1U);
    EXPECT_EQ(collected.removed_objects, 1U);
    EXPECT_EQ(collected.reclaimed_bytes, preview.reclaimable_bytes);
    EXPECT_EQ(scalar("SELECT COUNT(*) FROM chunks"), 1);
    RestoreRequest request;
    request.snapshot_id = older;
    request.destination_root = std::filesystem::canonical(temporary.path()) / "restored";
    const auto restored = RestoreEngine(*repository).restore(request);
    EXPECT_EQ(restored.restored_files, 2U);
    test::expect_file_bytes_equal(source / "shared.txt", request.destination_root / "shared.txt");
    test::expect_file_bytes_equal(source / "copy.txt", request.destination_root / "copy.txt");
    EXPECT_FALSE(std::filesystem::exists(request.destination_root / "newer-only.txt"));
    EXPECT_EQ(gc.collect({.dry_run = false}).removed_objects, 0U);
}

TEST_F(GarbageCollectorTest, PreviewLeavesEntireRepositoryAndDatabaseByteIdentical) {
    test::DatasetBuilder(source).text_file("file.txt", "unreferenced payload");
    GarbageCollector gc(*repository);
    gc.delete_snapshot(snapshot());
    const auto orphan_path = orphan();
    test::DatasetBuilder(root / "temporary").text_file("objects/stale.tmp", "stale bytes");
    const auto before = capture_tree(root);
    const auto preview = gc.collect();
    const auto after = capture_tree(root);
    EXPECT_EQ(before, after);
    EXPECT_EQ(preview.unreferenced_chunks, 1U);
    EXPECT_EQ(preview.orphan_objects, 1U);
    EXPECT_EQ(preview.stale_temporary_files, 1U);
    EXPECT_GT(preview.reclaimable_bytes, std::filesystem::file_size(orphan_path));
    const auto collected = gc.collect({.dry_run = false});
    EXPECT_EQ(collected.removed_objects, 2U);
    EXPECT_EQ(collected.reclaimed_bytes, preview.reclaimable_bytes);
    EXPECT_FALSE(std::filesystem::exists(orphan_path));
    EXPECT_FALSE(std::filesystem::exists(root / "temporary/objects/stale.tmp"));
    EXPECT_EQ(scalar("SELECT COUNT(*) FROM chunks"), 0);
}

TEST_F(GarbageCollectorTest, ClosedSidecarFreeMaintenancePreviewIsByteIdenticalAndCannotExecute) {
    const auto orphan_path = orphan();
    repository.reset();
    {
        Database database(root / "repository.db");
        auto checkpoint = database.statement("PRAGMA wal_checkpoint(TRUNCATE)");
        ASSERT_TRUE(checkpoint.step());
        ASSERT_EQ(checkpoint.column_int64(0), 0);
        ASSERT_FALSE(checkpoint.step());
    }
    std::filesystem::remove(root / "repository.db-wal");
    std::filesystem::remove(root / "repository.db-shm");
    const auto before = capture_tree(root);
    {
        auto maintenance = Repository::open(root, OpenMode::maintenance_read_only);
        GarbageCollector gc(maintenance);
        const auto preview = gc.collect();
        EXPECT_EQ(preview.orphan_objects, 1U);
        EXPECT_EQ(preview.reclaimable_bytes, std::filesystem::file_size(orphan_path));
        EXPECT_THROW((void)gc.collect({.dry_run = false}), LocalVaultError);
        EXPECT_THROW(gc.delete_snapshot(1), LocalVaultError);
    }
    EXPECT_EQ(capture_tree(root), before);
    EXPECT_FALSE(std::filesystem::exists(root / "repository.db-wal"));
    EXPECT_FALSE(std::filesystem::exists(root / "repository.db-shm"));
}

TEST_F(GarbageCollectorTest, StandaloneMaintenancePreviewReadsLiveWalWithoutChangingAnyBytes) {
    repository.reset();
    {
        Database writer(root / "repository.db");
        int enabled{};
        ASSERT_EQ(sqlite3_db_config(writer.handle(), SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, &enabled),
                  SQLITE_OK);
        writer.execute("INSERT INTO snapshots(created_at_ns, source_root, status) "
                       "VALUES(0, '/source', 'pending')");
    }
    ASSERT_GT(std::filesystem::file_size(root / "repository.db-wal"), 0U);
    const auto before = capture_tree(root);
    {
        auto maintenance = Repository::open(root, OpenMode::maintenance_read_only);
        const auto preview = GarbageCollector(maintenance).collect();
        EXPECT_EQ(preview.stale_snapshots, 1U);
    }
    EXPECT_EQ(capture_tree(root), before);
}

TEST_F(GarbageCollectorTest, PreviewPredictsRecoveryWithoutChangingStaleReferences) {
    test::DatasetBuilder(source).text_file("file.txt", "stale snapshot payload");
    (void)snapshot();
    sql("UPDATE snapshots SET status = 'pending'");
    const auto before = capture_tree(root);
    GarbageCollector gc(*repository);
    const auto preview = gc.collect();
    EXPECT_EQ(preview.stale_snapshots, 1U);
    EXPECT_EQ(preview.unreferenced_chunks, 1U);
    EXPECT_EQ(before, capture_tree(root));
    const auto collected = gc.collect({.dry_run = false});
    EXPECT_EQ(collected.unreferenced_chunks, 1U);
    EXPECT_EQ(collected.reclaimed_bytes, preview.reclaimable_bytes);
    EXPECT_EQ(scalar("SELECT COUNT(*) FROM snapshots WHERE status = 'failed'"), 1);
    EXPECT_EQ(scalar("SELECT COUNT(*) FROM entries"), 0);
    EXPECT_EQ(scalar("SELECT COUNT(*) FROM chunks"), 0);
}

TEST_F(GarbageCollectorTest, InterruptedObjectDeletionLeavesRecoverableUnreferencedRow) {
    test::DatasetBuilder(source).text_file("file.txt", "gc interrupted payload");
    GarbageCollector gc(*repository);
    gc.delete_snapshot(snapshot());
    const auto object = first_object();
    repository->set_failure_injector(std::make_shared<ThrowOnGcDelete>());
    EXPECT_THROW((void)gc.collect({.dry_run = false}), std::runtime_error);
    EXPECT_FALSE(std::filesystem::exists(object));
    EXPECT_EQ(scalar("SELECT COUNT(*) FROM chunks"), 1);
    repository->set_failure_injector(nullptr);
    const auto preview = gc.collect();
    EXPECT_EQ(preview.unreferenced_chunks, 1U);
    EXPECT_EQ(preview.reclaimable_bytes, 0U);
    const auto result = gc.collect({.dry_run = false});
    EXPECT_EQ(result.unreferenced_chunks, 1U);
    EXPECT_EQ(result.removed_objects, 0U);
    EXPECT_EQ(scalar("SELECT COUNT(*) FROM chunks"), 0);
}

TEST_F(GarbageCollectorTest, PreviewPredictsOrphansAfterStaleReferencesAreRecovered) {
    for (const std::string status : {"pending", "failed", "deleting"}) {
        test::DatasetBuilder(source).text_file("file.txt", "stale orphan payload");
        const auto id = snapshot();
        sql("UPDATE snapshots SET status='" + status + "' WHERE id=" + std::to_string(id));
        {
            Database database(root / "repository.db");
            database.execute("PRAGMA foreign_keys=OFF");
            database.execute("DELETE FROM chunks");
        }
        const auto before = capture_tree(root);
        GarbageCollector gc(*repository);
        const auto preview = gc.collect();
        EXPECT_EQ(before, capture_tree(root));
        EXPECT_EQ(preview.orphan_objects, 1U) << status;
        const auto collected = gc.collect({.dry_run = false});
        EXPECT_EQ(collected.orphan_objects, preview.orphan_objects) << status;
        EXPECT_EQ(collected.reclaimed_bytes, preview.reclaimable_bytes) << status;
    }
}

TEST_F(GarbageCollectorTest, RejectsRedirectedObjectDirectoriesWithoutTouchingTargets) {
    test::DatasetBuilder(source).text_file("file.txt", "retained outside payload");
    GarbageCollector gc(*repository);
    gc.delete_snapshot(snapshot());
    const auto object = first_object();
    for (const auto& directory : {root / "objects", object.parent_path()}) {
        const auto outside = temporary.path() / "moved-objects";
        std::filesystem::rename(directory, outside);
        std::error_code error;
        std::filesystem::create_directory_symlink(outside, directory, error);
        if (error) {
            std::filesystem::rename(outside, directory);
            GTEST_SKIP() << "directory symlink unavailable: " << error.message();
        }
        const auto before = capture_tree(outside);
        EXPECT_THROW((void)gc.collect({.dry_run = false}), LocalVaultError);
        EXPECT_EQ(before, capture_tree(outside));
        std::filesystem::remove(directory);
        std::filesystem::rename(outside, directory);
    }
}

#ifndef _WIN32
TEST_F(GarbageCollectorTest, RejectsRepositoryRootRedirectedAfterOpen) {
    test::DatasetBuilder(source).text_file("file.txt", "unreferenced payload");
    GarbageCollector(*repository).delete_snapshot(snapshot());
    const auto relative = first_object().lexically_relative(root);
    const auto moved = temporary.path() / "moved-repository";
    const auto outside = temporary.path() / "outside";
    Repository::create(outside);
    std::filesystem::create_directories((outside / relative).parent_path());
    std::ofstream(outside / relative) << "outside object sentinel";
    test::DatasetBuilder(outside / "temporary")
        .text_file("objects/stale.tmp", "outside temp sentinel");
    std::filesystem::rename(root, moved);
    std::filesystem::create_directory_symlink(outside, root);
    const auto before = capture_tree(outside);
    // Construct GC after redirection, so only Repository's original trust baseline can help.
    EXPECT_THROW((void)GarbageCollector(*repository).collect({.dry_run = false}), LocalVaultError);
    EXPECT_EQ(before, capture_tree(outside));
    std::filesystem::remove(root);
    std::filesystem::rename(moved, root);
}
#endif

TEST_F(GarbageCollectorTest, CancelledDeletionRemainsResumableAndRetainsObjects) {
    test::DatasetBuilder(source).text_file("file.txt", "cancel deletion payload");
    const auto id = snapshot();
    const auto object = first_object();
    std::stop_source stop;
    repository->set_failure_injector(std::make_shared<CancelDeleting>(stop));
    EXPECT_THROW(GarbageCollector(*repository).delete_snapshot(id, stop.get_token()),
                 LocalVaultError);
    EXPECT_EQ(scalar("SELECT COUNT(*) FROM snapshots WHERE status = 'deleting'"), 1);
    EXPECT_TRUE(std::filesystem::exists(object));
    repository->set_failure_injector(nullptr);
    EXPECT_NO_THROW((void)GarbageCollector(*repository).collect({.dry_run = false}));
    EXPECT_EQ(scalar("SELECT COUNT(*) FROM snapshots"), 0);
    EXPECT_FALSE(std::filesystem::exists(object));
}

TEST_F(GarbageCollectorTest, RejectsPendingAndMissingSnapshotsWithoutPublishingDeletion) {
    test::DatasetBuilder(source).text_file("file.txt", "pending payload");
    const auto id = snapshot();
    sql("UPDATE snapshots SET status = 'pending'");
    GarbageCollector gc(*repository);
    EXPECT_THROW(gc.delete_snapshot(id), LocalVaultError);
    EXPECT_THROW(gc.delete_snapshot(id + 100), LocalVaultError);
    EXPECT_EQ(scalar("SELECT COUNT(*) FROM snapshots WHERE status = 'pending'"), 1);
    EXPECT_EQ(scalar("SELECT COUNT(*) FROM chunks"), 1);
}

TEST_F(GarbageCollectorTest, RejectsReadOnlyAndCompetingOperations) {
    auto read_only = Repository::open(root, OpenMode::read_only);
    EXPECT_THROW((void)GarbageCollector(read_only).collect(), LocalVaultError);
    EXPECT_THROW(GarbageCollector(read_only).delete_snapshot(1), LocalVaultError);
    const auto lock = RepositoryLock::acquire_exclusive(root / "repository.lock");
    try {
        (void)GarbageCollector(*repository).collect();
        FAIL() << "GC must take the exclusive lock even for a preview";
    } catch (const LocalVaultError& error) {
        EXPECT_EQ(error.code(), ErrorCode::repository_busy);
    }
}

TEST_F(GarbageCollectorTest, CancellationAndInvalidOptionsPrecedeDeletion) {
    const auto path = orphan();
    GarbageCollector gc(*repository);
    EXPECT_THROW((void)gc.collect({.dry_run = false, .batch_size = 0}), LocalVaultError);
    std::stop_source stop;
    EXPECT_THROW((void)gc.collect({.dry_run = false}, stop.get_token(),
                                  [&](const ProgressEvent&) { stop.request_stop(); }),
                 LocalVaultError);
    EXPECT_TRUE(std::filesystem::exists(path));
}

TEST_F(GarbageCollectorTest, InvalidStoredPathCannotDeleteOutsideRepository) {
    test::DatasetBuilder(source).text_file("file.txt", "payload");
    GarbageCollector gc(*repository);
    gc.delete_snapshot(snapshot());
    test::DatasetBuilder(temporary.path()).text_file("sentinel.txt", "keep me");
    sql("UPDATE chunks SET object_path = '../sentinel.txt'");
    EXPECT_THROW((void)gc.collect({.dry_run = false}), LocalVaultError);
    EXPECT_EQ(test::read_all_bytes(temporary.path() / "sentinel.txt").size(), 7U);
    EXPECT_EQ(scalar("SELECT COUNT(*) FROM chunks"), 1);
}

TEST_F(GarbageCollectorTest, IgnoresNonCanonicalOrphanNamesAndRetainsLiveObjects) {
    test::DatasetBuilder(source).text_file("file.txt", "live payload");
    (void)snapshot();
    const auto live = first_object();
    const auto live_bytes = test::read_all_bytes(live);
    test::DatasetBuilder(root / "objects")
        .text_file("aa/unknown.tmp", "keep unknown file")
        .text_file("unknown/keep.zst", "keep unknown shard");
    const auto result = GarbageCollector(*repository).collect({.dry_run = false});
    EXPECT_EQ(result.removed_objects, 0U);
    EXPECT_EQ(result.ignored_paths, 2U);
    EXPECT_EQ(test::read_all_bytes(live), live_bytes);
    EXPECT_TRUE(std::filesystem::exists(root / "objects/aa/unknown.tmp"));
    EXPECT_TRUE(std::filesystem::exists(root / "objects/unknown/keep.zst"));
}

TEST_F(GarbageCollectorTest, RejectsTemporaryIndirectionWithoutTouchingItsTarget) {
    test::DatasetBuilder(temporary.path() / "outside").text_file("sentinel.txt", "outside payload");
    std::error_code error;
    const auto link = root / "temporary/objects/link";
    std::filesystem::create_directory_symlink(temporary.path() / "outside", link, error);
    if (error) {
        GTEST_SKIP() << "directory symlink unavailable: " << error.message();
    }
    EXPECT_THROW((void)GarbageCollector(*repository).collect({.dry_run = false}), LocalVaultError);
    EXPECT_TRUE(std::filesystem::exists(temporary.path() / "outside/sentinel.txt"));
    std::filesystem::remove(link);
}

} // namespace
} // namespace localvault
