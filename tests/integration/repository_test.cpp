#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "database/database.hpp"
#include "database/statement.hpp"
#include "filesystem/platform/platform_lock.hpp"
#include "localvault/error.hpp"
#include "localvault/repository.hpp"

namespace localvault {
namespace {

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
