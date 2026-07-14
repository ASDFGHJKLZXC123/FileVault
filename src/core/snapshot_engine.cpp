#include "localvault/snapshot_engine.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "concurrency/bounded_queue.hpp"
#include "database/metadata_store.hpp"
#include "database/transaction.hpp"
#include "filesystem/file_scanner.hpp"
#include "filesystem/platform/file_metadata.hpp"
#include "filesystem/platform/path_safety.hpp"
#include "filesystem/platform/platform_lock.hpp"
#include "localvault/error.hpp"
#include "localvault/failure_injector.hpp"
#include "localvault/repository.hpp"
#include "storage/blake3_hasher.hpp"
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

class ProgressAggregator final {
  public:
    using TimePoint = std::chrono::steady_clock::time_point;
    using ClockFunction = std::function<TimePoint()>;

    explicit ProgressAggregator(ProgressCallback callback, ClockFunction clock = {})
        : callback_(std::move(callback)),
          clock_(clock ? std::move(clock) : [] { return std::chrono::steady_clock::now(); }) {}

    void emit_required(OperationPhase phase, const std::filesystem::path& current_path) {
        emit(phase, current_path, true);
    }

    void record_discovered(const ScannedEntry& entry) {
        add_atomic(discovered_entries_, 1, "discovered entries");
        add_atomic(discovered_bytes_, entry.logical_size, "discovered bytes");
        notify(entry.source_path);
    }

    void record_processed(const ScannedEntry& entry, std::uint64_t new_chunks,
                          std::uint64_t reused_chunks) {
        add_atomic(processed_entries_, 1, "processed entries");
        add_atomic(processed_bytes_, entry.logical_size, "processed bytes");
        add_atomic(new_chunks_, new_chunks, "new chunks");
        add_atomic(reused_chunks_, reused_chunks, "reused chunks");
        notify(entry.source_path);
    }

    void mark_scan_complete(const std::filesystem::path& current_path) {
        {
            std::lock_guard lock(state_mutex_);
            scan_complete_ = true;
            scan_completion_pending_ = true;
            current_path_ = current_path;
            dirty_ = true;
        }
        state_changed_.notify_one();
    }

    void finish() {
        {
            std::lock_guard lock(state_mutex_);
            finished_ = true;
        }
        state_changed_.notify_one();
    }

    void cancel() {
        {
            std::lock_guard lock(state_mutex_);
            cancelled_ = true;
        }
        state_changed_.notify_one();
    }

    void run() {
        while (true) {
            std::filesystem::path current_path;
            bool scan_completion = false;
            {
                std::unique_lock lock(state_mutex_);
                state_changed_.wait(lock, [&] { return dirty_ || finished_ || cancelled_; });
                if (cancelled_) {
                    return;
                }
                if (!dirty_) {
                    return;
                }
                current_path = current_path_;
                scan_completion = scan_completion_pending_;
                scan_completion_pending_ = false;
                dirty_ = false;
            }

            const bool has_processed = processed_entries_.load(std::memory_order_relaxed) != 0;
            if (scan_completion) {
                const OperationPhase phase =
                    has_processed ? OperationPhase::writing_metadata : OperationPhase::scanning;
                emit_required(phase, current_path);
                if (has_processed) {
                    writing_phase_emitted_ = true;
                }
            } else if (has_processed && !writing_phase_emitted_) {
                emit_required(OperationPhase::writing_metadata, current_path);
                writing_phase_emitted_ = true;
            } else {
                emit(has_processed ? OperationPhase::writing_metadata : OperationPhase::scanning,
                     current_path, false);
            }
        }
    }

  private:
    struct Emission final {
        TimePoint time;
        bool required{};
    };

    static void add_atomic(std::atomic<std::uint64_t>& counter, std::uint64_t value,
                           std::string_view name) {
        std::uint64_t current = counter.load(std::memory_order_relaxed);
        do {
            if (current > (std::numeric_limits<std::uint64_t>::max)() - value) {
                throw LocalVaultError(ErrorCode::internal_error,
                                      std::string(name) + " counter overflow");
            }
        } while (
            !counter.compare_exchange_weak(current, current + value, std::memory_order_relaxed));
    }

