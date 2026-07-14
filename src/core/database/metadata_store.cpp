#include "database/metadata_store.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "database/database.hpp"
#include "database/statement.hpp"
#include "database/transaction.hpp"
#include "filesystem/file_scanner.hpp"
#include "localvault/error.hpp"
#include "localvault/failure_injector.hpp"

namespace localvault {
namespace {

[[nodiscard]] std::int64_t checked_int64(ByteCount value, std::string_view field) {
    constexpr auto maximum = static_cast<ByteCount>((std::numeric_limits<std::int64_t>::max)());
    if (value > maximum) {
        throw LocalVaultError(ErrorCode::database_error,
                              std::string(field) + " is outside SQLite's integer range");
    }
    return static_cast<std::int64_t>(value);
}

[[nodiscard]] std::uint32_t checked_uint32(std::int64_t value, std::string_view field) {
    if (value < 0 || static_cast<std::uint64_t>(value) >
                         static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())) {
        throw LocalVaultError(ErrorCode::database_error,
                              std::string(field) + " is outside the supported range");
    }
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] ByteCount checked_byte_count(std::int64_t value, std::string_view field) {
    if (value < 0) {
        throw LocalVaultError(ErrorCode::database_error,
                              std::string(field) + " is outside the supported range");
    }
    return static_cast<ByteCount>(value);
}

[[nodiscard]] std::uint64_t checked_uint64(std::int64_t value, std::string_view field) {
    if (value < 0) {
        throw LocalVaultError(ErrorCode::database_error,
                              std::string(field) + " is outside the supported range");
    }
    return static_cast<std::uint64_t>(value);
}

[[nodiscard]] bool is_lowercase_blake3_hex(std::string_view value) noexcept {
    if (value.size() != 64U) {
        return false;
    }
    return std::ranges::all_of(value, [](char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

[[nodiscard]] std::string_view stored_entry_type(EntryType type) {
    switch (type) {
    case EntryType::regular_file:
        return "file";
    case EntryType::directory:
        return "directory";
    case EntryType::symbolic_link:
        return "symlink";
    }
    throw LocalVaultError(ErrorCode::internal_error, "unknown entry type");
}

[[nodiscard]] EntryType entry_type_from_storage(std::string_view type) {
    if (type == "file") {
        return EntryType::regular_file;
    }
    if (type == "directory") {
        return EntryType::directory;
    }
    if (type == "symlink") {
        return EntryType::symbolic_link;
    }
    throw LocalVaultError(ErrorCode::database_error, "database contains an unknown entry type");
}

[[nodiscard]] std::filesystem::path utf8_path(std::string_view text) {
    std::u8string encoded;
    encoded.reserve(text.size());
    for (const char character : text) {
        encoded.push_back(static_cast<char8_t>(static_cast<unsigned char>(character)));
    }
    return std::filesystem::path(encoded);
}

[[nodiscard]] EntryInfo read_entry(Statement& query) {
    EntryInfo entry;
    entry.id = query.column_int64(0);
    entry.snapshot_id = query.column_int64(1);
    entry.relative_path = utf8_path(query.column_text(2));
    entry.type = entry_type_from_storage(query.column_text(3));
    entry.logical_size = checked_byte_count(query.column_int64(4), "entry logical size");
    entry.modified_time_ns = query.column_int64(5);
    entry.posix_mode = checked_uint32(query.column_int64(6), "entry POSIX mode");
    if (!query.column_is_null(7)) {
        entry.windows_attributes =
            checked_uint32(query.column_int64(7), "entry Windows attributes");
    }
    if (!query.column_is_null(8)) {
        entry.file_hash_hex = query.column_text(8);
    }
    if (!query.column_is_null(9)) {
        entry.symlink_target = utf8_path(query.column_text(9));
    }
    return entry;
}

[[nodiscard]] SnapshotSummary read_snapshot(Statement& query) {
    SnapshotSummary snapshot;
    snapshot.id = query.column_int64(0);
    snapshot.created_at = TimePoint(std::chrono::duration_cast<Clock::duration>(
        std::chrono::nanoseconds(query.column_int64(1))));
    snapshot.source_root = utf8_path(query.column_text(2));
    snapshot.message = query.column_text(3);
    snapshot.status = SnapshotStatus::complete;
    snapshot.file_count = checked_uint64(query.column_int64(4), "snapshot file count");
    snapshot.directory_count = checked_uint64(query.column_int64(5), "snapshot directory count");
    snapshot.logical_size = checked_byte_count(query.column_int64(6), "snapshot logical size");
    snapshot.new_stored_size = checked_byte_count(query.column_int64(7), "snapshot stored size");
    snapshot.duration = std::chrono::milliseconds(query.column_int64(8));
    return snapshot;
}

[[nodiscard]] SnapshotStatus snapshot_status_from_storage(std::string_view status) {
    if (status == "pending") {
        return SnapshotStatus::pending;
    }
    if (status == "failed") {
        return SnapshotStatus::failed;
    }
    if (status == "cancelled") {
        return SnapshotStatus::cancelled;
    }
    if (status == "deleting") {
        return SnapshotStatus::deleting;
    }
    throw LocalVaultError(ErrorCode::database_error,
                          "database contains an unexpected incomplete snapshot status");
}

void require_single_updated_row(Statement& statement, std::string_view action) {
    if (!statement.step()) {
        throw LocalVaultError(ErrorCode::invalid_argument,
                              std::string(action) + " requires a pending snapshot");
    }
    (void)statement.column_int64(0);
    if (statement.step()) {
        throw LocalVaultError(ErrorCode::database_error,
                              std::string(action) + " updated multiple snapshots");
    }
}

[[nodiscard]] std::vector<EntryInfo> read_entries(Statement query) {
    std::vector<EntryInfo> entries;
    while (query.step()) {
        entries.push_back(read_entry(query));
    }
    return entries;
}

[[nodiscard]] std::int64_t checked_batch_limit(std::size_t entry_batch_limit) {
    if (entry_batch_limit == 0 ||
        entry_batch_limit > static_cast<std::size_t>((std::numeric_limits<std::int64_t>::max)())) {
        throw LocalVaultError(ErrorCode::invalid_argument,
                              "snapshot entry batch limit is outside the supported range");
    }
    return static_cast<std::int64_t>(entry_batch_limit);
}

[[nodiscard]] std::size_t delete_entry_batch(Database& database, SnapshotId snapshot_id,
                                             std::int64_t entry_batch_limit) {
    auto remove = database.statement(
        "DELETE FROM entries WHERE id IN (SELECT id FROM entries "
        "WHERE snapshot_id = :snapshot_id ORDER BY id LIMIT :entry_batch_limit) RETURNING id");
    remove.bind(":snapshot_id", snapshot_id);
    remove.bind(":entry_batch_limit", entry_batch_limit);
    std::size_t removed = 0;
    while (remove.step()) {
        (void)remove.column_int64(0);
        ++removed;
    }
    return removed;
}

[[nodiscard]] std::size_t delete_warning_batch(Database& database, SnapshotId snapshot_id,
                                               std::int64_t warning_batch_limit) {
    auto remove = database.statement(
        "DELETE FROM snapshot_warnings WHERE id IN (SELECT id FROM snapshot_warnings "
        "WHERE snapshot_id = :snapshot_id ORDER BY id LIMIT :warning_batch_limit) RETURNING id");
    remove.bind(":snapshot_id", snapshot_id);
    remove.bind(":warning_batch_limit", warning_batch_limit);
    std::size_t removed = 0;
    while (remove.step()) {
        (void)remove.column_int64(0);
        ++removed;
    }
    return removed;
}

constexpr std::string_view entry_projection =
    "id, snapshot_id, relative_path, entry_type, logical_size, modified_time_ns, posix_mode, "
    "windows_attributes, file_hash, symlink_target";

} // namespace

SnapshotId MetadataStore::create_pending_snapshot(std::string_view source_root,
                                                  std::string_view message,
                                                  std::int64_t created_at_ns) const {
    auto insert = database_.statement(
        "INSERT INTO snapshots (created_at_ns, source_root, message, status) "
        "VALUES (:created_at_ns, :source_root, :message, 'pending') RETURNING id");
    insert.bind(":created_at_ns", created_at_ns);
    insert.bind(":source_root", source_root);
    insert.bind(":message", message);
    if (!insert.step()) {
        throw LocalVaultError(ErrorCode::database_error,
                              "snapshot insertion returned no database identifier");
    }
    const SnapshotId snapshot_id = insert.column_int64(0);
    if (insert.step()) {
        throw LocalVaultError(ErrorCode::database_error,
                              "snapshot insertion returned multiple database identifiers");
    }
    return snapshot_id;
}

void MetadataStore::mark_snapshot_complete(SnapshotId snapshot_id, const SnapshotTotals& totals,
                                           std::int64_t completed_at_ns) const {
    auto count_entries = database_.statement(
        "SELECT COALESCE(SUM(CASE WHEN entry_type = 'file' THEN 1 ELSE 0 END), 0), "
        "COALESCE(SUM(CASE WHEN entry_type = 'directory' THEN 1 ELSE 0 END), 0), "
        "COALESCE(SUM(CASE WHEN entry_type = 'symlink' THEN 1 ELSE 0 END), 0), "
        "COALESCE(SUM(CASE WHEN entry_type = 'file' THEN logical_size ELSE 0 END), 0) "
        "FROM entries WHERE snapshot_id = :snapshot_id");
    count_entries.bind(":snapshot_id", snapshot_id);
    if (!count_entries.step()) {
        throw LocalVaultError(ErrorCode::database_error, "snapshot counter query returned no row");
    }
    SnapshotTotals final_totals = totals;
    final_totals.file_count = checked_uint64(count_entries.column_int64(0), "snapshot file count");
    final_totals.directory_count =
        checked_uint64(count_entries.column_int64(1), "snapshot directory count");
    final_totals.symlink_count =
        checked_uint64(count_entries.column_int64(2), "snapshot symlink count");
    final_totals.logical_size =
        checked_byte_count(count_entries.column_int64(3), "snapshot logical size");
    if (count_entries.step()) {
        throw LocalVaultError(ErrorCode::database_error,
                              "snapshot counter query returned multiple rows");
    }

    auto update = database_.statement(
        "UPDATE snapshots SET completed_at_ns = :completed_at_ns, status = 'complete', "
        "file_count = :file_count, directory_count = :directory_count, "
        "symlink_count = :symlink_count, logical_size = :logical_size, "
        "new_stored_size = :new_stored_size, new_chunk_count = :new_chunk_count, "
        "reused_chunk_count = :reused_chunk_count, duration_ms = :duration_ms "
        "WHERE id = :snapshot_id AND status = 'pending' AND NOT EXISTS "
        "(SELECT 1 FROM entries WHERE snapshot_id = :snapshot_id "
        "AND entry_type = 'file' AND file_hash IS NULL) RETURNING id");
    update.bind(":completed_at_ns", completed_at_ns);
    update.bind(":file_count", checked_int64(final_totals.file_count, "snapshot file count"));
    update.bind(":directory_count",
                checked_int64(final_totals.directory_count, "snapshot directory count"));
    update.bind(":symlink_count",
                checked_int64(final_totals.symlink_count, "snapshot symlink count"));
    update.bind(":logical_size", checked_int64(final_totals.logical_size, "snapshot logical size"));
    update.bind(":new_stored_size",
                checked_int64(final_totals.new_stored_size, "snapshot stored size"));
    update.bind(":new_chunk_count",
                checked_int64(final_totals.new_chunk_count, "snapshot new chunk count"));
    update.bind(":reused_chunk_count",
                checked_int64(final_totals.reused_chunk_count, "snapshot reused chunk count"));
    update.bind(":duration_ms", final_totals.duration_ms);
    update.bind(":snapshot_id", snapshot_id);
    require_single_updated_row(update, "completing a snapshot");
}

void MetadataStore::mark_snapshot_incomplete(SnapshotId snapshot_id, SnapshotStatus status,
                                             std::string_view failure_message,
                                             std::int64_t completed_at_ns) const {
    if (status != SnapshotStatus::failed && status != SnapshotStatus::cancelled) {
        throw LocalVaultError(ErrorCode::invalid_argument,
                              "an incomplete snapshot must be failed or cancelled");
    }

    auto update = database_.statement(
        "UPDATE snapshots SET completed_at_ns = :completed_at_ns, status = :status, "
        "failure_message = :failure_message "
        "WHERE id = :snapshot_id AND status = 'pending' RETURNING id");
    update.bind(":completed_at_ns", completed_at_ns);
    update.bind(":status", status == SnapshotStatus::cancelled ? "cancelled" : "failed");
    update.bind(":failure_message", failure_message);
    update.bind(":snapshot_id", snapshot_id);
    require_single_updated_row(update, "finishing an incomplete snapshot");
}

SnapshotSummary MetadataStore::require_complete_snapshot(SnapshotId snapshot_id) const {
    auto query = database_.statement(
        "SELECT id, created_at_ns, source_root, message, file_count, directory_count, "
        "logical_size, new_stored_size, duration_ms FROM snapshots "
        "WHERE id = :snapshot_id AND status = 'complete'");
    query.bind(":snapshot_id", snapshot_id);
    if (!query.step()) {
        throw LocalVaultError(ErrorCode::invalid_argument,
                              "snapshot does not exist or is not complete");
    }
    SnapshotSummary snapshot = read_snapshot(query);
    if (query.step()) {
        throw LocalVaultError(ErrorCode::database_error,
                              "snapshot identifier selected multiple rows");
    }
    return snapshot;
}

std::vector<SnapshotSummary> MetadataStore::list_complete_snapshots() const {
    auto query = database_.statement(
        "SELECT id, created_at_ns, source_root, message, file_count, directory_count, "
        "logical_size, new_stored_size, duration_ms FROM snapshots "
        "WHERE status = 'complete' ORDER BY created_at_ns DESC, id DESC");
    std::vector<SnapshotSummary> snapshots;
    while (query.step()) {
        snapshots.push_back(read_snapshot(query));
    }
    return snapshots;
}

void MetadataStore::transition_snapshot_to_deleting(SnapshotId snapshot_id,
                                                    FailureInjector& failure_injector) const {
    Transaction transaction(database_);
    auto update =
        database_.statement("UPDATE snapshots SET status = 'deleting' "
                            "WHERE id = :snapshot_id AND status = 'complete' RETURNING id");
    update.bind(":snapshot_id", snapshot_id);
    if (!update.step()) {
        throw LocalVaultError(ErrorCode::invalid_argument,
                              "deleting requires an existing complete snapshot");
    }
    (void)update.column_int64(0);
    if (update.step()) {
        throw LocalVaultError(ErrorCode::database_error, "deleting updated multiple snapshots");
    }
    failure_injector.hit(FailurePoint::before_metadata_batch_commit);
    transaction.commit();
}

void MetadataStore::delete_deleting_snapshot(SnapshotId snapshot_id,
                                             FailureInjector& failure_injector,
                                             std::size_t entry_batch_limit) const {
    const std::int64_t batch_limit = checked_batch_limit(entry_batch_limit);
    for (;;) {
        Transaction transaction(database_);
        auto status = database_.statement(
            "SELECT 1 FROM snapshots WHERE id = :snapshot_id AND status = 'deleting'");
        status.bind(":snapshot_id", snapshot_id);
        if (!status.step()) {
            throw LocalVaultError(ErrorCode::invalid_argument,
                                  "resuming deletion requires a deleting snapshot");
        }
        if (status.step()) {
            throw LocalVaultError(ErrorCode::database_error,
                                  "deleting snapshot identifier selected multiple rows");
        }

        const std::size_t removed = delete_entry_batch(database_, snapshot_id, batch_limit);
        failure_injector.hit(FailurePoint::before_metadata_batch_commit);
        transaction.commit();
        if (removed == 0) {
            break;
        }
    }

    for (;;) {
        Transaction transaction(database_);
        auto status = database_.statement(
            "SELECT 1 FROM snapshots WHERE id = :snapshot_id AND status = 'deleting'");
        status.bind(":snapshot_id", snapshot_id);
        if (!status.step()) {
            throw LocalVaultError(ErrorCode::invalid_argument,
                                  "resuming deletion requires a deleting snapshot");
        }
        if (status.step()) {
            throw LocalVaultError(ErrorCode::database_error,
                                  "deleting snapshot identifier selected multiple rows");
        }

        const std::size_t removed = delete_warning_batch(database_, snapshot_id, batch_limit);
        failure_injector.hit(FailurePoint::before_metadata_batch_commit);
        transaction.commit();
        if (removed == 0) {
            break;
        }
    }

    Transaction final_transaction(database_);
    auto remove = database_.statement(
        "DELETE FROM snapshots WHERE id = :snapshot_id AND status = 'deleting' RETURNING id");
    remove.bind(":snapshot_id", snapshot_id);
    if (!remove.step()) {
        throw LocalVaultError(ErrorCode::invalid_argument,
                              "finishing deletion requires a deleting snapshot");
    }
    (void)remove.column_int64(0);
    if (remove.step()) {
        throw LocalVaultError(ErrorCode::database_error,
                              "finishing deletion removed multiple snapshots");
    }
    failure_injector.hit(FailurePoint::before_metadata_batch_commit);
    final_transaction.commit();
}

std::vector<IncompleteSnapshotInfo> MetadataStore::list_incomplete_snapshots() const {
    auto query = database_.statement(
        "SELECT id, status FROM snapshots WHERE status <> 'complete' ORDER BY id");
    std::vector<IncompleteSnapshotInfo> snapshots;
    while (query.step()) {
        snapshots.push_back(
            {query.column_int64(0), snapshot_status_from_storage(query.column_text(1))});
    }
    return snapshots;
}

void MetadataStore::mark_stale_pending_snapshot_failed(SnapshotId snapshot_id,
                                                       std::string_view failure_message,
                                                       std::int64_t completed_at_ns,
                                                       FailureInjector& failure_injector) const {
    Transaction transaction(database_);
    auto update = database_.statement(
        "UPDATE snapshots SET completed_at_ns = :completed_at_ns, status = 'failed', "
        "failure_message = :failure_message "
        "WHERE id = :snapshot_id AND status = 'pending' RETURNING id");
    update.bind(":completed_at_ns", completed_at_ns);
    update.bind(":failure_message", failure_message);
    update.bind(":snapshot_id", snapshot_id);
    if (!update.step()) {
        throw LocalVaultError(ErrorCode::invalid_argument,
                              "recovery requires a stale pending snapshot");
    }
    (void)update.column_int64(0);
    if (update.step()) {
        throw LocalVaultError(ErrorCode::database_error,
                              "recovery failed multiple pending snapshots");
    }
    failure_injector.hit(FailurePoint::before_metadata_batch_commit);
    transaction.commit();
}

void MetadataStore::clean_incomplete_snapshot(SnapshotId snapshot_id,
                                              FailureInjector& failure_injector,
                                              std::size_t entry_batch_limit) const {
    const std::int64_t batch_limit = checked_batch_limit(entry_batch_limit);
    for (;;) {
        Transaction transaction(database_);
        auto status = database_.statement("SELECT 1 FROM snapshots WHERE id = :snapshot_id "
                                          "AND status IN ('failed', 'cancelled')");
        status.bind(":snapshot_id", snapshot_id);
        if (!status.step()) {
            throw LocalVaultError(ErrorCode::invalid_argument,
                                  "cleanup requires a failed or cancelled snapshot");
        }
        if (status.step()) {
            throw LocalVaultError(ErrorCode::database_error,
                                  "incomplete snapshot identifier selected multiple rows");
        }

        const std::size_t removed = delete_entry_batch(database_, snapshot_id, batch_limit);
        failure_injector.hit(FailurePoint::before_metadata_batch_commit);
        transaction.commit();
        if (removed == 0) {
            break;
        }
    }

    for (;;) {
        Transaction transaction(database_);
        auto status = database_.statement("SELECT 1 FROM snapshots WHERE id = :snapshot_id "
                                          "AND status IN ('failed', 'cancelled')");
        status.bind(":snapshot_id", snapshot_id);
        if (!status.step()) {
            throw LocalVaultError(ErrorCode::invalid_argument,
                                  "cleanup requires a failed or cancelled snapshot");
        }
        if (status.step()) {
            throw LocalVaultError(ErrorCode::database_error,
                                  "incomplete snapshot identifier selected multiple rows");
        }

        const std::size_t removed = delete_warning_batch(database_, snapshot_id, batch_limit);
        failure_injector.hit(FailurePoint::before_metadata_batch_commit);
        transaction.commit();
        if (removed == 0) {
            break;
        }
    }
}

void MetadataStore::quick_relationship_check() const {
    auto foreign_keys = database_.statement("PRAGMA foreign_key_check");
    if (foreign_keys.step()) {
        throw LocalVaultError(ErrorCode::database_error,
                              "repository metadata contains a foreign-key violation");
    }

    auto invalid = database_.statement(
        "SELECT EXISTS ("
        "SELECT 1 FROM snapshots WHERE status IN ('pending', 'deleting') "
        "UNION ALL "
        "SELECT 1 FROM entries AS e JOIN snapshots AS s ON s.id = e.snapshot_id "
        "WHERE s.status IN ('failed', 'cancelled') "
        "UNION ALL "
        "SELECT 1 FROM snapshot_warnings AS w JOIN snapshots AS s ON s.id = w.snapshot_id "
        "WHERE s.status IN ('failed', 'cancelled') "
        "UNION ALL "
        "SELECT 1 FROM entries AS e JOIN snapshots AS s ON s.id = e.snapshot_id "
        "WHERE s.status = 'complete' AND e.entry_type = 'file' "
        "AND (e.file_hash IS NULL OR length(e.file_hash) <> 64 "
        "OR e.file_hash GLOB '*[^0-9a-f]*')"
        ")");
    if (!invalid.step()) {
        throw LocalVaultError(ErrorCode::database_error,
                              "repository relationship check returned no row");
    }
    const bool has_invalid_relationship = invalid.column_int64(0) != 0;
    if (invalid.step()) {
        throw LocalVaultError(ErrorCode::database_error,
                              "repository relationship check returned multiple rows");
    }
    if (has_invalid_relationship) {
        throw LocalVaultError(ErrorCode::database_error,
                              "repository metadata failed the quick relationship check");
    }
}

std::int64_t MetadataStore::insert_entry(SnapshotId snapshot_id, const ScannedEntry& entry) const {
    auto insert = database_.statement(
        "INSERT INTO entries (snapshot_id, relative_path, parent_path, name, entry_type, "
        "logical_size, modified_time_ns, posix_mode, windows_attributes, file_hash, "
        "symlink_target) VALUES (:snapshot_id, :relative_path, :parent_path, :name, :entry_type, "
        ":logical_size, :modified_time_ns, :posix_mode, :windows_attributes, :file_hash, "
        ":symlink_target) RETURNING id");
    insert.bind(":snapshot_id", snapshot_id);
    insert.bind(":relative_path", entry.relative_path);
    insert.bind(":parent_path", entry.parent_path);
    insert.bind(":name", entry.name);
    insert.bind(":entry_type", stored_entry_type(entry.type));
    insert.bind(":logical_size", checked_int64(entry.logical_size, "entry logical size"));
    insert.bind(":modified_time_ns", entry.modified_time_ns);
    insert.bind(":posix_mode", static_cast<std::int64_t>(entry.posix_mode));
    insert.bind_null(":windows_attributes");
    insert.bind_null(":file_hash");
    if (entry.symlink_target.has_value()) {
        insert.bind(":symlink_target", *entry.symlink_target);
    } else {
        insert.bind_null(":symlink_target");
    }
    if (!insert.step()) {
        throw LocalVaultError(ErrorCode::database_error,
                              "entry insertion returned no database identifier");
    }
    const std::int64_t id = insert.column_int64(0);
    if (insert.step()) {
        throw LocalVaultError(ErrorCode::database_error,
                              "entry insertion returned multiple database identifiers");
    }
    return id;
}

void MetadataStore::set_regular_file_hash(std::int64_t entry_id,
                                          std::string_view file_hash_hex) const {
    if (!is_lowercase_blake3_hex(file_hash_hex)) {
        throw LocalVaultError(ErrorCode::invalid_argument,
                              "file hash must be exactly 64 lowercase hexadecimal characters");
    }

    auto update =
        database_.statement("UPDATE entries SET file_hash = :file_hash WHERE id = :entry_id "
                            "AND entry_type = 'file' AND file_hash IS NULL AND EXISTS "
                            "(SELECT 1 FROM snapshots WHERE snapshots.id = entries.snapshot_id "
                            "AND snapshots.status = 'pending') RETURNING id");
    update.bind(":file_hash", file_hash_hex);
    update.bind(":entry_id", entry_id);
    if (!update.step()) {
        throw LocalVaultError(
            ErrorCode::invalid_argument,
            "setting a file hash requires an unhashed regular file in a pending snapshot");
    }
    (void)update.column_int64(0);
    if (update.step()) {
        throw LocalVaultError(ErrorCode::database_error,
                              "setting a file hash updated multiple entries");
    }
}

void MetadataStore::ensure_chunk(const StoredChunkInfo& chunk) const {
    auto insert = database_.statement(
        "INSERT INTO chunks (hash, raw_size, compressed_size, object_path, created_at_ns) "
        "VALUES (:hash, :raw_size, :stored_size, :object_path, :created_at_ns) "
        "ON CONFLICT DO NOTHING");
    insert.bind(":hash", chunk.hash_hex);
    insert.bind(":raw_size", checked_int64(chunk.raw_size, "chunk raw size"));
    insert.bind(":stored_size", checked_int64(chunk.stored_size, "chunk stored size"));
    insert.bind(":object_path", chunk.object_path.generic_string());
    insert.bind(":created_at_ns", chunk.created_at_ns);
    insert.execute();

    auto verify = database_.statement(
        "SELECT raw_size, compressed_size, object_path FROM chunks WHERE hash = :hash");
    verify.bind(":hash", chunk.hash_hex);
    if (!verify.step()) {
        throw LocalVaultError(ErrorCode::object_corrupt,
                              "chunk object path conflicts with existing metadata");
    }
    const ByteCount raw_size = checked_byte_count(verify.column_int64(0), "chunk raw size");
    const ByteCount stored_size = checked_byte_count(verify.column_int64(1), "chunk stored size");
    const std::string object_path = verify.column_text(2);
    if (verify.step()) {
        throw LocalVaultError(ErrorCode::database_error, "chunk hash selected multiple rows");
    }
    if (raw_size != chunk.raw_size || stored_size != chunk.stored_size ||
        object_path != chunk.object_path.generic_string()) {
        throw LocalVaultError(ErrorCode::object_corrupt,
                              "existing chunk metadata conflicts with stored content");
    }
}

std::optional<StoredChunkInfo> MetadataStore::find_chunk(std::string_view hash_hex) const {
    auto query = database_.statement("SELECT raw_size, compressed_size, object_path, created_at_ns "
                                     "FROM chunks WHERE hash = :hash");
    query.bind(":hash", hash_hex);
    if (!query.step()) {
        return std::nullopt;
    }

    const std::int64_t raw_size = query.column_int64(0);
    const std::int64_t stored_size = query.column_int64(1);
    if (raw_size <= 0 || stored_size <= 0) {
        throw LocalVaultError(ErrorCode::object_corrupt,
                              "chunk metadata contains invalid object sizes");
    }
    StoredChunkInfo chunk{
        .hash_hex = std::string(hash_hex),
        .raw_size = static_cast<ByteCount>(raw_size),
        .stored_size = static_cast<ByteCount>(stored_size),
        .object_path = utf8_path(query.column_text(2)),
        .created_at_ns = query.column_int64(3),
    };
    if (query.step()) {
        throw LocalVaultError(ErrorCode::database_error, "chunk hash selected multiple rows");
    }
    return chunk;
}

void MetadataStore::insert_entry_chunk(std::int64_t entry_id, std::int64_t sequence_number,
                                       std::string_view chunk_hash, ByteCount raw_offset,
                                       ByteCount raw_length) const {
    auto insert = database_.statement(
        "INSERT INTO entry_chunks (entry_id, sequence_number, chunk_hash, raw_offset, "
        "raw_length) VALUES (:entry_id, :sequence_number, :chunk_hash, :raw_offset, "
        ":raw_length)");
    insert.bind(":entry_id", entry_id);
    insert.bind(":sequence_number", sequence_number);
    insert.bind(":chunk_hash", chunk_hash);
    insert.bind(":raw_offset", checked_int64(raw_offset, "chunk raw offset"));
    insert.bind(":raw_length", checked_int64(raw_length, "chunk raw length"));
    insert.execute();
}

void MetadataStore::insert_warning(SnapshotId snapshot_id, std::string_view relative_path,
                                   std::string_view warning_code, std::string_view message) const {
    auto insert = database_.statement(
        "INSERT INTO snapshot_warnings (snapshot_id, relative_path, warning_code, message) "
        "VALUES (:snapshot_id, :relative_path, :warning_code, :message)");
    insert.bind(":snapshot_id", snapshot_id);
    insert.bind(":relative_path", relative_path);
    insert.bind(":warning_code", warning_code);
    insert.bind(":message", message);
    insert.execute();
}

std::vector<EntryInfo> MetadataStore::list_entries(SnapshotId snapshot_id) const {
    auto query = database_.statement("SELECT " + std::string(entry_projection) +
                                     " FROM entries WHERE snapshot_id = :snapshot_id "
                                     "AND EXISTS (SELECT 1 FROM snapshots "
                                     "WHERE snapshots.id = entries.snapshot_id "
                                     "AND snapshots.status = 'complete') "
                                     "ORDER BY relative_path");
    query.bind(":snapshot_id", snapshot_id);
    return read_entries(std::move(query));
}

std::vector<EntryInfo> MetadataStore::list_children(SnapshotId snapshot_id,
                                                    std::string_view parent_path) const {
    auto query = database_.statement(
        "SELECT " + std::string(entry_projection) +
        " FROM entries WHERE snapshot_id = :snapshot_id AND parent_path = :parent_path "
        "AND relative_path <> '' AND EXISTS (SELECT 1 FROM snapshots "
        "WHERE snapshots.id = entries.snapshot_id AND snapshots.status = 'complete') "
        "ORDER BY name, relative_path");
    query.bind(":snapshot_id", snapshot_id);
    query.bind(":parent_path", parent_path);
    return read_entries(std::move(query));
}

std::vector<ChunkReferenceInfo> MetadataStore::list_entry_chunks(std::int64_t entry_id) const {
    auto query = database_.statement(
        "SELECT ec.sequence_number, ec.chunk_hash, c.object_path, ec.raw_offset, ec.raw_length, "
        "c.raw_size, c.compressed_size FROM entry_chunks AS ec "
        "JOIN chunks AS c ON c.hash = ec.chunk_hash "
        "JOIN entries AS e ON e.id = ec.entry_id "
        "JOIN snapshots AS s ON s.id = e.snapshot_id "
        "WHERE ec.entry_id = :entry_id AND s.status = 'complete' "
        "ORDER BY ec.sequence_number");
    query.bind(":entry_id", entry_id);
    std::vector<ChunkReferenceInfo> chunks;
    while (query.step()) {
        chunks.push_back({
            query.column_int64(0),
            query.column_text(1),
            utf8_path(query.column_text(2)),
            checked_byte_count(query.column_int64(3), "chunk raw offset"),
            checked_byte_count(query.column_int64(4), "chunk raw length"),
            checked_byte_count(query.column_int64(5), "chunk raw size"),
            checked_byte_count(query.column_int64(6), "chunk stored size"),
        });
    }
    return chunks;
}

} // namespace localvault
