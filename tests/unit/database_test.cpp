#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "database/database.hpp"
#include "database/statement.hpp"
#include "database/transaction.hpp"
#include "filesystem/platform/platform_lock.hpp"
#include "localvault/error.hpp"
#include "support/test_filesystem.hpp"

namespace {

class TemporaryDatabase final {
  public:
    TemporaryDatabase() {
        static std::atomic<unsigned int> sequence{0};
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = std::filesystem::temp_directory_path() /
                ("localvault_database_test_" + std::to_string(timestamp) + "_" +
                 std::to_string(sequence.fetch_add(1)));
        std::filesystem::create_directory(root_);
#ifdef _WIN32
        path_ = root_ / "repository #%25.db";
#else
        path_ = root_ / "repository #?%25.db";
#endif
    }

    ~TemporaryDatabase() {
        std::error_code error;
        std::filesystem::permissions(root_, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add, error);
        std::filesystem::remove_all(root_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
    }

  private:
    std::filesystem::path root_;
    std::filesystem::path path_;
};

[[nodiscard]] std::set<std::string> directory_entry_names(const std::filesystem::path& root) {
    std::set<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        names.insert(entry.path().filename().string());
    }
    return names;
}

[[nodiscard]] std::int64_t scalar_int(localvault::Database& database, std::string_view sql) {
    auto statement = database.statement(sql);
    if (!statement.step()) {
        throw std::runtime_error("query returned no row");
    }
    return statement.column_int64(0);
}

TEST(Database, ConfiguresEveryConnection) {
    TemporaryDatabase temporary;
    localvault::Database database(temporary.path());

    EXPECT_EQ(scalar_int(database, "PRAGMA foreign_keys"), 1);
    EXPECT_EQ(scalar_int(database, "PRAGMA synchronous"), 2);
    EXPECT_EQ(scalar_int(database, "PRAGMA busy_timeout"), 5000);

    auto journal_mode = database.statement("PRAGMA journal_mode");
    ASSERT_TRUE(journal_mode.step());
    EXPECT_EQ(journal_mode.column_text(0), "wal");
}

TEST(Statement, EmptyTextBindingsRemainDistinctFromNull) {
    TemporaryDatabase temporary;
    localvault::Database database(temporary.path());
    auto query = database.statement(
        "SELECT :default_empty, typeof(:default_empty), :literal_empty, :bytes, :null");
    query.bind(":default_empty", std::string_view{});
    query.bind(":literal_empty", "");
    query.bind(":bytes", std::string_view("a\0b", 3));
    query.bind_null(":null");

    ASSERT_TRUE(query.step());
    ASSERT_FALSE(query.column_is_null(0));
    EXPECT_EQ(query.column_text(0), "");
    EXPECT_EQ(query.column_text(1), "text");
    ASSERT_FALSE(query.column_is_null(2));
    EXPECT_EQ(query.column_text(2), "");
    EXPECT_EQ(query.column_text(3), std::string("a\0b", 3));
    EXPECT_TRUE(query.column_is_null(4));
    EXPECT_FALSE(query.step());
}

TEST(Database, ReadOnlyConnectionUsesRequiredSettings) {
    TemporaryDatabase temporary;
    {
        localvault::Database writable(temporary.path());
        writable.execute("CREATE TABLE item (id INTEGER PRIMARY KEY)");
    }

    localvault::Database read_only(temporary.path(), true);
    EXPECT_EQ(scalar_int(read_only, "PRAGMA foreign_keys"), 1);
    EXPECT_EQ(scalar_int(read_only, "PRAGMA synchronous"), 2);
    EXPECT_EQ(scalar_int(read_only, "PRAGMA busy_timeout"), 5000);
}

TEST(Database, ReadOnlyConnectionCreatesNoDirectoryEntries) {
    TemporaryDatabase temporary;
    {
        localvault::Database writable(temporary.path());
        writable.execute("CREATE TABLE item (id INTEGER PRIMARY KEY)");
    }

    const auto entries_before = directory_entry_names(temporary.root());
    const std::string filename = temporary.path().filename().string();
    EXPECT_EQ(entries_before,
              (std::set<std::string>{filename, filename + "-shm", filename + "-wal"}));
    {
        localvault::Database read_only(temporary.path(), true);
        EXPECT_EQ(scalar_int(read_only, "SELECT COUNT(*) FROM item"), 0);
        EXPECT_EQ(directory_entry_names(temporary.root()), entries_before);
    }
    EXPECT_EQ(directory_entry_names(temporary.root()), entries_before);
}

TEST(Database, LockedReadOnlyReadsLiveWalWithoutChangingAnyDatabaseFile) {
    TemporaryDatabase temporary;
    localvault::Database writable(temporary.path());
    writable.execute("CREATE TABLE item (id INTEGER PRIMARY KEY)");
    // Place the schema in the main database, then leave the row only in the WAL.
    auto checkpoint = writable.statement("PRAGMA wal_checkpoint(TRUNCATE)");
    ASSERT_TRUE(checkpoint.step());
    ASSERT_EQ(checkpoint.column_int64(0), 0);
    ASSERT_FALSE(checkpoint.step());
    writable.execute("INSERT INTO item (id) VALUES (42)");
    ASSERT_GT(std::filesystem::file_size(temporary.path().string() + "-wal"), 0U);

    auto lock = localvault::RepositoryLock::acquire_exclusive(temporary.root() / "repository.lock");
    const auto capture = [&] {
        std::map<std::string, std::vector<std::byte>> files;
        for (const std::string suffix : {"", "-wal", "-shm"}) {
            auto path = temporary.path();
            path += suffix;
            files.emplace(suffix, localvault::test::read_all_bytes(path));
        }
        return files;
    };
    const auto entries_before = directory_entry_names(temporary.root());
    const auto before = capture();
    {
        localvault::Database reader(temporary.path(), localvault::DatabaseAccess::locked_read_only);
        EXPECT_EQ(scalar_int(reader, "SELECT id FROM item"), 42);
        EXPECT_EQ(capture(), before);
        EXPECT_EQ(directory_entry_names(temporary.root()), entries_before);
        EXPECT_THROW(reader.execute("INSERT INTO item (id) VALUES (43)"),
                     localvault::LocalVaultError);
        EXPECT_EQ(capture(), before);
    }
    EXPECT_EQ(capture(), before);
    EXPECT_EQ(directory_entry_names(temporary.root()), entries_before);
    EXPECT_EQ(scalar_int(writable, "SELECT COUNT(*) FROM item"), 1);
}

TEST(Database, LockedReadOnlyRejectsMissingOrNonregularWalWithoutCreatingFiles) {
    TemporaryDatabase temporary;
    {
        localvault::Database writable(temporary.path());
        writable.execute("CREATE TABLE item (id INTEGER PRIMARY KEY)");
    }
    auto wal_path = temporary.path();
    wal_path += "-wal";
    ASSERT_TRUE(std::filesystem::remove(wal_path));
    const auto before = localvault::test::read_all_bytes(temporary.path());
    const auto entries_before = directory_entry_names(temporary.root());
    EXPECT_THROW(localvault::Database(temporary.path(), localvault::DatabaseAccess::locked_read_only),
                 localvault::LocalVaultError);
    EXPECT_EQ(localvault::test::read_all_bytes(temporary.path()), before);
    EXPECT_EQ(directory_entry_names(temporary.root()), entries_before);
    ASSERT_TRUE(std::filesystem::create_directory(wal_path));
    EXPECT_THROW(localvault::Database(temporary.path(), localvault::DatabaseAccess::locked_read_only),
                 localvault::LocalVaultError);
    EXPECT_TRUE(std::filesystem::is_directory(wal_path));
}

TEST(Database, LockedReadOnlyPreservesWalWhenMainDatabaseIsEmpty) {
    TemporaryDatabase temporary;
    {
        localvault::Database writable(temporary.path());
        writable.execute("CREATE TABLE item (id INTEGER PRIMARY KEY)");
    }
    auto wal_path = temporary.path();
    wal_path += "-wal";
    const auto wal_before = localvault::test::read_all_bytes(wal_path);
    localvault::test::truncate_file(temporary.path(), 0);
    const auto entries_before = directory_entry_names(temporary.root());
    EXPECT_THROW(localvault::Database(temporary.path(), localvault::DatabaseAccess::locked_read_only),
                 localvault::LocalVaultError);
    EXPECT_EQ(std::filesystem::file_size(temporary.path()), 0U);
    EXPECT_EQ(localvault::test::read_all_bytes(wal_path), wal_before);
    EXPECT_EQ(directory_entry_names(temporary.root()), entries_before);
}

TEST(Database, ExplicitImmutableReadOnlyCreatesNoFilesForEncodedPath) {
    TemporaryDatabase temporary;
    {
        localvault::Database writable(temporary.path());
        writable.execute("CREATE TABLE item (id INTEGER PRIMARY KEY)");
        writable.execute("INSERT INTO item (id) VALUES (1)");
        auto checkpoint = writable.statement("PRAGMA wal_checkpoint(TRUNCATE)");
        while (checkpoint.step()) {
        }
    }

    ASSERT_TRUE(std::filesystem::remove(temporary.path().string() + "-wal"));
    ASSERT_TRUE(std::filesystem::remove(temporary.path().string() + "-shm"));
    const auto entries_before = directory_entry_names(temporary.root());
#ifndef _WIN32
    std::filesystem::permissions(temporary.path(), std::filesystem::perms::owner_read,
                                 std::filesystem::perm_options::replace);
    std::filesystem::permissions(
        temporary.root(), std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace);
#endif

    {
        localvault::Database immutable(temporary.path(),
                                       localvault::DatabaseAccess::immutable_read_only);
        EXPECT_EQ(scalar_int(immutable, "SELECT COUNT(*) FROM item"), 1);
        EXPECT_EQ(scalar_int(immutable, "PRAGMA foreign_keys"), 1);
        EXPECT_EQ(scalar_int(immutable, "PRAGMA synchronous"), 2);
        EXPECT_EQ(scalar_int(immutable, "PRAGMA busy_timeout"), 5000);
        EXPECT_EQ(directory_entry_names(temporary.root()), entries_before);
    }
    EXPECT_EQ(directory_entry_names(temporary.root()), entries_before);
}

#ifndef _WIN32
[[nodiscard]] int child_database_lock_probe(const std::filesystem::path& path) {
    const auto* filename = path.c_str();
    const pid_t child = ::fork();
    if (child < 0) {
        return -1;
    }
    if (child == 0) {
        const int descriptor = ::open(filename, O_RDWR);
        if (descriptor < 0) {
            ::_exit(3);
        }
        struct flock lock {};
        lock.l_type = F_WRLCK;
        lock.l_whence = SEEK_SET;
        lock.l_start = 0x40000002; // SQLite's SHARED_FIRST byte.
        lock.l_len = 510;         // SQLite's SHARED_SIZE range.
        const int result = ::fcntl(descriptor, F_SETLK, &lock);
        ::_exit(result == 0 ? 1 : (errno == EACCES || errno == EAGAIN ? 0 : 2));
    }
    int status = 0;
    pid_t waited = 0;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    return waited == child && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

TEST(Database, LockedReadOnlyClosePreservesNativeDatabaseLocksAcrossProcesses) {
    TemporaryDatabase temporary;
    localvault::Database writable(temporary.path());
    writable.execute("CREATE TABLE item (id INTEGER PRIMARY KEY)");
    writable.execute("INSERT INTO item (id) VALUES (42)");
    auto lock = localvault::RepositoryLock::acquire_exclusive(temporary.root() / "repository.lock");
    // Do not open/read/close this DB through std::ifstream here: POSIX close would
    // itself release this process's fcntl locks and invalidate the regression.
    ASSERT_EQ(child_database_lock_probe(temporary.path()), 0);
    {
        localvault::Database reader(temporary.path(), localvault::DatabaseAccess::locked_read_only);
        EXPECT_EQ(scalar_int(reader, "SELECT id FROM item"), 42);
        EXPECT_EQ(child_database_lock_probe(temporary.path()), 0);
    }
    EXPECT_EQ(child_database_lock_probe(temporary.path()), 0);
    EXPECT_THROW(
        {
            localvault::Database reader(temporary.path(),
                                       localvault::DatabaseAccess::locked_read_only);
            EXPECT_EQ(scalar_int(reader, "SELECT id FROM item"), 42);
            throw std::runtime_error("injected reader failure");
        },
        std::runtime_error);
    EXPECT_EQ(child_database_lock_probe(temporary.path()), 0);
}

TEST(Database, ReadOnlyConnectionWorksWithoutWritePermissions) {
    TemporaryDatabase temporary;
    {
        localvault::Database writable(temporary.path());
        writable.execute("CREATE TABLE item (id INTEGER PRIMARY KEY)");
        writable.execute("INSERT INTO item (id) VALUES (1)");
    }

    const auto entries_before = directory_entry_names(temporary.root());
    for (const auto& entry : std::filesystem::directory_iterator(temporary.root())) {
        std::filesystem::permissions(entry.path(), std::filesystem::perms::owner_read,
                                     std::filesystem::perm_options::replace);
    }
    std::filesystem::permissions(
        temporary.root(), std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace);

    localvault::Database read_only(temporary.path(), true);
    EXPECT_EQ(scalar_int(read_only, "SELECT COUNT(*) FROM item"), 1);
    EXPECT_EQ(directory_entry_names(temporary.root()), entries_before);
}
#endif

TEST(Transaction, ExceptionRollsBackAllRows) {
    TemporaryDatabase temporary;
    localvault::Database database(temporary.path());
    database.execute("CREATE TABLE item (id INTEGER PRIMARY KEY, value TEXT NOT NULL)");

    EXPECT_THROW(
        {
            localvault::Transaction transaction(database);
            auto insert = database.statement("INSERT INTO item (id, value) VALUES (:id, :value)");
            insert.bind(":id", std::int64_t{1});
            insert.bind(":value", "first");
            insert.execute();
            insert.reset();
            insert.bind(":id", std::int64_t{2});
            insert.bind(":value", "second");
            insert.execute();
            throw std::runtime_error("injected failure");
        },
        std::runtime_error);

    EXPECT_EQ(scalar_int(database, "SELECT COUNT(*) FROM item"), 0);
}

TEST(Transaction, RollbackFailureDuringUnwindingPreservesOriginalException) {
    struct OriginalFailure final {
        const void* identity;
    };

    TemporaryDatabase temporary;
    localvault::Database database(temporary.path());
    const int identity = 42;
    bool caught_original = false;
    try {
        localvault::Transaction transaction(database);
        database.execute("ROLLBACK");
        throw OriginalFailure{&identity};
    } catch (const OriginalFailure& failure) {
        caught_original = true;
        EXPECT_EQ(failure.identity, &identity);
    } catch (...) {
        FAIL() << "transaction destructor replaced the propagating exception";
    }
    EXPECT_TRUE(caught_original);
}

TEST(Transaction, CommitPersistsRows) {
    TemporaryDatabase temporary;
    localvault::Database database(temporary.path());
    database.execute("CREATE TABLE item (id INTEGER PRIMARY KEY)");

    {
        localvault::Transaction transaction(database);
        database.execute("INSERT INTO item (id) VALUES (1)");
        transaction.commit();
    }

    EXPECT_EQ(scalar_int(database, "SELECT COUNT(*) FROM item"), 1);
}

TEST(Database, EnforcesForeignKeys) {
    TemporaryDatabase temporary;
    localvault::Database database(temporary.path());
    database.execute("CREATE TABLE parent (id INTEGER PRIMARY KEY)");
    database.execute("CREATE TABLE child (parent_id INTEGER NOT NULL REFERENCES parent(id))");

    try {
        auto insert = database.statement("INSERT INTO child (parent_id) VALUES (:parent_id)");
        insert.bind(":parent_id", std::int64_t{99});
        insert.execute();
        FAIL() << "foreign-key-violating insert unexpectedly succeeded";
    } catch (const localvault::LocalVaultError& error) {
        EXPECT_EQ(error.code(), localvault::ErrorCode::database_error);
    }
}

} // namespace
