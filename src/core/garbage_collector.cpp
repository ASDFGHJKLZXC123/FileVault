#include "localvault/garbage_collector.hpp"

#include <chrono>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "database/database.hpp"
#include "database/metadata_store.hpp"
#include "database/statement.hpp"
#include "database/transaction.hpp"
#include "filesystem/platform/path_safety.hpp"
#include "filesystem/platform/platform_lock.hpp"
#include "localvault/error.hpp"
#include "localvault/repository.hpp"
#include "storage/object_store.hpp"

namespace localvault {
namespace {

void check_stop(std::stop_token token) {
    if (token.stop_requested()) {
        throw LocalVaultError(ErrorCode::cancelled, "maintenance cancelled");
    }
}

void add(ByteCount& total, ByteCount value) {
    if (value > (std::numeric_limits<ByteCount>::max)() - total) {
        throw LocalVaultError(ErrorCode::database_error, "maintenance byte/count overflow");
    }
    total += value;
}

void require_directory(const std::filesystem::path& path) {
    if (inspect_path_no_follow(path) != NoFollowPathType::directory) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "maintenance requires a real directory without indirection", path);
    }
}

std::optional<ByteCount> regular_size(const std::filesystem::path& path) {
    const auto type = inspect_path_no_follow(path);
    if (type == NoFollowPathType::not_found) {
        return std::nullopt;
    }
    if (type != NoFollowPathType::other || !std::filesystem::is_regular_file(path)) {
        throw LocalVaultError(ErrorCode::object_corrupt,
                              "maintenance refuses non-regular files or indirection", path);
    }
    return std::filesystem::file_size(path);
}

std::optional<ByteCount> object_size(const std::filesystem::path& root,
                                     const std::filesystem::path& relative) {
    require_directory(root / "objects");
    const auto parent = root / relative.parent_path();
    if (inspect_path_no_follow(parent) == NoFollowPathType::not_found) {
        return std::nullopt;
    }
    require_directory(parent);
    return regular_size(root / relative);
}

struct TemporaryTotals {
    std::uint64_t files{};
    ByteCount bytes{};
};

TemporaryTotals inspect_temporary(const std::filesystem::path& root, std::stop_token token) {
    const auto temporary = root / "temporary";
    require_directory(temporary);
    TemporaryTotals totals;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(temporary)) {
        check_stop(token);
        const auto type = inspect_path_no_follow(entry.path());
        if (type == NoFollowPathType::directory) {
            continue;
        }
        // Preflight recovery so its recursive removal never crosses a link or junction.
        const auto bytes = regular_size(entry.path());
        if (bytes) {
            add(totals.files, 1);
            add(totals.bytes, *bytes);
        }
    }
    return totals;
}

class CancellableInjector final : public FailureInjector {
  public:
    CancellableInjector(FailureInjector& delegate, std::stop_token token)
        : delegate_(delegate), token_(token) {}
    void hit(FailurePoint point) override {
        check_stop(token_);
        delegate_.hit(point);
    }

  private:
    FailureInjector& delegate_;
    std::stop_token token_;
};

bool chunk_is_unreferenced(Database& database, std::string_view hash) {
    auto query = database.statement("SELECT 1 FROM chunks c WHERE c.hash = :hash AND NOT EXISTS "
                                    "(SELECT 1 FROM entry_chunks ec WHERE ec.chunk_hash = c.hash)");
    query.bind(":hash", hash);
    return query.step();
}

bool known_object(Database& database, std::string_view hash, std::string_view path, bool preview) {
    const std::string references =
        preview ? "SELECT 1 FROM entry_chunks ec LEFT JOIN entries e ON e.id=ec.entry_id "
                  "LEFT JOIN snapshots s ON s.id=e.snapshot_id WHERE ec.chunk_hash=:hash "
                  "AND (s.status='complete' OR s.id IS NULL)"
                : "SELECT 1 FROM entry_chunks WHERE chunk_hash=:hash";
    auto query =
        database.statement("SELECT 1 FROM chunks WHERE hash = :hash OR object_path = :path "
                           "UNION ALL " +
                           references + " LIMIT 1");
    query.bind(":hash", hash);
    query.bind(":path", path);
    return query.step();
}

