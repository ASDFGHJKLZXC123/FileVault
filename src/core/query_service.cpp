#include "localvault/query_service.hpp"

#include <limits>
#include <string>
#include <utility>

#include "database/database.hpp"
#include "database/metadata_store.hpp"
#include "database/statement.hpp"
#include "database/transaction.hpp"
#include "localvault/error.hpp"
#include "localvault/repository.hpp"

namespace localvault {
namespace {

[[nodiscard]] std::uint64_t checked_unsigned(std::int64_t value) {
    if (value < 0) {
        throw LocalVaultError(ErrorCode::database_error, "negative metadata count or size");
    }
    return static_cast<std::uint64_t>(value);
}

[[nodiscard]] std::uint32_t checked_uint32(std::int64_t value) {
    const auto result = checked_unsigned(value);
    if (result > (std::numeric_limits<std::uint32_t>::max)()) {
        throw LocalVaultError(ErrorCode::database_error, "metadata value exceeds uint32 range");
    }
    return static_cast<std::uint32_t>(result);
}

void add_checked(std::uint64_t& total, std::int64_t value) {
    const auto increment = checked_unsigned(value);
    if (increment > (std::numeric_limits<std::uint64_t>::max)() - total) {
        throw LocalVaultError(ErrorCode::database_error, "repository statistics overflow");
    }
    total += increment;
}

void validate_page(PageRequest page) {
    constexpr auto maximum = static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
    if (page.limit == 0 || page.limit > 10'000 || page.offset > maximum - page.limit) {
        throw LocalVaultError(ErrorCode::invalid_argument, "page is outside the supported range");
    }
}

void bind_page(Statement& statement, PageRequest page) {
    statement.bind(":limit", static_cast<std::int64_t>(page.limit));
    statement.bind(":offset", static_cast<std::int64_t>(page.offset));
}

[[nodiscard]] std::uint64_t read_count(Statement& query) {
    if (!query.step()) {
        throw LocalVaultError(ErrorCode::database_error, "metadata count query returned no row");
    }
    const auto result = checked_unsigned(query.column_int64(0));
    if (query.step()) {
        throw LocalVaultError(ErrorCode::database_error,
                              "metadata count query returned extra rows");
    }
    return result;
}

[[nodiscard]] std::filesystem::path utf8_path(const std::string& value) {
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

[[nodiscard]] std::string normalized_parent(const std::filesystem::path& parent) {
    const auto encoded = parent.generic_u8string();
    const std::string text(encoded.begin(), encoded.end());
    const bool drive_prefix =
        text.size() >= 2 && text[1] == ':' &&
        ((text[0] >= 'A' && text[0] <= 'Z') || (text[0] >= 'a' && text[0] <= 'z'));
    if (parent.has_root_path() || drive_prefix || text.find('\0') != std::string::npos) {
        throw LocalVaultError(ErrorCode::invalid_argument, "parent path must be relative", parent);
    }
    for (const auto& component : parent) {
        if (component == "..") {
            throw LocalVaultError(ErrorCode::invalid_argument,
                                  "parent path must not contain traversal", parent);
        }
    }
    auto normalized = parent.lexically_normal();
    while (!normalized.empty() && normalized.filename().empty()) {
        normalized = normalized.parent_path();
    }
    if (normalized == ".") {
        return {};
    }
    const auto result = normalized.generic_u8string();
    return std::string(result.begin(), result.end());
}

[[nodiscard]] EntryInfo read_entry(Statement& query) {
    EntryInfo entry;
    entry.id = query.column_int64(0);
    entry.snapshot_id = query.column_int64(1);
    entry.relative_path = utf8_path(query.column_text(2));
    const auto type = query.column_text(3);
    if (type == "file") {
        entry.type = EntryType::regular_file;
    } else if (type == "directory") {
        entry.type = EntryType::directory;
    } else if (type == "symlink") {
        entry.type = EntryType::symbolic_link;
    } else {
        throw LocalVaultError(ErrorCode::database_error, "unknown metadata entry type");
    }
    entry.logical_size = checked_unsigned(query.column_int64(4));
    entry.modified_time_ns = query.column_int64(5);
    entry.posix_mode = checked_uint32(query.column_int64(6));
    if (!query.column_is_null(7)) {
        entry.windows_attributes = checked_uint32(query.column_int64(7));
    }
    if (!query.column_is_null(8)) {
        entry.file_hash_hex = query.column_text(8);
    }
    if (!query.column_is_null(9)) {
        entry.symlink_target = utf8_path(query.column_text(9));
    }
    return entry;
}

[[nodiscard]] Page<EntryInfo> entry_page(Database& database, SnapshotId id, std::string_view filter,
                                         std::string_view value, PageRequest page) {
    validate_page(page);
    Transaction transaction(database);
    (void)MetadataStore(database).require_complete_snapshot(id);
    const std::string selection = " FROM entries WHERE snapshot_id = :snapshot_id "
                                  "AND relative_path <> '' AND " +
                                  std::string(filter);
    auto count = database.statement("SELECT COUNT(*)" + selection);
    count.bind(":snapshot_id", id);
    count.bind(":value", value);
    Page<EntryInfo> result;
    result.total_count = read_count(count);
    auto entries = database.statement(
        "SELECT id, snapshot_id, relative_path, entry_type, logical_size, modified_time_ns, "
        "posix_mode, windows_attributes, file_hash, symlink_target" +
        selection + " ORDER BY relative_path COLLATE BINARY LIMIT :limit OFFSET :offset");
    entries.bind(":snapshot_id", id);
    entries.bind(":value", value);
    bind_page(entries, page);
    while (entries.step()) {
        result.items.push_back(read_entry(entries));
    }
    transaction.commit();
    return result;
}

[[nodiscard]] double savings(std::uint64_t numerator, std::uint64_t denominator) {
    return denominator == 0
               ? 0.0
               : 1.0 - static_cast<double>(numerator) / static_cast<double>(denominator);
}

} // namespace

QueryService::QueryService(Repository& repository) : repository_(repository) {}

Page<SnapshotSummary> QueryService::list_snapshots(PageRequest page) const {
    validate_page(page);
    auto& database = repository_.database();
    Transaction transaction(database);
    auto count = database.statement("SELECT COUNT(*) FROM snapshots WHERE status = 'complete'");
    Page<SnapshotSummary> result;
    result.total_count = read_count(count);
    auto query = database.statement(
        "SELECT id, created_at_ns, source_root, message, file_count, directory_count, "
        "logical_size, new_stored_size, duration_ms FROM snapshots WHERE status = 'complete' "
        "ORDER BY created_at_ns DESC, id DESC LIMIT :limit OFFSET :offset");
    bind_page(query, page);
    while (query.step()) {
        SnapshotSummary snapshot;
        snapshot.id = query.column_int64(0);
        snapshot.created_at = TimePoint(std::chrono::duration_cast<Clock::duration>(
            std::chrono::nanoseconds(query.column_int64(1))));
        snapshot.source_root = utf8_path(query.column_text(2));
        snapshot.message = query.column_text(3);
        snapshot.status = SnapshotStatus::complete;
        snapshot.file_count = checked_unsigned(query.column_int64(4));
        snapshot.directory_count = checked_unsigned(query.column_int64(5));
        snapshot.logical_size = checked_unsigned(query.column_int64(6));
        snapshot.new_stored_size = checked_unsigned(query.column_int64(7));
        snapshot.duration = std::chrono::milliseconds(query.column_int64(8));
        result.items.push_back(std::move(snapshot));
    }
    transaction.commit();
    return result;
}

SnapshotSummary QueryService::get_snapshot(SnapshotId id) const {
    auto& database = repository_.database();
    Transaction transaction(database);
    auto snapshot = MetadataStore(database).require_complete_snapshot(id);
    transaction.commit();
    return snapshot;
}

Page<EntryInfo> QueryService::list_children(SnapshotId id, const std::filesystem::path& parent,
                                            PageRequest page) const {
    return entry_page(repository_.database(), id, "parent_path = :value", normalized_parent(parent),
                      page);
}

Page<EntryInfo> QueryService::search_paths(SnapshotId id, std::string_view query,
                                           PageRequest page) const {
    if (query.find('\0') != std::string_view::npos) {
        throw LocalVaultError(ErrorCode::invalid_argument, "search text contains a null character");
    }
    return entry_page(repository_.database(), id, "instr(relative_path, :value) > 0", query, page);
}

RepositoryStats QueryService::repository_stats() const {
    auto& database = repository_.database();
    Transaction transaction(database);
    RepositoryStats result;
    auto count = database.statement("SELECT COUNT(*) FROM snapshots WHERE status = 'complete'");
    result.complete_snapshot_count = read_count(count);
    auto entries = database.statement(
        "SELECT e.logical_size FROM entries AS e JOIN snapshots AS s ON s.id = e.snapshot_id "
        "WHERE s.status = 'complete' AND e.entry_type = 'file'");
    while (entries.step()) {
        add_checked(result.logical_bytes, entries.column_int64(0));
    }
    auto chunks = database.statement(
        "SELECT raw_size, compressed_size FROM chunks WHERE hash IN ("
        "SELECT ec.chunk_hash FROM entry_chunks AS ec JOIN entries AS e ON e.id = ec.entry_id "
        "JOIN snapshots AS s ON s.id = e.snapshot_id WHERE s.status = 'complete')");
    while (chunks.step()) {
        add_checked(result.unique_chunk_count, 1);
        add_checked(result.unique_raw_bytes, chunks.column_int64(0));
        add_checked(result.stored_bytes, chunks.column_int64(1));
    }
    result.deduplication_savings = savings(result.unique_raw_bytes, result.logical_bytes);
    result.compression_savings = savings(result.stored_bytes, result.unique_raw_bytes);
    result.total_savings = savings(result.stored_bytes, result.logical_bytes);
    transaction.commit();
    return result;
}

} // namespace localvault