    void notify(const std::filesystem::path& current_path) {
        {
            std::lock_guard lock(state_mutex_);
            current_path_ = current_path;
            dirty_ = true;
        }
        state_changed_.notify_one();
    }

    [[nodiscard]] ProgressEvent event(OperationPhase phase,
                                      const std::filesystem::path& current_path) {
        const bool complete = [&] {
            std::lock_guard lock(state_mutex_);
            return scan_complete_;
        }();
        return ProgressEvent{
            .phase = phase,
            .current_path = current_path,
            .discovered_entries = discovered_entries_.load(std::memory_order_relaxed),
            .processed_entries = processed_entries_.load(std::memory_order_relaxed),
            .processed_bytes = processed_bytes_.load(std::memory_order_relaxed),
            .total_entries = complete ? std::optional<std::uint64_t>(
                                            discovered_entries_.load(std::memory_order_relaxed))
                                      : std::nullopt,
            .total_bytes = complete ? std::optional<std::uint64_t>(
                                          discovered_bytes_.load(std::memory_order_relaxed))
                                    : std::nullopt,
            .new_chunks = new_chunks_.load(std::memory_order_relaxed),
            .reused_chunks = reused_chunks_.load(std::memory_order_relaxed),
            .message = {},
        };
    }

    void emit(OperationPhase phase, const std::filesystem::path& current_path, bool required) {
        if (!callback_) {
            return;
        }
        const TimePoint now = clock_();
        while (!emissions_.empty() && now >= emissions_.front().time &&
               now - emissions_.front().time >= std::chrono::seconds(1)) {
            emissions_.pop_front();
        }
        const auto ordinary_count =
            std::count_if(emissions_.begin(), emissions_.end(),
                          [](const Emission& emission) { return !emission.required; });
        if (emissions_.size() >= 10U || (!required && ordinary_count >= 4)) {
            return;
        }
        emissions_.push_back({now, required});
        callback_(event(phase, current_path));
    }

    ProgressCallback callback_;
    ClockFunction clock_;
    std::atomic<std::uint64_t> discovered_entries_{0};
    std::atomic<std::uint64_t> discovered_bytes_{0};
    std::atomic<std::uint64_t> processed_entries_{0};
    std::atomic<std::uint64_t> processed_bytes_{0};
    std::atomic<std::uint64_t> new_chunks_{0};
    std::atomic<std::uint64_t> reused_chunks_{0};
    std::mutex state_mutex_;
    std::condition_variable state_changed_;
    std::filesystem::path current_path_;
    bool dirty_{};
    bool finished_{};
    bool cancelled_{};
    bool scan_complete_{};
    bool scan_completion_pending_{};
    bool writing_phase_emitted_{};
    std::deque<Emission> emissions_;
};

void mark_incomplete_best_effort(const MetadataStore& metadata, SnapshotId snapshot_id,
                                 SnapshotStatus status, std::string_view message,
                                 FailureInjector& failure_injector,
                                 std::size_t metadata_batch_limit) noexcept {
    try {
        metadata.mark_snapshot_incomplete(snapshot_id, status, message, now_ns());
        metadata.clean_incomplete_snapshot(snapshot_id, failure_injector, metadata_batch_limit);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "LocalVault: cleaning incomplete snapshot also failed: %s\n",
                     error.what());
    } catch (...) {
        std::fprintf(stderr, "LocalVault: cleaning incomplete snapshot also failed\n");
    }
}

[[nodiscard]] std::int64_t checked_sequence(std::uint64_t sequence) {
    if (sequence > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        throw LocalVaultError(ErrorCode::internal_error, "file has too many chunks");
    }
    return static_cast<std::int64_t>(sequence);
}

class SynchronizedFailureInjector final : public FailureInjector {
  public:
    explicit SynchronizedFailureInjector(std::shared_ptr<FailureInjector> delegate)
        : delegate_(std::move(delegate)) {}

    void hit(FailurePoint point) override {
        std::lock_guard lock(mutex_);
        delegate_->hit(point);
    }

  private:
    std::shared_ptr<FailureInjector> delegate_;
    std::mutex mutex_;
};

struct FileJob {
    std::variant<ScannedEntry, ScanWarning> value;
};