std::filesystem::path canonical_object(std::string_view hash, std::string_view stored_path) {
    const auto relative = ObjectStore::object_relative_path(hash);
    if (relative.generic_string() != stored_path) {
        throw LocalVaultError(ErrorCode::object_corrupt,
                              "chunk path differs from its canonical hash-derived path");
    }
    return relative;
}

void remove_object(const std::filesystem::path& root, const std::filesystem::path& relative,
                   GarbageCollectionResult& result, FailureInjector& injector) {
    const auto bytes = object_size(root, relative);
    if (bytes && std::filesystem::remove(root / relative)) {
        add(result.removed_objects, 1);
        add(result.reclaimed_bytes, *bytes);
    }
    injector.hit(FailurePoint::after_gc_object_delete);
}

} // namespace

GarbageCollector::GarbageCollector(Repository& repository) noexcept : repository_(repository) {}

void GarbageCollector::delete_snapshot(SnapshotId id, std::stop_token token) try {
    if (repository_.open_mode() != OpenMode::read_write) {
        throw LocalVaultError(ErrorCode::invalid_argument,
                              "deletion requires a writable repository");
    }
    check_stop(token);
    repository_.validate_root_after_open();
    (void)regular_size(repository_.root() / "repository.lock");
    const auto lock = RepositoryLock::acquire_exclusive(repository_.root() / "repository.lock");
    repository_.validate_root_after_open();
    MetadataStore metadata(repository_.database());
    // Refuse pending/in-use snapshots before recovery can change their status.
    (void)metadata.require_complete_snapshot(id);
    (void)inspect_temporary(repository_.root(), token);
    repository_.validate_root_after_open();
    repository_.recover_after_writer_lock(true, token);
    CancellableInjector injector(*repository_.failure_injector(), token);
    metadata.transition_snapshot_to_deleting(id, injector);
    metadata.delete_deleting_snapshot(id, injector);
} catch (const std::filesystem::filesystem_error& error) {
    throw LocalVaultError(ErrorCode::filesystem_error, error.what(), error.path1());
}

