#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

#include "database/database.hpp"
#include "database/statement.hpp"
#include "database/transaction.hpp"
#include "localvault/error.hpp"

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