struct ChunkReferenceRecord {
    std::array<char, Blake3Hasher::digest_size * 2U> hash_hex{};
    ByteCount raw_offset{};
    std::int64_t sequence_number{};
    ByteCount raw_size{};
    ByteCount stored_size{};
    std::uint8_t newly_stored{};
    std::array<std::byte, 7> reserved{};
};

static_assert(std::is_trivially_copyable_v<ChunkReferenceRecord>);
static_assert(sizeof(ChunkReferenceRecord) == 104U);

class ChunkReferenceSpool final {
  public:
    explicit ChunkReferenceSpool(const std::filesystem::path& directory) : file_(directory) {}

    ChunkReferenceSpool(ChunkReferenceSpool&& other) noexcept
        : file_(std::move(other.file_)), reader_(std::move(other.reader_)) {}

    ChunkReferenceSpool& operator=(ChunkReferenceSpool&& other) noexcept {
        if (this != &other) {
            reader_.reset();
            file_ = std::move(other.file_);
            reader_ = std::move(other.reader_);
        }
        return *this;
    }

    ChunkReferenceSpool(const ChunkReferenceSpool&) = delete;
    ChunkReferenceSpool& operator=(const ChunkReferenceSpool&) = delete;

    void append(const StoredObject& object, ByteCount raw_offset, std::int64_t sequence_number) {
        if (object.hash_hex.size() != ChunkReferenceRecord{}.hash_hex.size()) {
            throw LocalVaultError(ErrorCode::internal_error,
                                  "stored object has an invalid chunk-reference hash");
        }
        ChunkReferenceRecord record{};
        std::copy(object.hash_hex.begin(), object.hash_hex.end(), record.hash_hex.begin());
        record.raw_offset = raw_offset;
        record.sequence_number = sequence_number;
        record.raw_size = object.raw_size;
        record.stored_size = object.stored_size;
        record.newly_stored = object.newly_stored ? 1U : 0U;
        file_.write(std::as_bytes(std::span<const ChunkReferenceRecord>(&record, std::size_t{1})));
    }

    void rewind_for_reading() {
        reader_ = std::make_unique<std::ifstream>(file_.path(), std::ios::binary);
        if (!*reader_) {
            throw LocalVaultError(ErrorCode::filesystem_error,
                                  "failed to open temporary chunk-reference spool", file_.path());
        }
    }

    [[nodiscard]] ChunkReferenceRecord read() {
        ChunkReferenceRecord record{};
        const auto size = static_cast<std::streamsize>(sizeof(record));
        reader_->read(reinterpret_cast<char*>(&record), size);
        if (reader_->gcount() != size) {
            throw LocalVaultError(ErrorCode::filesystem_error,
                                  "failed to read temporary chunk-reference spool", file_.path());
        }
        return record;
    }

  private:
    TemporaryOutputFile file_;
    std::unique_ptr<std::ifstream> reader_;
};

static_assert(!std::is_copy_constructible_v<ChunkReferenceSpool>);

struct ProcessedEntry {
    std::variant<ScannedEntry, ScanWarning> value;
    std::optional<ChunkReferenceSpool> chunk_references;
    std::string file_hash_hex;
    std::uint64_t chunk_count{};
    std::uint64_t new_chunk_count{};
};

static_assert(!std::is_copy_constructible_v<ProcessedEntry>);

void saturating_add(std::size_t& total, std::size_t value) noexcept {
    if (value > (std::numeric_limits<std::size_t>::max)() - total) {
        total = (std::numeric_limits<std::size_t>::max)();
    } else {
        total += value;
    }
}

[[nodiscard]] std::size_t processed_entry_size(const ProcessedEntry& result) noexcept {
    std::size_t bytes = sizeof(ProcessedEntry);
    if (const auto* entry = std::get_if<ScannedEntry>(&result.value)) {
        saturating_add(bytes, entry->source_path.native().size() *
                                  sizeof(std::filesystem::path::value_type));
        saturating_add(bytes, entry->relative_path.size());
        saturating_add(bytes, entry->parent_path.size());
        saturating_add(bytes, entry->name.size());
        if (entry->symlink_target.has_value()) {
            saturating_add(bytes, entry->symlink_target->size());
        }
    } else {
        const ScanWarning& warning = std::get<ScanWarning>(result.value);
        saturating_add(bytes, warning.relative_path.size());
        saturating_add(bytes, warning.code.size());
        saturating_add(bytes, warning.message.size());
    }
    saturating_add(bytes, result.file_hash_hex.size());
    return bytes;
}

} // namespace

