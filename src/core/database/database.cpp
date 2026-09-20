#include "database/database.hpp"

#include <sqlite3.h>

#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

#include "database/locked_read_vfs.hpp"
#include "database/statement.hpp"
#include "filesystem/platform/path_safety.hpp"
#include "localvault/error.hpp"

namespace localvault {
namespace {

[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& path) {
    const std::u8string utf8 = path.generic_u8string();
    std::string result;
    result.reserve(utf8.size());
    for (const char8_t character : utf8) {
        result.push_back(static_cast<char>(character));
    }
    return result;
}

[[nodiscard]] bool is_uri_path_character(unsigned char character) {
    return (character >= static_cast<unsigned char>('a') &&
            character <= static_cast<unsigned char>('z')) ||
           (character >= static_cast<unsigned char>('A') &&
            character <= static_cast<unsigned char>('Z')) ||
           (character >= static_cast<unsigned char>('0') &&
            character <= static_cast<unsigned char>('9')) ||
           character == static_cast<unsigned char>('-') ||
           character == static_cast<unsigned char>('.') ||
           character == static_cast<unsigned char>('_') ||
           character == static_cast<unsigned char>('~') ||
           character == static_cast<unsigned char>('/') ||
           character == static_cast<unsigned char>(':');
}

[[nodiscard]] std::string percent_encode_uri_path(std::string_view path) {
    constexpr std::string_view hexadecimal = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(path.size());
    for (const char character : path) {
        const auto byte = static_cast<unsigned char>(character);
        if (is_uri_path_character(byte)) {
            encoded.push_back(character);
            continue;
        }
        encoded.push_back('%');
        encoded.push_back(hexadecimal[byte >> 4U]);
        encoded.push_back(hexadecimal[byte & 0x0FU]);
    }
    return encoded;
}

[[nodiscard]] std::string immutable_database_uri(const std::filesystem::path& path) {
    const std::string utf8_path = path_to_utf8(path);
    std::string uri = "file:";
#ifdef _WIN32
    if (path.is_absolute() && utf8_path.size() >= 3U && utf8_path[1] == ':' &&
        utf8_path[2] == '/') {
        uri += "///";
    }
#endif
    uri += percent_encode_uri_path(utf8_path);
    uri += "?immutable=1";
    return uri;
}

void enable_persistent_wal(sqlite3* database) {
    int enabled = 1;
    const int set_result =
        sqlite3_file_control(database, "main", SQLITE_FCNTL_PERSIST_WAL, &enabled);
    if (set_result != SQLITE_OK) {
        throw LocalVaultError(ErrorCode::database_error,
                              "enabling persistent SQLite WAL files failed (SQLite result " +
                                  std::to_string(set_result) + "): " + sqlite3_errmsg(database));
    }

    enabled = -1;
    const int query_result =
        sqlite3_file_control(database, "main", SQLITE_FCNTL_PERSIST_WAL, &enabled);
    if (query_result != SQLITE_OK) {
        throw LocalVaultError(ErrorCode::database_error,
                              "verifying persistent SQLite WAL files failed (SQLite result " +
                                  std::to_string(query_result) + "): " + sqlite3_errmsg(database));
    }
    if (enabled != 1) {
        throw LocalVaultError(ErrorCode::database_error,
                              "persistent SQLite WAL files could not be enabled");
    }
}

void run_pragma(sqlite3* database, std::string_view sql) {
    Statement pragma(database, sql);
    while (pragma.step()) {
    }
}

[[nodiscard]] std::int64_t integer_pragma(sqlite3* database, std::string_view sql) {
    Statement pragma(database, sql);
    if (!pragma.step()) {
        throw LocalVaultError(ErrorCode::database_error,
                              "SQLite pragma returned no value; SQL: " + std::string(sql));
    }
    const std::int64_t value = pragma.column_int64(0);
    if (pragma.step()) {
        throw LocalVaultError(ErrorCode::database_error,
                              "SQLite pragma returned multiple values; SQL: " + std::string(sql));
    }
    return value;
}

[[nodiscard]] std::string text_pragma(sqlite3* database, std::string_view sql) {
    Statement pragma(database, sql);
    if (!pragma.step()) {
        throw LocalVaultError(ErrorCode::database_error,
                              "SQLite pragma returned no value; SQL: " + std::string(sql));
    }
    std::string value = pragma.column_text(0);
    if (pragma.step()) {
        throw LocalVaultError(ErrorCode::database_error,
                              "SQLite pragma returned multiple values; SQL: " + std::string(sql));
    }
    return value;
}

} // namespace

Database::Database(const std::filesystem::path& path, bool read_only)
    : Database(path, read_only ? DatabaseAccess::read_only : DatabaseAccess::read_write) {}

Database::Database(const std::filesystem::path& path, DatabaseAccess access) {
    const bool immutable = access == DatabaseAccess::immutable_read_only;
    const bool writable = access == DatabaseAccess::read_write;
    const bool locked_read_only = access == DatabaseAccess::locked_read_only;
    const char* vfs = nullptr;
    if (locked_read_only) {
        auto wal_path = path;
        wal_path += "-wal";
        for (const auto& candidate : {path, wal_path}) {
            std::error_code error;
            const auto status = std::filesystem::symlink_status(candidate, error);
            if (error || !std::filesystem::is_regular_file(status) ||
                inspect_path_no_follow(candidate) != NoFollowPathType::other) {
                throw LocalVaultError(ErrorCode::database_error,
                                      "locked read-only view requires regular database and WAL files",
                                      candidate);
            }
        }
        // SQLite may delete an existing WAL when opening an empty main database,
        // even for a read-only connection. Reject it before entering the pager.
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error || size < 100) {
            throw LocalVaultError(ErrorCode::database_error,
                                  "locked read-only view requires a complete database header", path);
        }
        vfs = locked_read_vfs_name();
    }
    const int flags =
        writable ? SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX
                 : SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX | (immutable ? SQLITE_OPEN_URI : 0);
    const std::string sqlite_path = immutable ? immutable_database_uri(path) : path_to_utf8(path);
    const int result = sqlite3_open_v2(sqlite_path.c_str(), &database_, flags, vfs);
    if (result != SQLITE_OK) {
        const std::string detail =
            database_ == nullptr ? "unknown SQLite error" : sqlite3_errmsg(database_);
        if (database_ != nullptr) {
            const int close_result = sqlite3_close_v2(database_);
            if (close_result != SQLITE_OK) {
                std::fprintf(
                    stderr,
                    "LocalVault: closing failed database handle returned SQLite result %d\n",
                    close_result);
            }
            database_ = nullptr;
        }
        std::string message = "opening SQLite database failed (SQLite result ";
        message += std::to_string(result);
        message += "): ";
        message += detail;
        throw LocalVaultError(ErrorCode::database_error, std::move(message), path);
    }

