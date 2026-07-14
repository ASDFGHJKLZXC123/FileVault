#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "database/database.hpp"
#include "database/metadata_store.hpp"
#include "database/statement.hpp"
#include "filesystem/platform/platform_lock.hpp"
#include "localvault/error.hpp"
#include "localvault/repository.hpp"
#include "localvault/restore_engine.hpp"
#include "localvault/snapshot_engine.hpp"

namespace localvault {

class QueryService final {
  public:
    [[nodiscard]] static std::shared_ptr<FailureInjector>
    failure_injector(const Repository& repository) noexcept {
        return repository.failure_injector();
    }

    static void recover_after_writer_lock(Repository& repository) {
        repository.recover_after_writer_lock();
    }

    static void set_recovery_entry_batch_limit(Repository& repository,
                                               std::size_t entry_batch_limit) {
        repository.set_recovery_entry_batch_limit_for_testing(entry_batch_limit);
    }
};

namespace {

static_assert(all_failure_points == std::array{
                                        FailurePoint::after_temp_object_write,
                                        FailurePoint::after_object_fsync,
                                        FailurePoint::after_object_rename,
                                        FailurePoint::before_metadata_batch_commit,
                                        FailurePoint::before_snapshot_publish,
                                        FailurePoint::during_restore_write,
                                    });

class RecordingFailureInjector final : public FailureInjector {
  public:
    void hit(FailurePoint point) override {
        hits.push_back(point);
    }

    std::vector<FailurePoint> hits;
};

class ThrowOnOccurrenceInjector final : public FailureInjector {
  public:
    explicit ThrowOnOccurrenceInjector(std::size_t occurrence) : occurrence_(occurrence) {}

    void hit(FailurePoint point) override {
        if (point == FailurePoint::before_metadata_batch_commit && ++hits_ == occurrence_) {
            throw std::runtime_error("injected recovery batch failure");
        }
    }