SnapshotEngine::SnapshotEngine(Repository& repository) : repository_(repository) {}

unsigned SnapshotEngine::default_worker_count() noexcept {
    auto count = std::thread::hardware_concurrency();
    count = count == 0 ? 4 : count;
    return std::clamp(count, 1U, 16U);
}

unsigned SnapshotEngine::resolved_worker_count(std::size_t requested) noexcept {
    if (requested == 0) {
        return default_worker_count();
    }
    return static_cast<unsigned>(std::clamp<std::size_t>(requested, 1U, 16U));
}

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

    ProgressAggregator progress_aggregator(std::move(progress), progress_clock_test_hook_);
    SnapshotTotals totals;
    progress_aggregator.emit_required(OperationPhase::preparing, source);
    RepositoryLock writer_lock =
        RepositoryLock::acquire_exclusive(repository_.root() / "repository.lock");
    repository_.recover_after_writer_lock();
    MetadataStore metadata(repository_.database());
    const std::int64_t started_at_ns = now_ns();
    const SnapshotId snapshot_id =
        metadata.create_pending_snapshot(path_to_utf8(source), options.message, started_at_ns);
    const std::shared_ptr<FailureInjector> failure_injector =
        std::make_shared<SynchronizedFailureInjector>(repository_.failure_injector());

    SnapshotResult result;
    result.snapshot_id = snapshot_id;
    try {
        check_cancelled(stop_token, source);
        progress_aggregator.emit_required(OperationPhase::scanning, source);
        check_cancelled(stop_token, source);
        const ByteCount chunk_size = repository_.info().chunk_size_bytes;
        const int compression_level = repository_.info().zstd_level;
        const std::filesystem::path repository_root = repository_.root();
        const unsigned worker_count = resolved_worker_count(options.worker_count);
        const auto object_synchronization = std::make_shared<ObjectStoreSynchronization>();
        object_synchronization->stripe_test_hook = object_stripe_test_hook_;
        BoundedQueue<FileJob> jobs(job_queue_capacity_);
        BoundedQueue<ProcessedEntry> processed(result_queue_capacity_, result_queue_byte_capacity_,
                                               processed_entry_size);
        std::stop_source pipeline_stop;
        std::mutex first_error_mutex;
        std::exception_ptr first_error;
        std::atomic<unsigned> active_workers{worker_count};

        const auto shutdown = [&](std::exception_ptr error) {
            {
                std::lock_guard lock(first_error_mutex);
                if (!first_error) {
                    first_error = std::move(error);
                }
            }
            pipeline_stop.request_stop();
            jobs.close();
            processed.close();
            progress_aggregator.cancel();
        };
        const auto cancelled_error = [&] {
            return std::make_exception_ptr(
                LocalVaultError(ErrorCode::cancelled, "snapshot cancelled", source));
        };
        const auto require_push = [&](bool pushed, const std::filesystem::path& path,
                                      std::string_view queue_name) {
            if (pushed) {
                return;
            }
            check_cancelled(pipeline_stop.get_token(), path);
            throw LocalVaultError(ErrorCode::internal_error,
                                  std::string(queue_name) + " rejected a pipeline item", path);
        };

        std::jthread scanner_thread;
        std::vector<std::jthread> worker_threads;
        worker_threads.reserve(worker_count);
        std::jthread writer_thread;
        std::jthread progress_thread;
        {
            std::stop_callback external_cancellation(stop_token,
                                                     [&] { shutdown(cancelled_error()); });
            if (!pipeline_stop.stop_requested()) {
                try {
                    progress_thread = std::jthread([&] {
                        try {
                            progress_aggregator.run();
                        } catch (...) {
                            shutdown(std::current_exception());
                        }
                    });

                    writer_thread = std::jthread([&] {
                        try {
                            if (writer_test_hook_) {
                                writer_test_hook_();
                            }
                            ObjectStore objects(repository_root, metadata, chunk_size,
                                                compression_level, failure_injector);
                            auto metadata_batch =
                                std::make_unique<Transaction>(repository_.database());
                            std::size_t batch_record_count = 0;
                            ByteCount batch_logical_bytes = 0;
                            bool batch_has_records = false;
                            const auto commit_batch = [&] {
                                if (!batch_has_records) {
                                    return;
                                }
                                check_cancelled(pipeline_stop.get_token(), source);
                                failure_injector->hit(FailurePoint::before_metadata_batch_commit);
                                metadata_batch->commit();
                                metadata_batch =
                                    std::make_unique<Transaction>(repository_.database());
                                batch_record_count = 0;
                                batch_logical_bytes = 0;
                                batch_has_records = false;
                            };

                            while (std::optional<ProcessedEntry> next =
                                       processed.pop(pipeline_stop.get_token())) {
                                if (auto* warning = std::get_if<ScanWarning>(&next->value)) {
                                    metadata.insert_warning(snapshot_id, warning->relative_path,
                                                            warning->code, warning->message);
                                    result.skipped_entries.push_back(
                                        {path_from_utf8(warning->relative_path), warning->message});
                                    ++batch_record_count;
                                    batch_has_records = true;
                                } else {
                                    ScannedEntry& entry = std::get<ScannedEntry>(next->value);
                                    check_cancelled(pipeline_stop.get_token(), entry.source_path);
                                    const std::int64_t entry_id =
                                        metadata.insert_entry(snapshot_id, entry);
                                    switch (entry.type) {
                                    case EntryType::directory:
                                        ++totals.directory_count;
                                        break;
                                    case EntryType::symbolic_link:
                                        ++totals.symlink_count;
                                        break;
                                    case EntryType::regular_file:
                                        ++totals.file_count;
                                        add_bytes(totals.logical_size, entry.logical_size,
                                                  "logical size");
                                        if (!next->chunk_references.has_value()) {
                                            throw LocalVaultError(
                                                ErrorCode::internal_error,
                                                "regular file result is missing its chunk spool",
                                                entry.source_path);
                                        }
                                        next->chunk_references->rewind_for_reading();
                                        for (std::uint64_t index = 0; index < next->chunk_count;
                                             ++index) {
                                            check_cancelled(pipeline_stop.get_token(),
                                                            entry.source_path);
                                            const ChunkReferenceRecord chunk =
                                                next->chunk_references->read();
                                            const std::string hash_hex(chunk.hash_hex.data(),
                                                                       chunk.hash_hex.size());
                                            StoredObject object{
                                                hash_hex,
                                                ObjectStore::object_relative_path(hash_hex),
                                                chunk.raw_size,
                                                chunk.stored_size,
                                                chunk.newly_stored != 0U,
                                            };
                                            objects.ensure_metadata(object);
                                            metadata.insert_entry_chunk(
                                                entry_id, chunk.sequence_number, object.hash_hex,
                                                chunk.raw_offset, object.raw_size);
                                            if (object.newly_stored) {
                                                ++totals.new_chunk_count;
                                                add_bytes(totals.new_stored_size,
                                                          object.stored_size, "new stored size");
                                            } else {
                                                ++totals.reused_chunk_count;
                                            }
                                        }
                                        metadata.set_regular_file_hash(entry_id,
                                                                       next->file_hash_hex);
                                        break;
                                    }
                                    ++batch_record_count;
                                    add_bytes(batch_logical_bytes, entry.logical_size,
                                              "metadata batch logical size");
                                    batch_has_records = true;
                                }

                                if (batch_record_count >= metadata_batch_entry_limit_ ||
                                    batch_logical_bytes >= metadata_batch_logical_byte_limit_) {
                                    commit_batch();
                                }
                            }
                            if (pipeline_stop.stop_requested()) {
                                return;
                            }
                            commit_batch();
                            metadata_batch.reset();
                        } catch (...) {
                            shutdown(std::current_exception());
                        }
                    });

                    for (unsigned index = 0; index < worker_count; ++index) {
                        worker_threads.emplace_back([&] {
                            try {
                                if (worker_test_hook_) {
                                    worker_test_hook_();
                                }
                                Chunker chunker(chunk_size);
                                ObjectStore objects(repository_root, chunk_size, compression_level,
                                                    failure_injector, object_synchronization);
                                while (std::optional<FileJob> job =
                                           jobs.pop(pipeline_stop.get_token())) {
                                    ProcessedEntry output{
                                        std::move(job->value), std::nullopt, {}, 0, 0};
                                    if (auto* entry = std::get_if<ScannedEntry>(&output.value);
                                        entry != nullptr &&
                                        entry->type == EntryType::regular_file) {
                                        const ScannedEntry original = *entry;
                                        const std::size_t attempt_count =
                                            options.retry_unstable_files ? 2U : 1U;
                                        for (std::size_t attempt = 0; attempt < attempt_count;
                                             ++attempt) {
                                            ByteCount bytes_read = 0;
                                            std::uint64_t sequence = 0;
                                            std::uint64_t new_chunk_count = 0;
                                            Blake3Hasher file_hasher;
                                            ChunkReferenceSpool chunk_references(
                                                repository_root / "temporary" / "objects");
                                            std::exception_ptr callback_error;
                                            try {
                                                check_cancelled(pipeline_stop.get_token(),
                                                                original.source_path);
                                                const PlatformFileMetadata before =
                                                    read_platform_file_metadata_no_follow(
                                                        original.source_path);
                                                chunker.for_each_chunk(
                                                    original.source_path, pipeline_stop.get_token(),
                                                    [&](ByteCount raw_offset,
                                                        std::span<const std::byte> raw_bytes) {
                                                        try {
                                                            file_hasher.update(raw_bytes);
                                                            check_cancelled(
                                                                pipeline_stop.get_token(),
                                                                original.source_path);
                                                            StoredObject object = objects.store(
                                                                raw_bytes,
                                                                pipeline_stop.get_token());
                                                            add_bytes(bytes_read, object.raw_size,
                                                                      "file bytes read");
                                                            chunk_references.append(
                                                                object, raw_offset,
                                                                checked_sequence(sequence));
                                                            if (object.newly_stored) {
                                                                ++new_chunk_count;
                                                            }
                                                            ++sequence;
                                                        } catch (...) {
                                                            callback_error =
                                                                std::current_exception();
                                                            throw;
                                                        }
                                                    });
                                                if (file_read_test_hook_) {
                                                    file_read_test_hook_(original.source_path,
                                                                         attempt);
                                                }
                                                const PlatformFileMetadata after =
                                                    read_platform_file_metadata_no_follow(
                                                        original.source_path);
                                                if (bytes_read != before.logical_size ||
                                                    !before.same_stable_identity(after)) {
                                                    throw LocalVaultError(
                                                        ErrorCode::source_changed,
                                                        "source file changed while reading",
                                                        original.source_path);
                                                }

                                                entry->logical_size = before.logical_size;
                                                entry->modified_time_ns = before.modified_time_ns;
                                                entry->posix_mode = before.posix_mode;
                                                output.chunk_references.emplace(
                                                    std::move(chunk_references));
                                                output.file_hash_hex =
                                                    Blake3Hasher::to_hex(file_hasher.finalize());
                                                output.chunk_count = sequence;
                                                output.new_chunk_count = new_chunk_count;
                                                break;
                                            } catch (const LocalVaultError& error) {
                                                if (callback_error) {
                                                    std::rethrow_exception(callback_error);
                                                }
                                                if (error.code() == ErrorCode::cancelled) {
                                                    throw;
                                                }
                                                if (error.code() != ErrorCode::filesystem_error &&
                                                    error.code() != ErrorCode::source_changed) {
                                                    throw;
                                                }
                                                if (attempt + 1U < attempt_count) {
                                                    continue;
                                                }
                                                output.value = ScanWarning{
                                                    original.relative_path,
                                                    error.code() == ErrorCode::source_changed
                                                        ? "source_changed"
                                                        : "source_unavailable",
                                                    error.what(),
                                                };
                                            }
                                        }
                                    }
                                    const std::filesystem::path path =
                                        std::holds_alternative<ScannedEntry>(output.value)
                                            ? std::get<ScannedEntry>(output.value).source_path
                                            : source;
                                    if (const auto* completed =
                                            std::get_if<ScannedEntry>(&output.value)) {
                                        progress_aggregator.record_processed(
                                            *completed, output.new_chunk_count,
                                            output.chunk_count - output.new_chunk_count);
                                    }
                                    require_push(processed.push(std::move(output),
                                                                pipeline_stop.get_token()),
                                                 path, "processed-entry queue");
                                }
                            } catch (...) {
                                shutdown(std::current_exception());
                            }
                            if (active_workers.fetch_sub(1) == 1) {
                                processed.close();
                            }
                        });
                    }

                    scanner_thread = std::jthread([&] {
                        try {
                            if (scanner_test_hook_) {
                                scanner_test_hook_();
                            }
                            FileScanner{}.scan_streaming(
                                source, pipeline_stop.get_token(),
                                [&](ScannedEntry entry) {
                                    const std::filesystem::path path = entry.source_path;
                                    progress_aggregator.record_discovered(entry);
                                    if (scanner_entry_test_hook_) {
                                        scanner_entry_test_hook_();
                                    }
                                    require_push(jobs.push(FileJob{std::move(entry)},
                                                           pipeline_stop.get_token()),
                                                 path, "file-job queue");
                                },
                                [&](ScanWarning warning) {
                                    require_push(jobs.push(FileJob{std::move(warning)},
                                                           pipeline_stop.get_token()),
                                                 source, "file-job queue");
                                },
                                {
                                    .include_hidden = options.include_hidden,
                                    .one_file_system = options.one_file_system,
                                    .ignore_file = options.ignore_file,
                                });
                            progress_aggregator.mark_scan_complete(source);
                            jobs.close();
                        } catch (...) {
                            shutdown(std::current_exception());
                        }
                    });
                } catch (...) {
                    shutdown(std::current_exception());
                }
            }

            if (scanner_thread.joinable()) {
                scanner_thread.join();
            }
            for (std::jthread& worker : worker_threads) {
                if (worker.joinable()) {
                    worker.join();
                }
            }
            if (writer_thread.joinable()) {
                writer_thread.join();
            }
            progress_aggregator.finish();
            if (progress_thread.joinable()) {
                progress_thread.join();
            }
        }

        {
            std::lock_guard lock(first_error_mutex);
            if (first_error) {
                std::rethrow_exception(first_error);
            }
        }

        check_cancelled(stop_token, source);
        const auto elapsed = std::chrono::nanoseconds(now_ns() - started_at_ns);
        totals.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        progress_aggregator.emit_required(OperationPhase::finalizing, source);
        check_cancelled(stop_token, source);
        {
            Transaction publication(repository_.database());
            failure_injector->hit(FailurePoint::before_snapshot_publish);
            metadata.mark_snapshot_complete(snapshot_id, totals, now_ns());
            publication.commit();
        }
    } catch (const LocalVaultError& error) {
        const SnapshotStatus status = error.code() == ErrorCode::cancelled
                                          ? SnapshotStatus::cancelled
                                          : SnapshotStatus::failed;
        mark_incomplete_best_effort(metadata, snapshot_id, status, error.what(), *failure_injector,
                                    metadata_batch_entry_limit_);
        throw;
    } catch (const std::exception& error) {
        mark_incomplete_best_effort(metadata, snapshot_id, SnapshotStatus::failed, error.what(),
                                    *failure_injector, metadata_batch_entry_limit_);
        throw LocalVaultError(ErrorCode::internal_error,
                              "snapshot failed: " + std::string(error.what()), source);
    } catch (...) {
        mark_incomplete_best_effort(metadata, snapshot_id, SnapshotStatus::failed,
                                    "unknown snapshot failure", *failure_injector,
                                    metadata_batch_entry_limit_);
        throw LocalVaultError(ErrorCode::internal_error,
                              "snapshot failed: unknown snapshot failure", source);
    }

    result.file_count = totals.file_count;
    result.directory_count = totals.directory_count;
    result.logical_bytes = totals.logical_size;
    result.new_stored_bytes = totals.new_stored_size;
    result.new_chunks = totals.new_chunk_count;
    result.reused_chunks = totals.reused_chunk_count;
    try {
        progress_aggregator.emit_required(OperationPhase::complete, source);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "LocalVault: complete progress callback failed: %s\n", error.what());
    } catch (...) {
        std::fprintf(stderr, "LocalVault: complete progress callback failed\n");
    }
    return result;
}

} // namespace localvault
