#include "localvault/integrity_verifier.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>

#include "database/database.hpp"
#include "database/statement.hpp"
#include "filesystem/platform/path_safety.hpp"
#include "filesystem/platform/platform_lock.hpp"
#include "localvault/error.hpp"
#include "localvault/repository.hpp"
#include "storage/object_store.hpp"

namespace localvault {
namespace {

using Kind = VerificationIssue::Kind;
using Severity = VerificationIssue::Severity;

[[nodiscard]] std::filesystem::path utf8_path(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

void check_cancelled(std::stop_token stop) {
    if (stop.stop_requested()) {
        throw LocalVaultError(ErrorCode::cancelled, "verification cancelled");
    }
}

void issue(VerificationResult& result, Kind kind, const std::filesystem::path& path,
           std::string detail, Severity severity = Severity::error) {
    result.issues.push_back({kind, path, std::move(detail), severity});
}

// Independent checks can still diagnose objects even if one metadata query fails.
template <typename Check> void check_database(VerificationResult& result, Check&& check) {
    try {
        check();
    } catch (const LocalVaultError& error) {
        if (error.code() != ErrorCode::database_error) {
            throw;
        }
        issue(result, Kind::invalid_database, error.path(), error.what());
    }
}

void check_repository_info(Database& db, VerificationResult& result, ByteCount& maximum_raw_size) {
    auto count = db.statement("SELECT COUNT(*) FROM repository_info");
    (void)count.step();
    if (count.column_int64(0) != 1) {
        issue(result, Kind::invalid_repository_info, {},
              "repository_info must contain exactly one row");
    }
    auto info = db.statement(
        "SELECT singleton_id, format_version, hash_algorithm, path_encoding, chunk_size_bytes, "
        "repository_uuid, typeof(format_version), typeof(chunk_size_bytes), typeof(zstd_level), "
        "zstd_level, typeof(hash_algorithm), typeof(path_encoding), typeof(repository_uuid) "
        "FROM repository_info");
    while (info.step()) {
        const auto size = info.column_int64(4);
        if (info.column_int64(0) != 1 || info.column_int64(1) != 1 ||
            info.column_text(2) != "blake3" || info.column_text(3) != "utf-8" || size <= 0 ||
            size > static_cast<std::int64_t>(ObjectStore::permanent_maximum_chunk_size) ||
            info.column_text(5).empty() || info.column_text(6) != "integer" ||
            info.column_text(7) != "integer" || info.column_text(8) != "integer" ||
            info.column_int64(9) < (std::numeric_limits<int>::min)() ||
            info.column_int64(9) > (std::numeric_limits<int>::max)() ||
            info.column_text(10) != "text" || info.column_text(11) != "text" ||
            info.column_text(12) != "text") {
            issue(result, Kind::invalid_repository_info, {},
                  "repository format, algorithms, identity, or chunk-size limit is invalid");
        } else {
            maximum_raw_size = static_cast<ByteCount>(size);
        }
    }
}

void check_snapshots(Database& db, VerificationResult& result, std::stop_token stop) {
    auto snapshots = db.statement(
        "SELECT s.id, s.status, s.completed_at_ns, s.file_count, s.directory_count, "
        "s.symlink_count, s.logical_size, s.new_stored_size, s.new_chunk_count, "
        "s.reused_chunk_count, s.duration_ms, "
        "(SELECT COUNT(*) FROM entries e WHERE e.snapshot_id=s.id AND e.entry_type='file'), "
        "(SELECT COUNT(*) FROM entries e WHERE e.snapshot_id=s.id AND e.entry_type='directory'), "
        "(SELECT COUNT(*) FROM entries e WHERE e.snapshot_id=s.id AND e.entry_type='symlink'), "
        "(SELECT COALESCE(SUM(e.logical_size),0) FROM entries e "
        "WHERE e.snapshot_id=s.id AND e.entry_type='file'), "
        "typeof(s.file_count)='integer' AND typeof(s.directory_count)='integer' "
        "AND typeof(s.symlink_count)='integer' AND typeof(s.logical_size)='integer' "
        "AND typeof(s.new_stored_size)='integer' AND typeof(s.new_chunk_count)='integer' "
        "AND typeof(s.reused_chunk_count)='integer' AND typeof(s.duration_ms)='integer' "
        "AND typeof(s.completed_at_ns)='integer' FROM snapshots s ORDER BY s.id");
    while (snapshots.step()) {
        check_cancelled(stop);
        ++result.checked_snapshots;
        const std::string label = "snapshot " + std::to_string(snapshots.column_int64(0));
        const std::string status = snapshots.column_text(1);
        if (status != "complete") {
            const bool stale = status == "pending" || status == "failed" || status == "cancelled" ||
                               status == "deleting";
            issue(result, Kind::invalid_snapshot_state, {}, label + " has status " + status,
                  stale ? Severity::note : Severity::error);
            continue;
        }
        bool invalid = snapshots.column_is_null(2) || snapshots.column_int64(15) == 0;
        for (int column = 3; column <= 10; ++column) {
            invalid = invalid || snapshots.column_int64(column) < 0;
        }
        for (int column = 3; column <= 6; ++column) {
            invalid =
                invalid || snapshots.column_int64(column) != snapshots.column_int64(column + 8);
        }
        if (invalid) {
            issue(result, Kind::invalid_snapshot_state, {}, label + " has invalid counters");
        }
    }
}

void check_entry_relationships(Database& db, VerificationResult& result, std::stop_token stop) {
    auto entries = db.statement(
        "SELECT e.id, e.relative_path, e.entry_type, e.logical_size, e.file_hash, "
        "COUNT(ec.sequence_number), COALESCE(SUM(ec.raw_length),0), "
        "COALESCE(MIN(ec.sequence_number),0), COALESCE(MAX(ec.sequence_number),-1), s.status "
        "FROM entries e LEFT JOIN entry_chunks ec ON ec.entry_id=e.id "
        "LEFT JOIN snapshots s ON s.id=e.snapshot_id GROUP BY e.id ORDER BY e.id");
    while (entries.step()) {
        check_cancelled(stop);
        ++result.checked_entries;
        // Incomplete entries can legitimately lack a final hash or chunk suffix.
        if (!entries.column_is_null(9) && entries.column_text(9) != "complete") {
            continue;
        }
        const bool file = entries.column_text(2) == "file";
        const auto count = entries.column_int64(5);
        bool invalid = entries.column_int64(3) < 0;
        if (file) {
            invalid = invalid || entries.column_is_null(4) ||
                      entries.column_int64(3) != entries.column_int64(6) ||
                      entries.column_int64(7) != 0 || entries.column_int64(8) != count - 1;
        } else {
            invalid = invalid || count != 0;
        }
        if (invalid) {
            issue(result, Kind::invalid_entry_relationship, utf8_path(entries.column_text(1)),
                  "entry " + std::to_string(entries.column_int64(0)) +
                      " has invalid file/chunk totals or sequence numbers");
        }
    }
    auto chunks = db.statement(
        "SELECT ec.entry_id, ec.sequence_number, ec.raw_offset, ec.raw_length, c.raw_size, "
        "SUM(ec.raw_length) OVER (PARTITION BY ec.entry_id ORDER BY ec.sequence_number "
        "ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING), c.hash "
        "FROM entry_chunks ec LEFT JOIN chunks c ON c.hash=ec.chunk_hash "
        "ORDER BY ec.entry_id, ec.sequence_number");
    while (chunks.step()) {
        check_cancelled(stop);
        const auto prior = chunks.column_is_null(5) ? 0 : chunks.column_int64(5);
        if (chunks.column_is_null(6) || chunks.column_int64(2) != prior ||
            chunks.column_int64(3) <= 0 || chunks.column_int64(3) != chunks.column_int64(4)) {
            issue(result, Kind::invalid_entry_relationship, {},
                  "entry " + std::to_string(chunks.column_int64(0)) + " chunk " +
                      std::to_string(chunks.column_int64(1)) +
                      " has a missing chunk row or invalid offset/length");
        }
    }
}

// Only these hash-derived components may be inspected, never a stored DB path.
void require_safe_object(const std::filesystem::path& root, const std::filesystem::path& relative) {
    std::filesystem::path current = root;
    for (const auto& component : relative) {
        current /= component;
        const auto type = inspect_path_no_follow(current);
        if (type == NoFollowPathType::not_found) {
            throw LocalVaultError(ErrorCode::object_missing, "stored object is missing", current);
        }
        if (type == NoFollowPathType::indirection ||
            (current != root / relative && type != NoFollowPathType::directory)) {
            throw LocalVaultError(ErrorCode::object_corrupt,
                                  "object path contains an indirection or invalid directory",
                                  current);
        }
    }
}

void check_objects(Database& db, const std::filesystem::path& root, ObjectStore& store,
                   ByteCount maximum_raw_size, VerifyMode mode, VerificationResult& result,
                   std::stop_token stop, const ProgressCallback& progress) {
    auto chunks = db.statement("SELECT c.hash, c.object_path, c.raw_size, c.compressed_size, "
                               "EXISTS(SELECT 1 FROM entry_chunks ec WHERE ec.chunk_hash=c.hash), "
                               "typeof(c.raw_size), typeof(c.compressed_size) "
                               "FROM chunks c ORDER BY c.hash");
    while (chunks.step()) {
        check_cancelled(stop);
        const bool referenced = chunks.column_int64(4) != 0;
        const Severity severity = referenced ? Severity::error : Severity::note;
        if (referenced) {
            ++result.checked_objects;
        }
        std::filesystem::path relative;
        try {
            const auto hash = chunks.column_text(0);
            relative = ObjectStore::object_relative_path(hash);
            if (relative.generic_string() != chunks.column_text(1)) {
                issue(result, Kind::corrupt_object, relative,
                      "stored object path does not match the hash-derived path", severity);
            }
            const auto raw_size = chunks.column_int64(2);
            const auto stored_size = chunks.column_int64(3);
            const bool valid_sizes =
                chunks.column_text(5) == "integer" && chunks.column_text(6) == "integer" &&
                raw_size > 0 && stored_size > 0 &&
                static_cast<ByteCount>(raw_size) <= maximum_raw_size &&
                static_cast<ByteCount>(stored_size) <= maximum_raw_size + 65536U;
            if (!valid_sizes) {
                issue(result, Kind::invalid_chunk_size, relative,
                      "raw or compressed chunk size is outside the supported limit", severity);
            }
            require_safe_object(root, relative);
            std::error_code error;
            const auto status = std::filesystem::symlink_status(root / relative, error);
            if (!std::filesystem::is_regular_file(status) || error) {
                throw LocalVaultError(ErrorCode::object_corrupt,
                                      "stored object is not a readable regular file",
                                      root / relative);
            }
            const auto size = std::filesystem::file_size(root / relative, error);
            if (error) {
                throw LocalVaultError(ErrorCode::filesystem_error, error.message(),
                                      root / relative);
            }
            if (referenced) {
                if (size > (std::numeric_limits<ByteCount>::max)() - result.checked_stored_bytes) {
                    throw LocalVaultError(ErrorCode::object_corrupt,
                                          "checked object sizes overflow");
                }
                result.checked_stored_bytes += static_cast<ByteCount>(size);
            }
            if (stored_size <= 0 || size != static_cast<std::uintmax_t>(stored_size)) {
                issue(result, Kind::corrupt_object, relative,
                      "stored object size does not match compressed_size", severity);
            } else if (referenced && mode == VerifyMode::full && valid_sizes) {
                (void)store.read_verified(hash, relative, static_cast<ByteCount>(raw_size),
                                          static_cast<ByteCount>(stored_size));
            }
        } catch (const LocalVaultError& error) {
            if (error.code() == ErrorCode::database_error || error.code() == ErrorCode::cancelled) {
                throw;
            }
            const Kind kind = !referenced ? Kind::stale_chunk_metadata
                              : error.code() == ErrorCode::object_missing ? Kind::missing_object
                                                                          : Kind::corrupt_object;
            issue(result, kind, relative, error.what(), severity);
        }
        if (progress) {
            ProgressEvent event;
            event.phase = OperationPhase::verifying;
            event.current_path = relative;
            event.processed_entries = result.checked_objects;
            event.processed_bytes = result.checked_stored_bytes;
            progress(event);
        }
    }
}

void check_files(Database& db, const std::filesystem::path& root,
                 const std::filesystem::path& directory, bool temporary, VerificationResult& result,
                 std::stop_token stop) {
    const auto kind = temporary ? Kind::stale_temporary_file : Kind::orphan_object;
    const auto type = inspect_path_no_follow(directory);
    if (type != NoFollowPathType::directory) {
        issue(result, kind, directory, "repository storage directory is missing or indirect");
        return;
    }
    auto known = db.statement("SELECT 1 FROM chunks WHERE hash=:hash");
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(directory, error), end;
    if (error) {
        issue(result, kind, directory, error.message());
        return;
    }
    while (iterator != end) {
        check_cancelled(stop);
        const auto path = iterator->path();
        const auto child = inspect_path_no_follow(path);
        if (child != NoFollowPathType::directory) {
            iterator.disable_recursion_pending();
            bool orphan = true;
            if (!temporary && child != NoFollowPathType::indirection) {
                const auto hash = path.stem().string();
                try {
                    if (path.lexically_relative(root) == ObjectStore::object_relative_path(hash)) {
                        known.bind(":hash", hash);
                        orphan = !known.step();
                        known.reset();
                    }
                } catch (const LocalVaultError& invalid_hash) {
                    if (invalid_hash.code() != ErrorCode::invalid_argument) {
                        throw;
                    }
                }
            }
            if (temporary || orphan) {
                issue(result, kind, path.lexically_relative(root),
                      temporary ? "stale temporary file; maintenance candidate"
                                : "unregistered object or indirection; maintenance candidate",
                      Severity::note);
            }
        }
        iterator.increment(error);
        if (error) {
            issue(result, kind, path, "cannot finish storage directory scan: " + error.message());
            return;
        }
    }
}

} // namespace

bool VerificationResult::ok() const noexcept {
    return std::none_of(issues.begin(), issues.end(), [](const auto& value) {
        return value.severity == VerificationIssue::Severity::error;
    });
}

IntegrityVerifier::IntegrityVerifier(Repository& repository) : repository_(repository) {}

VerificationResult IntegrityVerifier::verify(VerifyMode mode, std::stop_token stop,
                                             ProgressCallback progress) {
    if (repository_.open_mode() != OpenMode::read_write) {
        throw LocalVaultError(ErrorCode::invalid_argument,
                              "verification requires a read-write repository", repository_.root());
    }
    if (mode != VerifyMode::quick && mode != VerifyMode::full) {
        throw LocalVaultError(ErrorCode::invalid_argument, "unknown verification mode");
    }
    check_cancelled(stop);
    repository_.validate_root_after_open();
    const auto lock =
        RepositoryLock::acquire_exclusive(repository_.root() / "repository.lock", false);
    repository_.validate_root_after_open();
    // Recovery would destroy the very stale metadata and temporary files we must report.
    Database db(repository_.root() / "repository.db", DatabaseAccess::locked_read_only);
    VerificationResult result;
    check_database(result, [&] {
        auto integrity = db.statement("PRAGMA integrity_check");
        while (integrity.step()) {
            check_cancelled(stop);
            if (integrity.column_text(0) != "ok") {
                issue(result, Kind::invalid_database, {}, integrity.column_text(0));
            }
        }
    });
    check_database(result, [&] {
        auto foreign_keys = db.statement("PRAGMA foreign_key_check");
        while (foreign_keys.step()) {
            check_cancelled(stop);
            issue(result, Kind::invalid_entry_relationship, {},
                  "foreign key violation in " + foreign_keys.column_text(0) + " row " +
                      std::to_string(foreign_keys.column_int64(1)));
        }
    });
    ByteCount maximum_raw_size = repository_.info().chunk_size_bytes;
    check_database(result, [&] { check_repository_info(db, result, maximum_raw_size); });
    check_database(result, [&] { check_snapshots(db, result, stop); });
    check_database(result, [&] { check_entry_relationships(db, result, stop); });
    ObjectStore store(repository_.root(), maximum_raw_size, repository_.info().zstd_level,
                      repository_.failure_injector());
    auto last_progress = std::chrono::steady_clock::time_point::min();
    const auto throttled_progress = [&](const ProgressEvent& event) {
        const auto now = std::chrono::steady_clock::now();
        if (progress && (last_progress == std::chrono::steady_clock::time_point::min() ||
                         now - last_progress >= std::chrono::milliseconds(100))) {
            progress(event);
            last_progress = now;
        }
    };
    check_database(result, [&] {
        check_objects(db, repository_.root(), store, maximum_raw_size, mode, result, stop,
                      throttled_progress);
    });
    for (const std::string_view directory : {"objects", "temporary"}) {
        check_cancelled(stop);
        check_database(result, [&] {
            try {
                check_files(db, repository_.root(), repository_.root() / directory,
                            directory == "temporary", result, stop);
            } catch (const LocalVaultError& error) {
                if (error.code() != ErrorCode::filesystem_error) {
                    throw;
                }
                issue(result, Kind::corrupt_object, error.path(), error.what());
            }
        });
    }
    check_cancelled(stop);
    if (progress) {
        ProgressEvent event;
        event.phase = OperationPhase::complete;
        event.processed_entries = result.checked_objects;
        event.processed_bytes = result.checked_stored_bytes;
        progress(event);
    }
    return result;
}

} // namespace localvault
