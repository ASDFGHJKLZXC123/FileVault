#include "database/statement.hpp"

#include <sqlite3.h>

#include <cctype>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>

#include "localvault/error.hpp"

namespace localvault {
namespace {

[[noreturn]] void throw_sqlite_error(sqlite3* database, std::string_view action,
                                     std::string_view sql, int result) {
    std::string message(action);
    message += " failed (SQLite result ";
    message += std::to_string(result);
    message += "): ";
    message += database == nullptr ? "unknown SQLite error" : sqlite3_errmsg(database);
    if (!sql.empty()) {
        message += "; SQL: ";
        message += sql;
    }
    throw LocalVaultError(ErrorCode::database_error, std::move(message));
}

void report_destructor_error(sqlite3* database, std::string_view action, int result) noexcept {
    if (result == SQLITE_OK) {
        return;
    }
    const char* detail = database == nullptr ? "unknown SQLite error" : sqlite3_errmsg(database);
    std::fprintf(stderr, "LocalVault: %.*s failed during cleanup (SQLite result %d): %s\n",
                 static_cast<int>(action.size()), action.data(), result, detail);
}

} // namespace

Statement::Statement(sqlite3* database, std::string_view sql) : database_(database), sql_(sql) {
    if (database_ == nullptr) {
        throw LocalVaultError(ErrorCode::database_error,
                              "cannot prepare a statement without a database");
    }
    if (sql_.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw LocalVaultError(ErrorCode::database_error, "SQLite statement is too large");
    }

    const char* tail = nullptr;
    const int result = sqlite3_prepare_v3(database_, sql_.data(), static_cast<int>(sql_.size()),
                                          SQLITE_PREPARE_PERSISTENT, &statement_, &tail);
    if (result != SQLITE_OK) {
        throw_sqlite_error(database_, "preparing statement", sql_, result);
    }
    if (statement_ == nullptr) {
        throw LocalVaultError(ErrorCode::database_error, "SQLite statement is empty");
    }
    while (tail != sql_.data() + sql_.size() &&
           std::isspace(static_cast<unsigned char>(*tail)) != 0) {
        ++tail;
    }
    if (tail != sql_.data() + sql_.size()) {
        finalize();
        throw LocalVaultError(ErrorCode::database_error,
                              "multiple SQL statements are not allowed: " + sql_);
    }
}

Statement::~Statement() noexcept {
    finalize();
}

Statement::Statement(Statement&& other) noexcept
    : database_(std::exchange(other.database_, nullptr)),
      statement_(std::exchange(other.statement_, nullptr)), sql_(std::move(other.sql_)),
      last_step_result_(std::exchange(other.last_step_result_, SQLITE_OK)) {}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        finalize();
        database_ = std::exchange(other.database_, nullptr);
        statement_ = std::exchange(other.statement_, nullptr);
        sql_ = std::move(other.sql_);
        last_step_result_ = std::exchange(other.last_step_result_, SQLITE_OK);
    }
    return *this;
}

void Statement::bind(std::string_view name, std::int64_t value) {
    const int result = sqlite3_bind_int64(statement_, parameter_index(name), value);
    if (result != SQLITE_OK) {
        throw_sqlite_error(database_, "binding integer", sql_, result);
    }
}

void Statement::bind(std::string_view name, std::string_view value) {
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw LocalVaultError(ErrorCode::database_error, "SQLite text value is too large");
    }
    const int result = sqlite3_bind_text(statement_, parameter_index(name), value.data(),
                                         static_cast<int>(value.size()), SQLITE_TRANSIENT);
    if (result != SQLITE_OK) {
        throw_sqlite_error(database_, "binding text", sql_, result);
    }
}

void Statement::bind_null(std::string_view name) {
    const int result = sqlite3_bind_null(statement_, parameter_index(name));
    if (result != SQLITE_OK) {
        throw_sqlite_error(database_, "binding null", sql_, result);
    }
}

bool Statement::step() {
    const int result = sqlite3_step(statement_);
    last_step_result_ = result;
    if (result == SQLITE_ROW) {
        return true;
    }
    if (result == SQLITE_DONE) {
        return false;
    }
    throw_sqlite_error(database_, "stepping statement", sql_, result);
}

void Statement::execute() {
    if (step()) {
        throw LocalVaultError(ErrorCode::database_error,
                              "statement unexpectedly returned rows; SQL: " + sql_);
    }
}

void Statement::reset() {
    const int reset_result = sqlite3_reset(statement_);
    if (reset_result != SQLITE_OK) {
        throw_sqlite_error(database_, "resetting statement", sql_, reset_result);
    }
    const int clear_result = sqlite3_clear_bindings(statement_);
    if (clear_result != SQLITE_OK) {
        throw_sqlite_error(database_, "clearing statement bindings", sql_, clear_result);
    }
    last_step_result_ = SQLITE_OK;
}

std::int64_t Statement::column_int64(int index) const {
    check_column_index(index);
    return sqlite3_column_int64(statement_, index);
}

std::string Statement::column_text(int index) const {
    check_column_index(index);
    const auto* text = sqlite3_column_text(statement_, index);
    if (text == nullptr) {
        if (sqlite3_column_type(statement_, index) == SQLITE_NULL) {
            throw LocalVaultError(ErrorCode::database_error,
                                  "cannot read a NULL SQLite column as text");
        }
        throw_sqlite_error(database_, "reading text column", sql_, sqlite3_errcode(database_));
    }
    const int byte_count = sqlite3_column_bytes(statement_, index);
    return {reinterpret_cast<const char*>(text), static_cast<std::size_t>(byte_count)};
}

bool Statement::column_is_null(int index) const {
    check_column_index(index);
    return sqlite3_column_type(statement_, index) == SQLITE_NULL;
}

int Statement::parameter_index(std::string_view name) const {
    const std::string terminated_name(name);
    const int index = sqlite3_bind_parameter_index(statement_, terminated_name.c_str());
    if (index == 0) {
        throw LocalVaultError(ErrorCode::database_error,
                              "unknown SQLite parameter '" + terminated_name + "'; SQL: " + sql_);
    }
    return index;
}

void Statement::check_column_index(int index) const {
    const int count = sqlite3_column_count(statement_);
    if (index < 0 || index >= count) {
        throw LocalVaultError(ErrorCode::database_error,
                              "SQLite column index " + std::to_string(index) + " is out of range");
    }
}

void Statement::finalize() noexcept {
    if (statement_ == nullptr) {
        return;
    }
    const int result = sqlite3_finalize(statement_);
    statement_ = nullptr;
    if (result != last_step_result_) {
        report_destructor_error(database_, "finalizing statement", result);
    }
    last_step_result_ = SQLITE_OK;
}

} // namespace localvault