    try {
        initialize_connection(access);
    } catch (...) {
        close();
        throw;
    }
}

Database::~Database() noexcept {
    close();
}

Database::Database(Database&& other) noexcept
    : database_(std::exchange(other.database_, nullptr)) {}

Database& Database::operator=(Database&& other) noexcept {
    if (this != &other) {
        close();
        database_ = std::exchange(other.database_, nullptr);
    }
    return *this;
}

Statement Database::statement(std::string_view sql) const {
    return Statement(database_, sql);
}

void Database::execute(std::string_view sql) const {
    Statement prepared(database_, sql);
    prepared.execute();
}

void Database::initialize_connection(DatabaseAccess access) {
    if (access == DatabaseAccess::locked_read_only) {
        int enabled = 0;
        if (sqlite3_db_config(database_, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, &enabled) !=
                SQLITE_OK ||
            enabled != 1) {
            throw LocalVaultError(ErrorCode::database_error,
                                  "could not disable checkpointing for a locked read-only view");
        }
        // The locked-read VFS plus EXCLUSIVE before the first read gives SQLite a private
        // heap WAL index. URI nolock=1 would instead disable WAL support entirely.
        if (text_pragma(database_, "PRAGMA locking_mode = EXCLUSIVE") != "exclusive") {
            throw LocalVaultError(ErrorCode::database_error,
                                  "could not enable private WAL indexing for read-only view");
        }
    }
    run_pragma(database_, "PRAGMA foreign_keys = ON");
    if (access == DatabaseAccess::read_write) {
        run_pragma(database_, "PRAGMA journal_mode = WAL");
        enable_persistent_wal(database_);
    }
    run_pragma(database_, "PRAGMA synchronous = FULL");
    run_pragma(database_, "PRAGMA busy_timeout = 5000");

    if (integer_pragma(database_, "PRAGMA foreign_keys") != 1) {
        throw LocalVaultError(ErrorCode::database_error,
                              "SQLite foreign keys could not be enabled");
    }
    if (access != DatabaseAccess::immutable_read_only &&
        text_pragma(database_, "PRAGMA journal_mode") != "wal") {
        throw LocalVaultError(ErrorCode::database_error, "SQLite WAL journal mode is required");
    }
    if (integer_pragma(database_, "PRAGMA synchronous") != 2) {
        throw LocalVaultError(ErrorCode::database_error,
                              "SQLite FULL synchronous mode is required");
    }
    if (integer_pragma(database_, "PRAGMA busy_timeout") != 5000) {
        throw LocalVaultError(ErrorCode::database_error, "SQLite busy timeout could not be set");
    }
}

void Database::close() noexcept {
    if (database_ == nullptr) {
        return;
    }
    const int result = sqlite3_close_v2(database_);
    if (result != SQLITE_OK) {
        std::fprintf(stderr, "LocalVault: closing database failed (SQLite result %d): %s\n", result,
                     sqlite3_errmsg(database_));
    }
    database_ = nullptr;
}

} // namespace localvault
