#include "localvault/snapshot_engine.hpp"

#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>

#include "database/metadata_store.hpp"
#include "filesystem/file_scanner.hpp"
#include "filesystem/platform/path_safety.hpp"
#include "filesystem/platform/platform_lock.hpp"
#include "localvault/error.hpp"
#include "localvault/repository.hpp"
#include "storage/chunker.hpp"
#include "storage/object_store.hpp"

namespace localvault {
namespace {

[[nodiscard]] std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] std::filesystem::path normalized_source(const std::filesystem::path& source) {
    if (source.empty()) {
        throw LocalVaultError(ErrorCode::invalid_argument, "source root path must not be empty");
    }
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(source, error);
    if (error) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to make source path absolute: " + error.message(), source);
    }
    const std::filesystem::path normalized = absolute.lexically_normal();
    const auto status = std::filesystem::symlink_status(normalized, error);
    if (error) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to inspect source root: " + error.message(), normalized);
    }
    if (!std::filesystem::is_directory(status) || std::filesystem::is_symlink(status)) {
        throw LocalVaultError(ErrorCode::invalid_argument,
                              "source root must be a directory and not a symbolic link",
                              normalized);
    }
    return normalized;
}

[[nodiscard]] std::filesystem::path canonical_path(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path canonical = std::filesystem::canonical(path, error);
    if (error) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to canonicalize path: " + error.message(), path);
    }
    return canonical;
}

void validate_containment(const std::filesystem::path& source,
                          const std::filesystem::path& repository) {
    const std::filesystem::path canonical_source = canonical_path(source);
    const std::filesystem::path canonical_repository = canonical_path(repository);
    if (platform_path_is_component_prefix(canonical_repository, canonical_source)) {
        throw LocalVaultError(ErrorCode::invalid_argument,
                              "source root must not be inside the repository", source);
    }
    if (platform_path_is_component_prefix(canonical_source, canonical_repository)) {
        throw LocalVaultError(ErrorCode::invalid_argument,
                              "repository must not be inside the source root", repository);
    }
}

[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& path) {
    const std::u8string encoded = path.generic_u8string();
    std::string result;
    result.reserve(encoded.size());
    for (const char8_t character : encoded) {
        result.push_back(static_cast<char>(character));
    }
    return result;
}

[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view value) {
    std::u8string encoded;
    encoded.reserve(value.size());
    for (const char character : value) {
        encoded.push_back(static_cast<char8_t>(static_cast<unsigned char>(character)));
    }
    return std::filesystem::path(encoded);
}

void add_bytes(ByteCount& total, ByteCount value, std::string_view counter) {
    if (total > (std::numeric_limits<ByteCount>::max)() - value) {
        throw LocalVaultError(ErrorCode::internal_error,
                              std::string(counter) + " counter overflow");
    }
    total += value;
}

void check_cancelled(std::stop_token stop_token, const std::filesystem::path& path = {}) {
    if (stop_token.stop_requested()) {
        throw LocalVaultError(ErrorCode::cancelled, "snapshot cancelled", path);
    }
}

void report_progress(const ProgressCallback& callback, OperationPhase phase,
                     const std::filesystem::path& current_path, const SnapshotTotals& totals,
                     std::uint64_t discovered_entries = 0) {
    if (!callback) {
        return;
    }
    callback(ProgressEvent{
        .phase = phase,
        .current_path = current_path,
        .discovered_entries = discovered_entries,
        .processed_entries = totals.file_count + totals.directory_count + totals.symlink_count,
        .processed_bytes = totals.logical_size,
        .total_entries = {},
        .total_bytes = {},
        .new_chunks = totals.new_chunk_count,
        .reused_chunks = totals.reused_chunk_count,
        .message = {},
    });
}

void mark_failed_best_effort(const MetadataStore& metadata, SnapshotId snapshot_id,
                             std::string_view message) noexcept {
    try {
        metadata.mark_snapshot_failed(snapshot_id, message, now_ns());
    } catch (const std::exception& error) {
        std::fprintf(stderr, "LocalVault: marking snapshot failed also failed: %s\n", error.what());
    } catch (...) {
        std::fprintf(stderr, "LocalVault: marking snapshot failed also failed\n");
    }
}

[[nodiscard]] std::int64_t checked_sequence(std::uint64_t sequence) {
    if (sequence > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        throw LocalVaultError(ErrorCode::internal_error, "file has too many chunks");
    }
    return static_cast<std::int64_t>(sequence);
}

} // namespace

SnapshotEngine::SnapshotEngine(Repository& repository) : repository_(repository) {}