GarbageCollectionResult GarbageCollector::collect(const GarbageCollectionOptions& options,
                                                  std::stop_token token,
                                                  ProgressCallback progress) try {
    if (repository_.open_mode() != OpenMode::read_write || options.batch_size == 0 ||
        options.batch_size > 10'000) {
        throw LocalVaultError(ErrorCode::invalid_argument,
                              "GC requires a writable repository and batch size in 1..10000");
    }
    check_stop(token);
    const auto& root = repository_.root();
    repository_.validate_root_after_open();
    (void)regular_size(root / "repository.lock");
    const auto lock = RepositoryLock::acquire_exclusive(root / "repository.lock", !options.dry_run);
    repository_.validate_root_after_open();
    std::optional<Database> preview_database;
    if (options.dry_run) {
        preview_database.emplace(root / "repository.db", DatabaseAccess::locked_read_only);
    }
    Database& database = preview_database ? *preview_database : repository_.database();
    auto injector = repository_.failure_injector();
    GarbageCollectionResult result;
    auto last_progress = std::chrono::steady_clock::time_point::min();
    const auto emit = [&](bool complete) {
        const auto now = std::chrono::steady_clock::now();
        if (progress &&
            (complete || last_progress == std::chrono::steady_clock::time_point::min() ||
             now - last_progress >= std::chrono::milliseconds(100))) {
            ProgressEvent event;
            event.phase = complete ? OperationPhase::complete : OperationPhase::garbage_collecting;
            event.processed_entries = result.unreferenced_chunks + result.orphan_objects;
            event.processed_bytes = result.reclaimable_bytes;
            progress(event);
            last_progress = now;
        }
    };
    emit(false);
    check_stop(token);
    repository_.validate_root_after_open();
    const auto temporary = inspect_temporary(root, token);
    result.stale_temporary_files = temporary.files;
    add(result.reclaimable_bytes, temporary.bytes);
    {
        auto stale =
            database.statement("SELECT COUNT(*) FROM snapshots WHERE status <> 'complete'");
        (void)stale.step();
        result.stale_snapshots = static_cast<std::uint64_t>(stale.column_int64(0));
    }
    if (!options.dry_run) {
        repository_.validate_root_after_open();
        repository_.recover_after_writer_lock(true, token);
        const auto remaining = inspect_temporary(root, token);
        if (remaining.bytes <= temporary.bytes) {
            result.reclaimed_bytes = temporary.bytes - remaining.bytes;
        }
    }

    // Preview predicts recovery without mutating incomplete snapshots or their references.
    const std::string live_references =
        options.dry_run ? "SELECT 1 FROM entry_chunks ec JOIN entries e ON e.id = ec.entry_id "
                          "JOIN snapshots s ON s.id = e.snapshot_id WHERE ec.chunk_hash = c.hash "
                          "AND s.status = 'complete'"
                        : "SELECT 1 FROM entry_chunks ec WHERE ec.chunk_hash = c.hash";
    std::string cursor;
    for (;;) {
        check_stop(token);
        std::vector<std::pair<std::string, std::filesystem::path>> batch;
        {
            auto query = database.statement(
                "SELECT c.hash, c.object_path FROM chunks c WHERE c.hash > :cursor "
                "AND NOT EXISTS (" +
                live_references + ") ORDER BY c.hash LIMIT :limit");
            query.bind(":cursor", cursor);
            query.bind(":limit", static_cast<std::int64_t>(options.batch_size));
            while (query.step()) {
                auto hash = query.column_text(0);
                auto relative = canonical_object(hash, query.column_text(1));
                batch.emplace_back(std::move(hash), std::move(relative));
            }
        }
        if (batch.empty()) {
            break;
        }
        std::optional<Transaction> transaction;
        if (!options.dry_run) {
            transaction.emplace(database, TransactionMode::exclusive);
        }
        for (const auto& [hash, relative] : batch) {
            check_stop(token);
            if (!options.dry_run && !chunk_is_unreferenced(database, hash)) {
                continue;
            }
            add(result.unreferenced_chunks, 1);
            add(result.reclaimable_bytes, object_size(root, relative).value_or(0));
            if (!options.dry_run) {
                repository_.validate_root_after_open();
                remove_object(root, relative, result, *injector);
                auto remove = database.statement("DELETE FROM chunks WHERE hash = :hash");
                remove.bind(":hash", hash);
                remove.execute();
            }
        }
        cursor = batch.back().first;
        if (transaction) {
            check_stop(token);
            injector->hit(FailurePoint::before_metadata_batch_commit);
            transaction->commit();
        }
        emit(false);
    }

    require_directory(root / "objects");
    for (const auto& shard : std::filesystem::directory_iterator(root / "objects")) {
        check_stop(token);
        const auto shard_utf8 = shard.path().filename().generic_u8string();
        const std::string shard_name(shard_utf8.begin(), shard_utf8.end());
        if (shard_name.size() != 2 ||
            shard_name.find_first_not_of("0123456789abcdef") != std::string::npos) {
            add(result.ignored_paths, 1);
            continue;
        }
        require_directory(shard.path());
        for (const auto& file : std::filesystem::directory_iterator(shard.path())) {
            check_stop(token);
            const auto name_utf8 = file.path().filename().generic_u8string();
            const std::string name(name_utf8.begin(), name_utf8.end());
            if (name.size() != 68 || name.substr(64) != ".zst" || name.substr(0, 2) != shard_name ||
                name.substr(0, 64).find_first_not_of("0123456789abcdef") != std::string::npos) {
                add(result.ignored_paths, 1);
                continue;
            }
            const auto hash = name.substr(0, 64);
            const auto relative = ObjectStore::object_relative_path(hash);
            std::optional<Transaction> transaction;
            if (!options.dry_run) {
                transaction.emplace(database, TransactionMode::exclusive);
            }
            if (!known_object(database, hash, relative.generic_string(), options.dry_run)) {
                const auto bytes = object_size(root, relative);
                if (bytes) {
                    add(result.orphan_objects, 1);
                    add(result.reclaimable_bytes, *bytes);
                    if (!options.dry_run) {
                        repository_.validate_root_after_open();
                        remove_object(root, relative, result, *injector);
                    }
                }
            }
            if (transaction) {
                transaction->commit();
            }
            emit(false);
        }
    }
    check_stop(token);
    try {
        emit(true);
    } catch (...) {
        // A notification cannot undo maintenance already committed successfully.
    }
    return result;
} catch (const std::filesystem::filesystem_error& error) {
    throw LocalVaultError(ErrorCode::filesystem_error, error.what(), error.path1());
}

} // namespace localvault
