#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "database/database.hpp"
#include "database/migrations.hpp"
#include "database/statement.hpp"
#include "localvault/error.hpp"

namespace {

class TemporaryDatabase final {
  public:
    TemporaryDatabase() {
        static std::atomic<unsigned int> sequence{0};
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("localvault_migrations_test_" + std::to_string(timestamp) + "_" +
                 std::to_string(sequence.fetch_add(1)) + ".db");
    }

    ~TemporaryDatabase() {
        std::error_code error;
        std::filesystem::remove(path_, error);
        std::filesystem::remove(path_.string() + "-wal", error);
        std::filesystem::remove(path_.string() + "-shm", error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

[[nodiscard]] std::int64_t scalar_int(localvault::Database& database, std::string_view sql) {
    auto query = database.statement(sql);
    EXPECT_TRUE(query.step());
    const std::int64_t result = query.column_int64(0);
    EXPECT_FALSE(query.step());
    return result;
}

[[nodiscard]] bool schema_object_exists(localvault::Database& database, std::string_view type,
                                        std::string_view name) {
    auto query = database.statement(
        "SELECT COUNT(*) FROM sqlite_schema WHERE type = :type AND name = :name");
    query.bind(":type", type);
    query.bind(":name", name);
    EXPECT_TRUE(query.step());
    const bool exists = query.column_int64(0) == 1;
    EXPECT_FALSE(query.step());
    return exists;
}

void expect_columns(localvault::Database& database, std::string_view table,
                    std::initializer_list<std::string_view> expected) {
    auto query = database.statement("SELECT name FROM pragma_table_info(:table_name) ORDER BY cid");
    query.bind(":table_name", table);

    std::vector<std::string> actual;
    while (query.step()) {
        actual.push_back(query.column_text(0));
    }
    std::vector<std::string> expected_strings;
    expected_strings.reserve(expected.size());
    for (const std::string_view column : expected) {
        expected_strings.emplace_back(column);
    }
    EXPECT_EQ(actual, expected_strings) << "table: " << table;
}

TEST(Migrations, AppliesMigrationOnceAndReopeningIsIdempotent) {
    TemporaryDatabase temporary;
    localvault::Database database(temporary.path());

    localvault::run_migrations(database);
    EXPECT_EQ(scalar_int(database, "SELECT COUNT(*) FROM schema_migrations"), 1);
    EXPECT_EQ(scalar_int(database, "SELECT MAX(version) FROM schema_migrations"), 1);
    EXPECT_GT(scalar_int(database, "SELECT applied_at_ns FROM schema_migrations WHERE version = 1"),
              0);
    const std::int64_t applied_at =
        scalar_int(database, "SELECT applied_at_ns FROM schema_migrations WHERE version = 1");

    localvault::run_migrations(database);

    EXPECT_EQ(scalar_int(database, "SELECT COUNT(*) FROM schema_migrations"), 1);
    EXPECT_EQ(scalar_int(database, "SELECT applied_at_ns FROM schema_migrations WHERE version = 1"),
              applied_at);
}

TEST(Migrations, InitialSchemaContainsAllRequiredObjectsAndColumns) {
    TemporaryDatabase temporary;
    localvault::Database database(temporary.path());
    localvault::run_migrations(database);

    for (const std::string_view table :
         {"schema_migrations", "repository_info", "snapshots", "entries", "chunks", "entry_chunks",
          "snapshot_warnings", "repository_settings"}) {
        EXPECT_TRUE(schema_object_exists(database, "table", table)) << table;
    }
    EXPECT_FALSE(schema_object_exists(database, "table", "operation_history"));

    for (const std::string_view index :
         {"idx_snapshots_status_created", "idx_entries_snapshot_parent_name",
          "idx_entries_snapshot_path", "idx_entry_chunks_hash", "idx_snapshot_warnings_snapshot"}) {
        EXPECT_TRUE(schema_object_exists(database, "index", index)) << index;
    }

    expect_columns(database, "schema_migrations", {"version", "applied_at_ns", "description"});
    expect_columns(database, "repository_info",
                   {"singleton_id", "repository_uuid", "format_version", "created_at_ns",
                    "application_version", "chunk_size_bytes", "zstd_level", "hash_algorithm",
                    "path_encoding"});
    expect_columns(database, "snapshots",
                   {"id", "created_at_ns", "completed_at_ns", "source_root", "message", "status",
                    "file_count", "directory_count", "symlink_count", "logical_size",
                    "new_stored_size", "new_chunk_count", "reused_chunk_count", "duration_ms",
                    "failure_message"});
    expect_columns(database, "entries",
                   {"id", "snapshot_id", "relative_path", "parent_path", "name", "entry_type",
                    "logical_size", "modified_time_ns", "change_time_ns", "posix_mode",
                    "windows_attributes", "file_hash", "symlink_target", "source_device",
                    "source_inode"});
    expect_columns(database, "chunks",
                   {"hash", "raw_size", "compressed_size", "object_path", "created_at_ns"});
    expect_columns(database, "entry_chunks",
                   {"entry_id", "sequence_number", "chunk_hash", "raw_offset", "raw_length"});
    expect_columns(database, "snapshot_warnings",
                   {"id", "snapshot_id", "relative_path", "warning_code", "message"});
    expect_columns(database, "repository_settings", {"key", "value", "updated_at_ns"});

    auto insert = database.statement("INSERT INTO snapshots (created_at_ns, source_root, status) "
                                     "VALUES (:created_at_ns, :source_root, :status)");
    insert.bind(":created_at_ns", std::int64_t{1});
    insert.bind(":source_root", "/source");
    insert.bind(":status", "deleting");
    EXPECT_NO_THROW(insert.execute());

    insert.reset();
    insert.bind(":created_at_ns", std::int64_t{2});
    insert.bind(":source_root", "/source");
    insert.bind(":status", "unknown");
    EXPECT_THROW(insert.execute(), localvault::LocalVaultError);
}

TEST(Migrations, FailureRollsBackEveryChange) {
    TemporaryDatabase temporary;
    localvault::Database database(temporary.path());
    database.execute(R"sql(CREATE TABLE schema_migrations (
        version INTEGER PRIMARY KEY,
        applied_at_ns INTEGER NOT NULL,
        description TEXT NOT NULL
    ))sql");
    database.execute(R"sql(CREATE TRIGGER fail_migration_record
        BEFORE INSERT ON schema_migrations
        BEGIN
            SELECT RAISE(ABORT, 'injected migration failure');
        END)sql");

    EXPECT_THROW(localvault::run_migrations(database), localvault::LocalVaultError);

    EXPECT_EQ(scalar_int(database, "SELECT COUNT(*) FROM schema_migrations"), 0);
    for (const std::string_view table :
         {"repository_info", "snapshots", "entries", "chunks", "entry_chunks", "snapshot_warnings",
          "repository_settings"}) {
        EXPECT_FALSE(schema_object_exists(database, "table", table)) << table;
    }
}

} // namespace