  private:
    std::size_t occurrence_{};
    std::size_t hits_{};
};

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("localvault-repository-test-" + std::to_string(suffix));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

[[nodiscard]] std::set<std::filesystem::path> entries_below(const std::filesystem::path& root) {
    std::set<std::filesystem::path> entries;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        entries.insert(entry.path().lexically_relative(root));
    }
    return entries;
}

void checkpoint_database(const std::filesystem::path& path) {
    Database database(path);
    auto checkpoint = database.statement("PRAGMA wal_checkpoint(TRUNCATE)");
    ASSERT_TRUE(checkpoint.step());
    ASSERT_EQ(checkpoint.column_int64(0), 0);
    ASSERT_FALSE(checkpoint.step());
}

void expect_invalid_repository(const std::filesystem::path& root) {
    try {
        (void)Repository::open(root, OpenMode::read_only);
        FAIL() << "opening an invalid repository should fail";
    } catch (const LocalVaultError& error) {
        EXPECT_EQ(error.code(), ErrorCode::invalid_repository);
    }
}

[[nodiscard]] SnapshotId insert_snapshot(Database& database, std::string_view status,
                                         std::int64_t created_at_ns) {
    auto insert = database.statement("INSERT INTO snapshots (created_at_ns, source_root, status) "
                                     "VALUES (:created_at_ns, '/source', :status) RETURNING id");
    insert.bind(":created_at_ns", created_at_ns);
    insert.bind(":status", status);
    EXPECT_TRUE(insert.step());
    const SnapshotId snapshot_id = insert.column_int64(0);
    EXPECT_FALSE(insert.step());
    return snapshot_id;
}

void insert_directory_entries(Database& database, SnapshotId snapshot_id, int count) {
    for (int index = 0; index < count; ++index) {
        const std::string path = "directory-" + std::to_string(index);
        auto insert = database.statement(
            "INSERT INTO entries (snapshot_id, relative_path, parent_path, name, entry_type) "
            "VALUES (:snapshot_id, :path, '', :path, 'directory')");
        insert.bind(":snapshot_id", snapshot_id);
        insert.bind(":path", path);
        insert.execute();
    }
}

void insert_warnings(Database& database, SnapshotId snapshot_id, int count) {
    auto insert = database.statement(
        "INSERT INTO snapshot_warnings (snapshot_id, relative_path, warning_code, message) "
        "VALUES (:snapshot_id, :path, 'test', 'warning')");
    for (int index = 0; index < count; ++index) {
        insert.bind(":snapshot_id", snapshot_id);
        insert.bind(":path", "warning-" + std::to_string(index));
        insert.execute();
        insert.reset();
    }
}

[[nodiscard]] std::int64_t entry_count(Database& database, SnapshotId snapshot_id) {
    auto query =
        database.statement("SELECT COUNT(*) FROM entries WHERE snapshot_id = :snapshot_id");
    query.bind(":snapshot_id", snapshot_id);
    EXPECT_TRUE(query.step());
    const std::int64_t count = query.column_int64(0);
    EXPECT_FALSE(query.step());
    return count;
}

TEST(RepositoryTest, CreateAndOpenRoundTripPreservesRepositoryInformation) {
    TemporaryDirectory temporary;
    const std::filesystem::path root = temporary.path() / "repository";
    RepositoryCreateOptions options;
    options.chunk_size_bytes = 2ULL * 1024ULL * 1024ULL;
    options.zstd_level = 7;

    Repository::create(root, options);
    Repository read_only = Repository::open(root, OpenMode::read_only);
    Repository read_write = Repository::open(root, OpenMode::read_write);

    EXPECT_EQ(read_only.root(), std::filesystem::absolute(root).lexically_normal());
    EXPECT_EQ(read_only.format_version(), 1U);
    EXPECT_EQ(read_only.info().repository_uuid, read_write.info().repository_uuid);
    EXPECT_EQ(read_only.info().format_version, 1U);
    EXPECT_EQ(read_only.info().chunk_size_bytes, options.chunk_size_bytes);
    EXPECT_EQ(read_only.info().zstd_level, options.zstd_level);
    EXPECT_EQ(read_only.info().hash_algorithm, "blake3");
    EXPECT_FALSE(read_only.info().repository_uuid.empty());
}

TEST(RepositoryTest, FailureInjectorDefaultsToNoopAndNullRestoresIt) {
    TemporaryDirectory temporary;
    const std::filesystem::path root = temporary.path() / "repository";
    Repository::create(root);
    Repository repository = Repository::open(root, OpenMode::read_write);

    const std::shared_ptr<FailureInjector> default_injector =
        QueryService::failure_injector(repository);
    ASSERT_NE(default_injector, nullptr);
    for (const FailurePoint point : all_failure_points) {
        EXPECT_NO_THROW(default_injector->hit(point));
    }

    const auto recording_injector = std::make_shared<RecordingFailureInjector>();
    repository.set_failure_injector(recording_injector);
    const std::shared_ptr<FailureInjector> installed_injector =
        QueryService::failure_injector(repository);
    ASSERT_EQ(installed_injector, recording_injector);
    for (const FailurePoint point : all_failure_points) {
        installed_injector->hit(point);
    }
    EXPECT_EQ(recording_injector->hits,
              std::vector<FailurePoint>(all_failure_points.begin(), all_failure_points.end()));

    repository.set_failure_injector(nullptr);
    const std::shared_ptr<FailureInjector> restored_injector =
        QueryService::failure_injector(repository);
    ASSERT_NE(restored_injector, nullptr);
    EXPECT_EQ(restored_injector, default_injector);
    EXPECT_NO_THROW(restored_injector->hit(FailurePoint::before_snapshot_publish));
    EXPECT_EQ(recording_injector->hits.size(), all_failure_points.size());
}

TEST(RepositoryTest, FirstSnapshotOperationRecoversSnapshotsAndTemporaryTreeButKeepsOrphans) {
    TemporaryDirectory temporary;
    const std::filesystem::path root = temporary.path() / "repository";
    const std::filesystem::path source = temporary.path() / "source";
    std::filesystem::create_directory(source);
    Repository::create(root);

    SnapshotId stale_pending{};
    SnapshotId failed{};
    SnapshotId cancelled{};
    SnapshotId deleting{};
    {
        Database database(root / "repository.db");
        stale_pending = insert_snapshot(database, "pending", 1);
        failed = insert_snapshot(database, "failed", 2);
        cancelled = insert_snapshot(database, "cancelled", 3);
        deleting = insert_snapshot(database, "deleting", 4);
        insert_directory_entries(database, stale_pending, 2);
        insert_directory_entries(database, failed, 1);
        insert_directory_entries(database, cancelled, 1);
        insert_directory_entries(database, deleting, 2);
        auto warning = database.statement(
            "INSERT INTO snapshot_warnings (snapshot_id, relative_path, warning_code, message) "
            "VALUES (:snapshot_id, 'warning', 'test', 'warning')");
        for (const SnapshotId snapshot_id : {stale_pending, failed, cancelled, deleting}) {
            warning.bind(":snapshot_id", snapshot_id);
            warning.execute();
            warning.reset();
        }
    }

    std::ofstream(root / "temporary" / "objects" / "stale-object.tmp") << "stale";
    std::ofstream(root / "temporary" / "restores" / "stale-restore.tmp") << "stale";
    std::filesystem::create_directories(root / "temporary" / "other" / "nested");
    std::ofstream(root / "temporary" / "other" / "nested" / "stale.tmp") << "stale";
    const std::filesystem::path orphan = root / "objects" / "aa" / "orphan.zst";
    std::filesystem::create_directories(orphan.parent_path());
    std::ofstream(orphan) << "orphan final object";

    Repository repository = Repository::open(root, OpenMode::read_write);
    const SnapshotResult created =
        SnapshotEngine(repository).create_snapshot(source, SnapshotOptions{});

    Database database(root / "repository.db");
    auto recovered = database.statement(
        "SELECT status, failure_message, "
        "(SELECT COUNT(*) FROM entries WHERE snapshot_id = :snapshot_id), "
        "(SELECT COUNT(*) FROM snapshot_warnings WHERE snapshot_id = :snapshot_id) "
        "FROM snapshots WHERE id = :snapshot_id");
    recovered.bind(":snapshot_id", stale_pending);
    ASSERT_TRUE(recovered.step());
    EXPECT_EQ(recovered.column_text(0), "failed");
    EXPECT_FALSE(recovered.column_is_null(1));
    EXPECT_EQ(recovered.column_int64(2), 0);
    EXPECT_EQ(recovered.column_int64(3), 0);
    for (const SnapshotId snapshot_id : {failed, cancelled}) {
        EXPECT_EQ(entry_count(database, snapshot_id), 0);
    }
    auto deleting_row = database.statement("SELECT COUNT(*) FROM snapshots WHERE id = :id");
    deleting_row.bind(":id", deleting);
    ASSERT_TRUE(deleting_row.step());
    EXPECT_EQ(deleting_row.column_int64(0), 0);
    auto new_status = database.statement("SELECT status FROM snapshots WHERE id = :id");
    new_status.bind(":id", created.snapshot_id);
    ASSERT_TRUE(new_status.step());
    EXPECT_EQ(new_status.column_text(0), "complete");

    EXPECT_TRUE(std::filesystem::is_empty(root / "temporary" / "objects"));
    EXPECT_TRUE(std::filesystem::is_empty(root / "temporary" / "restores"));
    EXPECT_FALSE(std::filesystem::exists(root / "temporary" / "other"));
    EXPECT_TRUE(std::filesystem::is_regular_file(orphan));
    MetadataStore metadata(database);
    EXPECT_NO_THROW(metadata.quick_relationship_check());
}

TEST(RepositoryTest, FirstRestoreOperationRunsRecoveryBeforeReadingSnapshotMetadata) {
    TemporaryDirectory temporary;
    const std::filesystem::path root = temporary.path() / "repository";
    const std::filesystem::path source = temporary.path() / "source";
    const std::filesystem::path destination =
        std::filesystem::canonical(temporary.path()) / "destination";
    std::filesystem::create_directory(source);
    std::ofstream(source / "file.txt") << "preserved complete snapshot";
    Repository::create(root);

    SnapshotId complete{};
    {
        Repository repository = Repository::open(root, OpenMode::read_write);
        complete =
            SnapshotEngine(repository).create_snapshot(source, SnapshotOptions{}).snapshot_id;
    }

    SnapshotId stale_pending{};
    {
        Database database(root / "repository.db");
        stale_pending = insert_snapshot(database, "pending", 2);
        insert_directory_entries(database, stale_pending, 1);
    }
    std::ofstream(root / "temporary" / "restores" / "stale.tmp") << "stale";

    Repository repository = Repository::open(root, OpenMode::read_write);
    RestoreRequest request;
    request.snapshot_id = complete;
    request.destination_root = destination;
    const RestoreResult restored = RestoreEngine(repository).restore(request);
    EXPECT_EQ(restored.restored_files, 1U);
    EXPECT_TRUE(std::filesystem::is_regular_file(destination / "file.txt"));
    EXPECT_TRUE(std::filesystem::is_empty(root / "temporary" / "restores"));

    Database database(root / "repository.db");
    auto recovered = database.statement(
        "SELECT status, (SELECT COUNT(*) FROM entries WHERE snapshot_id = :snapshot_id) "
        "FROM snapshots WHERE id = :snapshot_id");
    recovered.bind(":snapshot_id", stale_pending);
    ASSERT_TRUE(recovered.step());
    EXPECT_EQ(recovered.column_text(0), "failed");
    EXPECT_EQ(recovered.column_int64(1), 0);
}

TEST(RepositoryTest, InterruptedDeletingRecoveryRetriesAndFinishesOnTheSameOpenRepository) {
    TemporaryDirectory temporary;
    const std::filesystem::path root = temporary.path() / "repository";
    Repository::create(root);
    SnapshotId deleting{};
    {
        Database database(root / "repository.db");
        deleting = insert_snapshot(database, "deleting", 1);
        insert_directory_entries(database, deleting, 3);
    }

    Repository repository = Repository::open(root, OpenMode::read_write);
    QueryService::set_recovery_entry_batch_limit(repository, 1);
    repository.set_failure_injector(std::make_shared<ThrowOnOccurrenceInjector>(2));
    {
        RepositoryLock writer_lock = RepositoryLock::acquire_exclusive(root / "repository.lock");
        EXPECT_THROW(QueryService::recover_after_writer_lock(repository), std::runtime_error);
        (void)writer_lock;
    }
    {
        Database database(root / "repository.db");
        EXPECT_EQ(entry_count(database, deleting), 2);
        auto status = database.statement("SELECT status FROM snapshots WHERE id = :id");
        status.bind(":id", deleting);
        ASSERT_TRUE(status.step());
        EXPECT_EQ(status.column_text(0), "deleting");
    }

    repository.set_failure_injector(nullptr);
    {
        RepositoryLock writer_lock = RepositoryLock::acquire_exclusive(root / "repository.lock");
        EXPECT_NO_THROW(QueryService::recover_after_writer_lock(repository));
        (void)writer_lock;
    }
    Database database(root / "repository.db");
    auto remaining = database.statement("SELECT COUNT(*) FROM snapshots WHERE id = :id");
    remaining.bind(":id", deleting);
    ASSERT_TRUE(remaining.step());
    EXPECT_EQ(remaining.column_int64(0), 0);
}

TEST(RepositoryTest, InterruptedPendingCleanupRetriesWithoutLosingFailureInformation) {
    TemporaryDirectory temporary;
    const std::filesystem::path root = temporary.path() / "repository";
    Repository::create(root);
    SnapshotId pending{};
    {
        Database database(root / "repository.db");
        pending = insert_snapshot(database, "pending", 1);
        insert_directory_entries(database, pending, 3);
    }

    Repository repository = Repository::open(root, OpenMode::read_write);
    QueryService::set_recovery_entry_batch_limit(repository, 1);
    repository.set_failure_injector(std::make_shared<ThrowOnOccurrenceInjector>(3));
    {
        RepositoryLock writer_lock = RepositoryLock::acquire_exclusive(root / "repository.lock");
        EXPECT_THROW(QueryService::recover_after_writer_lock(repository), std::runtime_error);
        (void)writer_lock;
    }
    {
        Database database(root / "repository.db");
        EXPECT_EQ(entry_count(database, pending), 2);
        auto failed =
            database.statement("SELECT status, failure_message FROM snapshots WHERE id = :id");
        failed.bind(":id", pending);
        ASSERT_TRUE(failed.step());
        EXPECT_EQ(failed.column_text(0), "failed");
        EXPECT_FALSE(failed.column_is_null(1));
    }

    repository.set_failure_injector(nullptr);
    {
        RepositoryLock writer_lock = RepositoryLock::acquire_exclusive(root / "repository.lock");
        EXPECT_NO_THROW(QueryService::recover_after_writer_lock(repository));
        (void)writer_lock;
    }
    Database database(root / "repository.db");
    EXPECT_EQ(entry_count(database, pending), 0);
    auto failed = database.statement("SELECT status FROM snapshots WHERE id = :id");
    failed.bind(":id", pending);
    ASSERT_TRUE(failed.step());
    EXPECT_EQ(failed.column_text(0), "failed");
}

TEST(RepositoryTest, InterruptedWarningCleanupFinishesAfterRepositoryReopen) {
    TemporaryDirectory temporary;
    const std::filesystem::path root = temporary.path() / "repository";
    Repository::create(root);
    SnapshotId pending{};
    {
        Database database(root / "repository.db");
        pending = insert_snapshot(database, "pending", 1);
        insert_warnings(database, pending, 3);
    }

    {
        Repository repository = Repository::open(root, OpenMode::read_write);
        QueryService::set_recovery_entry_batch_limit(repository, 1);
        repository.set_failure_injector(std::make_shared<ThrowOnOccurrenceInjector>(4));
        RepositoryLock writer_lock = RepositoryLock::acquire_exclusive(root / "repository.lock");
        EXPECT_THROW(QueryService::recover_after_writer_lock(repository), std::runtime_error);
    }
    {
        Database database(root / "repository.db");
        auto residue = database.statement(
            "SELECT status, failure_message, "
            "(SELECT COUNT(*) FROM snapshot_warnings WHERE snapshot_id = :snapshot_id), "
            "(SELECT COUNT(*) FROM snapshot_warnings WHERE snapshot_id = :snapshot_id "
            "AND relative_path = 'warning-0') FROM snapshots WHERE id = :snapshot_id");
        residue.bind(":snapshot_id", pending);
        ASSERT_TRUE(residue.step());
        EXPECT_EQ(residue.column_text(0), "failed");
        EXPECT_FALSE(residue.column_is_null(1));
        EXPECT_EQ(residue.column_int64(2), 2);
        EXPECT_EQ(residue.column_int64(3), 0);
    }

    {
        Repository reopened = Repository::open(root, OpenMode::read_write);
        QueryService::set_recovery_entry_batch_limit(reopened, 1);
        RepositoryLock writer_lock = RepositoryLock::acquire_exclusive(root / "repository.lock");
        EXPECT_NO_THROW(QueryService::recover_after_writer_lock(reopened));
    }
    Database database(root / "repository.db");
    auto recovered = database.statement(
        "SELECT status, failure_message, "
        "(SELECT COUNT(*) FROM snapshot_warnings WHERE snapshot_id = :snapshot_id) "
        "FROM snapshots WHERE id = :snapshot_id");
    recovered.bind(":snapshot_id", pending);
    ASSERT_TRUE(recovered.step());
    EXPECT_EQ(recovered.column_text(0), "failed");
    EXPECT_FALSE(recovered.column_is_null(1));
    EXPECT_EQ(recovered.column_int64(2), 0);
    MetadataStore metadata(database);
    EXPECT_NO_THROW(metadata.quick_relationship_check());
}

TEST(RepositoryTest, ChunkSizeMetadataIsPermanentlyBoundedByFourMiB) {
    TemporaryDirectory temporary;
    const std::filesystem::path zero_root = temporary.path() / "zero";
    const std::filesystem::path oversized_root = temporary.path() / "oversized";
    RepositoryCreateOptions options;

    options.chunk_size_bytes = 0;
    EXPECT_THROW(Repository::create(zero_root, options), LocalVaultError);
    EXPECT_FALSE(std::filesystem::exists(zero_root));

    options.chunk_size_bytes = 4ULL * 1024ULL * 1024ULL + 1ULL;
    EXPECT_THROW(Repository::create(oversized_root, options), LocalVaultError);
    EXPECT_FALSE(std::filesystem::exists(oversized_root));

    const std::filesystem::path corrupt_root = temporary.path() / "corrupt";
    Repository::create(corrupt_root);
    {
        Database database(corrupt_root / "repository.db");
        database.execute("UPDATE repository_info SET chunk_size_bytes = 4194305");
    }
    expect_invalid_repository(corrupt_root);
}

TEST(RepositoryTest, RandomDirectoryIsNotARepository) {
    TemporaryDirectory temporary;
    std::ofstream(temporary.path() / "unrelated.txt") << "user data";

    try {
        (void)Repository::open(temporary.path(), OpenMode::read_only);
        FAIL() << "opening a random directory should fail";
    } catch (const LocalVaultError& error) {
        EXPECT_EQ(error.code(), ErrorCode::invalid_repository);
    }
}

TEST(RepositoryTest, FutureFormatVersionIsRejectedClearly) {
    TemporaryDirectory temporary;
    const std::filesystem::path root = temporary.path() / "repository";
    Repository::create(root);
    {
        Database database(root / "repository.db");
        database.execute("UPDATE repository_info SET format_version = 2");
    }

    try {
        (void)Repository::open(root, OpenMode::read_only);
        FAIL() << "opening a future repository format should fail";
    } catch (const LocalVaultError& error) {
        EXPECT_EQ(error.code(), ErrorCode::unsupported_repository_version);
        EXPECT_NE(std::string(error.what()).find("format version 2"), std::string::npos);
    }
}

TEST(RepositoryTest, RealFormatVersionIsRejectedWithoutIntegerTruncation) {
    TemporaryDirectory temporary;
    const std::filesystem::path root = temporary.path() / "repository";
    Repository::create(root);
    {
        Database database(root / "repository.db");
        database.execute("UPDATE repository_info SET format_version = 1.5");
    }

    expect_invalid_repository(root);
}

TEST(RepositoryTest, UnsupportedPathEncodingIsRejected) {
    TemporaryDirectory temporary;
    const std::filesystem::path root = temporary.path() / "repository";
    Repository::create(root);
    {
        Database database(root / "repository.db");
        database.execute("UPDATE repository_info SET path_encoding = 'utf-16'");
    }

    expect_invalid_repository(root);
}

TEST(RepositoryTest, MissingRequiredSchemaObjectIsRejected) {
    TemporaryDirectory temporary;
    const std::filesystem::path root = temporary.path() / "repository";
    Repository::create(root);
    {
        Database database(root / "repository.db");
        database.execute("DROP INDEX idx_entries_snapshot_path");
    }

    expect_invalid_repository(root);
}

TEST(RepositoryTest, UnexpectedMigrationVersionIsRejected) {
    TemporaryDirectory temporary;
    const std::filesystem::path root = temporary.path() / "repository";
    Repository::create(root);
    {
        Database database(root / "repository.db");
        database.execute("INSERT INTO schema_migrations VALUES (2, 0, 'unexpected')");
    }

    expect_invalid_repository(root);
}

TEST(RepositoryTest, ExistingNonEmptyDirectoryRequiresApproval) {
    TemporaryDirectory temporary;
    const std::filesystem::path root = temporary.path() / "repository";
    std::filesystem::create_directory(root);
    std::ofstream(root / "keep.txt") << "preserve me";

    try {
        Repository::create(root);
        FAIL() << "creation in a non-empty directory should require approval";
    } catch (const LocalVaultError& error) {
        EXPECT_EQ(error.code(), ErrorCode::invalid_argument);
    }
    EXPECT_TRUE(std::filesystem::exists(root / "keep.txt"));
    EXPECT_FALSE(std::filesystem::exists(root / "repository.db"));
}

TEST(RepositoryTest, ApprovedNonEmptyDirectoryPreservesExistingContent) {
    TemporaryDirectory temporary;
    const std::filesystem::path root = temporary.path() / "repository";
    std::filesystem::create_directory(root);
    std::ofstream(root / "keep.txt") << "preserve me";
    RepositoryCreateOptions options;
    options.allow_existing_non_empty = true;

    Repository::create(root, options);

    std::ifstream preserved(root / "keep.txt");
    std::string contents;
    std::getline(preserved, contents);
    EXPECT_EQ(contents, "preserve me");
    EXPECT_EQ(Repository::open(root, OpenMode::read_only).format_version(), 1U);
}

TEST(RepositoryTest, ApprovedDirectoryStillRejectsReservedPathCollisions) {
    TemporaryDirectory temporary;
    const std::filesystem::path root = temporary.path() / "repository";
    std::filesystem::create_directories(root / "objects");
    std::ofstream(root / "objects" / "keep.txt") << "preserve me";
    RepositoryCreateOptions options;
    options.allow_existing_non_empty = true;

    EXPECT_THROW(Repository::create(root, options), LocalVaultError);
    EXPECT_TRUE(std::filesystem::exists(root / "objects" / "keep.txt"));
}

TEST(RepositoryTest, ExistingSqliteSidecarCollisionIsPreserved) {
    TemporaryDirectory temporary;
    const std::filesystem::path root = temporary.path() / "repository";
    std::filesystem::create_directory(root);
    std::ofstream(root / "repository.db-wal") << "pre-existing sidecar";
    RepositoryCreateOptions options;
    options.allow_existing_non_empty = true;

    EXPECT_THROW(Repository::create(root, options), LocalVaultError);

    std::ifstream sidecar(root / "repository.db-wal");
    std::string contents;
    std::getline(sidecar, contents);
    EXPECT_EQ(contents, "pre-existing sidecar");
    EXPECT_FALSE(std::filesystem::exists(root / "repository.db"));
}

TEST(RepositoryTest, ReadOnlyOpenCreatesNoEntriesAndTakesNoWriterLock) {
    TemporaryDirectory temporary;
    const std::filesystem::path root = temporary.path() / "repository";
    Repository::create(root);
    const std::set<std::filesystem::path> before = entries_below(root);

    Repository repository = Repository::open(root, OpenMode::read_only);
    RepositoryLock writer_lock = RepositoryLock::acquire_exclusive(root / "repository.lock");

    EXPECT_EQ(entries_below(root), before);
    EXPECT_EQ(repository.info().format_version, 1U);
    (void)writer_lock;
}

TEST(RepositoryTest, WritableRepositoryWithoutWalSidecarsIsRejectedWithoutCreatingPaths) {
    TemporaryDirectory temporary;
    const std::filesystem::path root = temporary.path() / "repository";
    Repository::create(root);
    checkpoint_database(root / "repository.db");
    std::filesystem::remove(root / "repository.db-wal");
    std::filesystem::remove(root / "repository.db-shm");
    const std::set<std::filesystem::path> before = entries_below(root);

    expect_invalid_repository(root);

    EXPECT_EQ(entries_below(root), before);
}

TEST(RepositoryTest, ReadOnlyOpenRejectsAnIncompleteWalSidecarPair) {
    TemporaryDirectory temporary;
    const std::filesystem::path root = temporary.path() / "repository";
    Repository::create(root);
    std::filesystem::remove(root / "repository.db-shm");
    const std::set<std::filesystem::path> before = entries_below(root);

    expect_invalid_repository(root);

    EXPECT_EQ(entries_below(root), before);
}

#ifndef _WIN32
TEST(RepositoryTest, ChmodOnlyRepositoryWithoutWalSidecarsIsRejected) {
    TemporaryDirectory temporary;
    const std::filesystem::path root = temporary.path() / "repository";
    Repository::create(root);
    checkpoint_database(root / "repository.db");
    std::filesystem::remove(root / "repository.db-wal");
    std::filesystem::remove(root / "repository.db-shm");
    ASSERT_EQ(::chmod(root.c_str(), 0500), 0);
    const std::set<std::filesystem::path> before = entries_below(root);

    expect_invalid_repository(root);

    EXPECT_EQ(entries_below(root), before);
    ASSERT_EQ(::chmod(root.c_str(), 0700), 0);
}

TEST(RepositoryTest, CreateRejectsSymlinkRepositoryRootWithoutPopulatingTarget) {
    TemporaryDirectory temporary;
    const std::filesystem::path target = temporary.path() / "target";
    const std::filesystem::path root = temporary.path() / "repository";
    std::filesystem::create_directory(target);
    std::filesystem::create_directory_symlink(target, root);
    RepositoryCreateOptions options;
    options.allow_existing_non_empty = true;

    EXPECT_THROW(Repository::create(root, options), LocalVaultError);
    EXPECT_TRUE(std::filesystem::is_empty(target));
}

TEST(RepositoryTest, OpenRejectsSymlinkLayoutEntry) {
    TemporaryDirectory temporary;
    const std::filesystem::path root = temporary.path() / "repository";
    const std::filesystem::path replacement = temporary.path() / "replacement";
    Repository::create(root);
    std::filesystem::create_directory(replacement);
    std::filesystem::remove(root / "logs");
    std::filesystem::create_directory_symlink(replacement, root / "logs");

    expect_invalid_repository(root);
}
#endif

TEST(RepositoryTest, FilesystemRootIsRejectedBeforeMutation) {
#ifdef _WIN32
    const std::filesystem::path filesystem_root =
        std::filesystem::temp_directory_path().root_path();
#else
    const std::filesystem::path filesystem_root = "/";
#endif
    RepositoryCreateOptions options;
    options.allow_existing_non_empty = true;

    try {
        Repository::create(filesystem_root, options);
        FAIL() << "filesystem root creation should fail";
    } catch (const LocalVaultError& error) {
        EXPECT_EQ(error.code(), ErrorCode::invalid_argument);
    }
}

#ifndef _WIN32
TEST(RepositoryTest, RepositoryRootHasOwnerOnlyPermissions) {
    TemporaryDirectory temporary;
    const std::filesystem::path root = temporary.path() / "repository";

    Repository::create(root);

    struct stat status{};
    ASSERT_EQ(::stat(root.c_str(), &status), 0);
    EXPECT_EQ(status.st_mode & 0777, 0700);
}
#endif

} // namespace
} // namespace localvault