SnapshotResult SnapshotEngine::create_snapshot(const std::filesystem::path& source_root,
                                               const SnapshotOptions& options,
                                               std::stop_token stop_token,
                                               ProgressCallback progress) {
    if (repository_.open_mode() != OpenMode::read_write) {
        throw LocalVaultError(ErrorCode::invalid_argument,
                              "snapshots require a read-write repository", repository_.root());
    }
    check_cancelled(stop_token, source_root);
    const std::filesystem::path source = normalized_source(source_root);
    validate_containment(source, repository_.root());

    SnapshotTotals totals;
    report_progress(progress, OperationPhase::preparing, source, totals);
    RepositoryLock writer_lock =
        RepositoryLock::acquire_exclusive(repository_.root() / "repository.lock");
    MetadataStore metadata(repository_.database());
    const std::int64_t started_at_ns = now_ns();
    const SnapshotId snapshot_id =
        metadata.create_pending_snapshot(path_to_utf8(source), options.message, started_at_ns);

    SnapshotResult result;
    result.snapshot_id = snapshot_id;
    try {
        check_cancelled(stop_token, source);
        report_progress(progress, OperationPhase::scanning, source, totals);
        check_cancelled(stop_token, source);
        const ScanResult scan = FileScanner{}.scan(source);
        check_cancelled(stop_token, source);
        for (const ScanWarning& warning : scan.warnings) {
            metadata.insert_warning(snapshot_id, warning.relative_path, warning.code,
                                    warning.message);
            result.skipped_entries.push_back(
                {path_from_utf8(warning.relative_path), warning.message});
        }

        Chunker chunker(repository_.info().chunk_size_bytes);
        ObjectStore objects(repository_.root());
        const std::uint64_t discovered_entries = scan.entries.size();
        for (const ScannedEntry& entry : scan.entries) {
            check_cancelled(stop_token, entry.source_path);
            report_progress(progress, OperationPhase::writing_metadata, entry.source_path, totals,
                            discovered_entries);
            const std::int64_t entry_id = metadata.insert_entry(snapshot_id, entry);
            switch (entry.type) {
            case EntryType::directory:
                ++totals.directory_count;
                break;
            case EntryType::symbolic_link:
                ++totals.symlink_count;
                break;
            case EntryType::regular_file: {
                ++totals.file_count;
                add_bytes(totals.logical_size, entry.logical_size, "logical size");
                std::uint64_t sequence = 0;
                ByteCount bytes_read = 0;
                report_progress(progress, OperationPhase::reading, entry.source_path, totals,
                                discovered_entries);
                chunker.for_each_chunk(
                    entry.source_path, stop_token,
                    [&](ByteCount raw_offset, std::span<const std::byte> raw_bytes) {
                        const StoredObject stored = objects.store(raw_bytes);
                        metadata.ensure_chunk({
                            .hash_hex = stored.hash_hex,
                            .raw_size = stored.raw_size,
                            .stored_size = stored.stored_size,
                            .object_path = stored.relative_path,
                            .created_at_ns = now_ns(),
                        });
                        metadata.insert_entry_chunk(entry_id, checked_sequence(sequence),
                                                    stored.hash_hex, raw_offset, stored.raw_size);
                        ++sequence;
                        add_bytes(bytes_read, stored.raw_size, "file bytes read");
                        if (stored.newly_stored) {
                            ++totals.new_chunk_count;
                            add_bytes(totals.new_stored_size, stored.stored_size,
                                      "new stored size");
                        } else {
                            ++totals.reused_chunk_count;
                        }
                    });
                if (bytes_read != entry.logical_size) {
                    throw LocalVaultError(ErrorCode::source_changed,
                                          "source file size changed while reading",
                                          entry.source_path);
                }
                break;
            }
            }
        }

        check_cancelled(stop_token, source);
        const auto elapsed = std::chrono::nanoseconds(now_ns() - started_at_ns);
        totals.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        report_progress(progress, OperationPhase::finalizing, source, totals, discovered_entries);
        metadata.mark_snapshot_complete(snapshot_id, totals, now_ns());
    } catch (const LocalVaultError& error) {
        mark_failed_best_effort(metadata, snapshot_id, error.what());
        throw;
    } catch (const std::exception& error) {
        mark_failed_best_effort(metadata, snapshot_id, error.what());
        throw LocalVaultError(ErrorCode::internal_error,
                              "snapshot failed: " + std::string(error.what()), source);
    } catch (...) {
        mark_failed_best_effort(metadata, snapshot_id, "unknown snapshot failure");
        throw;
    }

    result.file_count = totals.file_count;
    result.directory_count = totals.directory_count;
    result.logical_bytes = totals.logical_size;
    result.new_stored_bytes = totals.new_stored_size;
    result.new_chunks = totals.new_chunk_count;
    result.reused_chunks = totals.reused_chunk_count;
    report_progress(progress, OperationPhase::complete, source, totals);
    return result;
}

} // namespace localvault
