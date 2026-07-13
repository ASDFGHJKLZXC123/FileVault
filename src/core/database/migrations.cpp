#include "database/migrations.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <string_view>

#include "database/database.hpp"
#include "database/statement.hpp"
#include "database/transaction.hpp"
#include "localvault/error.hpp"

namespace localvault {
namespace {

constexpr std::array<std::string_view, 12> migration_001_sql{
    R"sql(CREATE TABLE repository_info (
        singleton_id INTEGER PRIMARY KEY CHECK (singleton_id = 1),
        repository_uuid TEXT NOT NULL UNIQUE,
        format_version INTEGER NOT NULL,
        created_at_ns INTEGER NOT NULL,
        application_version TEXT NOT NULL,
        chunk_size_bytes INTEGER NOT NULL CHECK (chunk_size_bytes > 0),
        zstd_level INTEGER NOT NULL,
        hash_algorithm TEXT NOT NULL CHECK (hash_algorithm = 'blake3'),
        path_encoding TEXT NOT NULL DEFAULT 'utf-8'
    ))sql",
    R"sql(CREATE TABLE snapshots (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        created_at_ns INTEGER NOT NULL,
        completed_at_ns INTEGER,
        source_root TEXT NOT NULL,
        message TEXT NOT NULL DEFAULT '',
        status TEXT NOT NULL
            CHECK (status IN ('pending', 'complete', 'failed', 'cancelled', 'deleting')),
        file_count INTEGER NOT NULL DEFAULT 0,
        directory_count INTEGER NOT NULL DEFAULT 0,
        symlink_count INTEGER NOT NULL DEFAULT 0,
        logical_size INTEGER NOT NULL DEFAULT 0,
        new_stored_size INTEGER NOT NULL DEFAULT 0,
        new_chunk_count INTEGER NOT NULL DEFAULT 0,
        reused_chunk_count INTEGER NOT NULL DEFAULT 0,
        duration_ms INTEGER NOT NULL DEFAULT 0,
        failure_message TEXT
    ))sql",
    "CREATE INDEX idx_snapshots_status_created "
    "ON snapshots(status, created_at_ns DESC)",
    R"sql(CREATE TABLE entries (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        snapshot_id INTEGER NOT NULL REFERENCES snapshots(id) ON DELETE CASCADE,
        relative_path TEXT NOT NULL,
        parent_path TEXT NOT NULL,
        name TEXT NOT NULL,
        entry_type TEXT NOT NULL CHECK (entry_type IN ('file', 'directory', 'symlink')),
        logical_size INTEGER NOT NULL DEFAULT 0,
        modified_time_ns INTEGER NOT NULL DEFAULT 0,
        change_time_ns INTEGER,
        posix_mode INTEGER NOT NULL DEFAULT 0,
        windows_attributes INTEGER,
        file_hash TEXT,
        symlink_target TEXT,
        source_device INTEGER,
        source_inode INTEGER,
        UNIQUE(snapshot_id, relative_path)
    ))sql",
    "CREATE INDEX idx_entries_snapshot_parent_name "
    "ON entries(snapshot_id, parent_path, name)",
    "CREATE INDEX idx_entries_snapshot_path ON entries(snapshot_id, relative_path)",
    R"sql(CREATE TABLE chunks (
        hash TEXT PRIMARY KEY,
        raw_size INTEGER NOT NULL CHECK (raw_size > 0),
        compressed_size INTEGER NOT NULL CHECK (compressed_size > 0),
        object_path TEXT NOT NULL UNIQUE,
        created_at_ns INTEGER NOT NULL
    ))sql",
    R"sql(CREATE TABLE entry_chunks (
        entry_id INTEGER NOT NULL REFERENCES entries(id) ON DELETE CASCADE,
        sequence_number INTEGER NOT NULL CHECK (sequence_number >= 0),
        chunk_hash TEXT NOT NULL REFERENCES chunks(hash),
        raw_offset INTEGER NOT NULL CHECK (raw_offset >= 0),
        raw_length INTEGER NOT NULL CHECK (raw_length > 0),
        PRIMARY KEY(entry_id, sequence_number)
    ))sql",
    "CREATE INDEX idx_entry_chunks_hash ON entry_chunks(chunk_hash)",
    R"sql(CREATE TABLE snapshot_warnings (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        snapshot_id INTEGER NOT NULL REFERENCES snapshots(id) ON DELETE CASCADE,
        relative_path TEXT NOT NULL,
        warning_code TEXT NOT NULL,
        message TEXT NOT NULL
    ))sql",
    "CREATE INDEX idx_snapshot_warnings_snapshot ON snapshot_warnings(snapshot_id)",
    R"sql(CREATE TABLE repository_settings (
        key TEXT PRIMARY KEY,
        value TEXT NOT NULL,
        updated_at_ns INTEGER NOT NULL
    ))sql",
};

struct Migration {
    std::int64_t version;
    std::string_view description;
    void (*apply)(Database& database);
};

void apply_migration_001(Database& database) {
    for (const std::string_view sql : migration_001_sql) {
        database.execute(sql);
    }
}

constexpr std::array migrations{
    Migration{1, "Initial schema", apply_migration_001},
};

[[nodiscard]] std::int64_t utc_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] std::int64_t current_version(Database& database) {
    auto query = database.statement("SELECT COALESCE(MAX(version), 0) FROM schema_migrations");
    if (!query.step()) {
        throw LocalVaultError(ErrorCode::database_error,
                              "schema migration version query returned no row");
    }
    const std::int64_t version = query.column_int64(0);
    if (query.step()) {
        throw LocalVaultError(ErrorCode::database_error,
                              "schema migration version query returned multiple rows");
    }
    return version;
}

} // namespace

void run_migrations(Database& database) {
    Transaction transaction(database, TransactionMode::exclusive);
    database.execute(R"sql(CREATE TABLE IF NOT EXISTS schema_migrations (
        version INTEGER PRIMARY KEY,
        applied_at_ns INTEGER NOT NULL,
        description TEXT NOT NULL
    ))sql");

    const std::int64_t installed_version = current_version(database);
    for (const Migration& migration : migrations) {
        if (migration.version <= installed_version) {
            continue;
        }
        migration.apply(database);

        auto record = database.statement(
            "INSERT INTO schema_migrations (version, applied_at_ns, description) "
            "VALUES (:version, :applied_at_ns, :description)");
        record.bind(":version", migration.version);
        record.bind(":applied_at_ns", utc_now_ns());
        record.bind(":description", migration.description);
        record.execute();
    }
    transaction.commit();
}

} // namespace localvault
