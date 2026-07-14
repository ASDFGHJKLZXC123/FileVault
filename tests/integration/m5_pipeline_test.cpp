#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "database/database.hpp"
#include "database/statement.hpp"
#include "localvault/error.hpp"
#include "localvault/repository.hpp"
#include "localvault/restore_engine.hpp"
#include "localvault/snapshot_engine.hpp"
#include "support/test_filesystem.hpp"

namespace localvault {

class M5PipelineTestAccess final {
  public:
    [[nodiscard]] static unsigned default_workers() {
        return SnapshotEngine::default_worker_count();
    }

    [[nodiscard]] static unsigned workers_for(std::size_t requested) {
        return SnapshotEngine::resolved_worker_count(requested);
    }

    static void set_queue_limits(SnapshotEngine& engine, std::size_t jobs, std::size_t results,
                                 std::size_t result_bytes) {
        engine.job_queue_capacity_ = jobs;
        engine.result_queue_capacity_ = results;
        engine.result_queue_byte_capacity_ = result_bytes;
    }

    static void set_hooks(SnapshotEngine& engine, std::function<void()> scanner,
                          std::function<void()> worker, std::function<void()> writer) {
        engine.scanner_test_hook_ = std::move(scanner);
        engine.worker_test_hook_ = std::move(worker);
        engine.writer_test_hook_ = std::move(writer);
    }

    static void freeze_progress_clock(SnapshotEngine& engine) {
        engine.progress_clock_test_hook_ = [] { return std::chrono::steady_clock::time_point{}; };
    }

    static void set_scan_entry_hook(SnapshotEngine& engine, std::function<void()> hook) {
        engine.scanner_entry_test_hook_ = std::move(hook);
    }

    static void set_progress_clock(SnapshotEngine& engine,
                                   std::function<std::chrono::steady_clock::time_point()> clock) {
        engine.progress_clock_test_hook_ = std::move(clock);
    }

    static void
    set_file_read_hook(SnapshotEngine& engine,
                       std::function<void(const std::filesystem::path&, std::size_t)> hook) {
        engine.file_read_test_hook_ = std::move(hook);
    }
};

namespace {

[[nodiscard]] std::int64_t scalar(Database& database, std::string_view sql) {
    auto query = database.statement(sql);
    EXPECT_TRUE(query.step());
    const std::int64_t value = query.column_int64(0);
    EXPECT_FALSE(query.step());
    return value;
}

void add_files(const std::filesystem::path& source, std::size_t count) {
    test::DatasetBuilder builder(source);
    for (std::size_t index = 0; index < count; ++index) {
        builder.text_file("file-" + std::to_string(index) + ".txt", "shared pipeline bytes");
    }
}

TEST(M5PipelineTest, NormativeWorkerDefaultAndConfiguredClamp) {
    EXPECT_GE(M5PipelineTestAccess::default_workers(), 1U);
    EXPECT_LE(M5PipelineTestAccess::default_workers(), 16U);
    EXPECT_EQ(M5PipelineTestAccess::workers_for(0), M5PipelineTestAccess::default_workers());
    EXPECT_EQ(M5PipelineTestAccess::workers_for(1), 1U);
    EXPECT_EQ(M5PipelineTestAccess::workers_for(16), 16U);
    EXPECT_EQ(M5PipelineTestAccess::workers_for(17), 16U);
    EXPECT_EQ(M5PipelineTestAccess::workers_for((std::numeric_limits<std::size_t>::max)()), 16U);
}

TEST(M5PipelineTest, BoundedRequestedWorkerPipelineRoundTripsWithOneWriterThread) {
    test::TemporaryDirectory temporary;
    const std::filesystem::path source = temporary.path() / "source";
    const std::filesystem::path repository_root = temporary.path() / "repository";
    const std::filesystem::path destination =
        std::filesystem::weakly_canonical(temporary.path()) / "restored";
    add_files(source, 24);
    Repository::create(repository_root);
    Repository repository = Repository::open(repository_root, OpenMode::read_write);
    SnapshotEngine engine(repository);
    M5PipelineTestAccess::set_queue_limits(engine, 1, 1, 1024U * 1024U);

    std::atomic<unsigned> worker_starts{0};
    std::thread::id scanner_thread;
    std::thread::id writer_thread;
    std::mutex worker_ids_mutex;
    std::vector<std::thread::id> worker_threads;
    M5PipelineTestAccess::set_hooks(
        engine, [&] { scanner_thread = std::this_thread::get_id(); },
        [&] {
            worker_starts.fetch_add(1);
            std::lock_guard lock(worker_ids_mutex);
            worker_threads.push_back(std::this_thread::get_id());
        },
        [&] { writer_thread = std::this_thread::get_id(); });

    const SnapshotResult snapshot =
        engine.create_snapshot(source, SnapshotOptions{.worker_count = 3});

    EXPECT_EQ(worker_starts.load(), 3U);
    EXPECT_EQ(worker_threads.size(), 3U);
    EXPECT_NE(scanner_thread, std::thread::id{});
    EXPECT_NE(writer_thread, std::thread::id{});
    EXPECT_NE(scanner_thread, writer_thread);
    EXPECT_NE(writer_thread, std::this_thread::get_id());
    for (const std::thread::id worker : worker_threads) {
        EXPECT_NE(worker, writer_thread);
        EXPECT_NE(worker, scanner_thread);
    }
    EXPECT_EQ(snapshot.file_count, 24U);
    EXPECT_EQ(snapshot.new_chunks, 1U);
    EXPECT_EQ(snapshot.reused_chunks, 23U);

    std::filesystem::create_directory(destination);
    const RestoreResult restored = RestoreEngine(repository)
                                       .restore({
                                           .snapshot_id = snapshot.snapshot_id,
                                           .relative_paths = {},
                                           .destination_root = destination,
                                           .conflict_resolver = {},
                                       });
    EXPECT_EQ(restored.restored_files, 24U);
    EXPECT_EQ(test::read_all_bytes(destination / "file-7.txt"),
              test::read_all_bytes(source / "file-7.txt"));

    Database database(repository_root / "repository.db");
    EXPECT_EQ(scalar(database, "SELECT COUNT(*) FROM chunks"), 1);
    EXPECT_EQ(scalar(database, "SELECT COUNT(*) FROM snapshots WHERE status = 'complete'"), 1);
}

TEST(M5PipelineTest, ChunkReferencesSpoolOutsideTinyResultQueueByteBudget) {
    test::TemporaryDirectory temporary;
    const std::filesystem::path source = temporary.path() / "source";
    const std::filesystem::path repository_root = temporary.path() / "repository";
    const std::filesystem::path destination =
        std::filesystem::weakly_canonical(temporary.path()) / "restored";
    constexpr std::size_t chunk_count = 4096;
    constexpr std::size_t result_byte_budget = 1024;
    test::DatasetBuilder(source).repeated_file("many-chunks.bin", chunk_count, std::byte{0x5a});
    RepositoryCreateOptions create_options;
    create_options.chunk_size_bytes = 1;
    Repository::create(repository_root, create_options);
    Repository repository = Repository::open(repository_root, OpenMode::read_write);
    SnapshotEngine engine(repository);
    M5PipelineTestAccess::set_queue_limits(engine, 1, 1, result_byte_budget);

    const SnapshotResult snapshot =
        engine.create_snapshot(source, SnapshotOptions{.worker_count = 1});

    EXPECT_EQ(snapshot.file_count, 1U);
    EXPECT_EQ(snapshot.new_chunks, 1U);
    EXPECT_EQ(snapshot.reused_chunks, chunk_count - 1U);
    Database database(repository_root / "repository.db");
    EXPECT_EQ(scalar(database, "SELECT COUNT(*) FROM entry_chunks"),
              static_cast<std::int64_t>(chunk_count));
    EXPECT_TRUE(std::filesystem::is_empty(repository_root / "temporary" / "objects"));
    std::filesystem::create_directory(destination);
    const RestoreResult restored = RestoreEngine(repository)
                                       .restore({
                                           .snapshot_id = snapshot.snapshot_id,
                                           .relative_paths = {},
                                           .destination_root = destination,
                                           .conflict_resolver = {},
                                       });
    EXPECT_EQ(restored.restored_files, 1U);
    EXPECT_EQ(test::read_all_bytes(destination / "many-chunks.bin"),
              test::read_all_bytes(source / "many-chunks.bin"));
}

enum class FailingStage { scanner, worker, writer };

class M5PipelineFailureTest : public ::testing::TestWithParam<FailingStage> {};

TEST_P(M5PipelineFailureTest, FirstFailureClosesBothQueuesJoinsAndCleansPartialSnapshot) {
    test::TemporaryDirectory temporary;
    const std::filesystem::path source = temporary.path() / "source";
    const std::filesystem::path repository_root = temporary.path() / "repository";
    add_files(source, 100);
    Repository::create(repository_root);
    Repository repository = Repository::open(repository_root, OpenMode::read_write);
    SnapshotEngine engine(repository);
    M5PipelineTestAccess::set_queue_limits(engine, 1, 1, 1024U * 1024U);
    const FailingStage stage = GetParam();
    const std::string marker = stage == FailingStage::scanner  ? "scanner-first-failure"
                               : stage == FailingStage::worker ? "worker-first-failure"
                                                               : "writer-first-failure";
    const auto fail = [marker] { throw std::runtime_error(marker); };
    M5PipelineTestAccess::set_hooks(engine,
                                    stage == FailingStage::scanner ? fail : std::function<void()>{},
                                    stage == FailingStage::worker ? fail : std::function<void()>{},
                                    stage == FailingStage::writer ? fail : std::function<void()>{});

    try {
        (void)engine.create_snapshot(source, SnapshotOptions{.worker_count = 4});
        FAIL() << "injected pipeline failure unexpectedly succeeded";
    } catch (const LocalVaultError& error) {
        EXPECT_EQ(error.code(), ErrorCode::internal_error);
        EXPECT_NE(std::string_view(error.what()).find(marker), std::string_view::npos);
    }

    Database database(repository_root / "repository.db");
    EXPECT_EQ(scalar(database, "SELECT COUNT(*) FROM snapshots WHERE status = 'failed'"), 1);
    EXPECT_EQ(scalar(database, "SELECT COUNT(*) FROM snapshots WHERE status = 'complete'"), 0);
    EXPECT_EQ(scalar(database, "SELECT COUNT(*) FROM entries"), 0);
    EXPECT_EQ(scalar(database, "SELECT COUNT(*) FROM snapshot_warnings"), 0);
}

INSTANTIATE_TEST_SUITE_P(AllFatalStages, M5PipelineFailureTest,
                         ::testing::Values(FailingStage::scanner, FailingStage::worker,
                                           FailingStage::writer));

TEST(M5PipelineTest, NonStandardWorkerFailureIsStructuredAndCleansPartialSnapshot) {
    test::TemporaryDirectory temporary;
    const std::filesystem::path source = temporary.path() / "source";
    const std::filesystem::path repository_root = temporary.path() / "repository";
    add_files(source, 100);
    Repository::create(repository_root);
    Repository repository = Repository::open(repository_root, OpenMode::read_write);
    SnapshotEngine engine(repository);
    M5PipelineTestAccess::set_queue_limits(engine, 1, 1, 1024U * 1024U);
    M5PipelineTestAccess::set_hooks(engine, {}, [] { throw 7; }, {});

    try {
        (void)engine.create_snapshot(source, SnapshotOptions{.worker_count = 4});
        FAIL() << "non-standard worker failure unexpectedly succeeded";
    } catch (const LocalVaultError& error) {
        EXPECT_EQ(error.code(), ErrorCode::internal_error);
        EXPECT_NE(std::string_view(error.what()).find("unknown snapshot failure"),
                  std::string_view::npos);
    } catch (...) {
        FAIL() << "non-standard worker failure escaped without structured conversion";
    }

    Database database(repository_root / "repository.db");
    EXPECT_EQ(scalar(database, "SELECT COUNT(*) FROM snapshots WHERE status = 'failed'"), 1);
    EXPECT_EQ(scalar(database, "SELECT COUNT(*) FROM snapshots WHERE status = 'complete'"), 0);
    EXPECT_EQ(scalar(database, "SELECT COUNT(*) FROM entries"), 0);
    EXPECT_EQ(scalar(database, "SELECT COUNT(*) FROM snapshot_warnings"), 0);
}

TEST(M5PipelineTest, ExternalCancellationClosesQueuesJoinsAndCleansPartialSnapshot) {
    test::TemporaryDirectory temporary;
    const std::filesystem::path source = temporary.path() / "source";
    const std::filesystem::path repository_root = temporary.path() / "repository";
    add_files(source, 100);
    Repository::create(repository_root);
    Repository repository = Repository::open(repository_root, OpenMode::read_write);
    SnapshotEngine engine(repository);
    M5PipelineTestAccess::set_queue_limits(engine, 1, 1, 1024U * 1024U);
    std::stop_source stop;
    std::atomic<bool> requested{false};
    M5PipelineTestAccess::set_hooks(engine, {},
                                    [&] {
                                        if (!requested.exchange(true)) {
                                            stop.request_stop();
                                        }
                                    },
                                    {});

    try {
        (void)engine.create_snapshot(source, SnapshotOptions{.worker_count = 4}, stop.get_token());
        FAIL() << "cancelled pipeline unexpectedly succeeded";
    } catch (const LocalVaultError& error) {
        EXPECT_EQ(error.code(), ErrorCode::cancelled);
    }

    Database database(repository_root / "repository.db");
    EXPECT_EQ(scalar(database, "SELECT COUNT(*) FROM snapshots WHERE status = 'cancelled'"), 1);
    EXPECT_EQ(scalar(database, "SELECT COUNT(*) FROM snapshots WHERE status = 'complete'"), 0);
    EXPECT_EQ(scalar(database, "SELECT COUNT(*) FROM entries"), 0);
}

TEST(M5PipelineTest,
     DuringScanExternalCancellationAfterObservedHundredEntriesCleansAndPreservesPriorSnapshot) {
    test::TemporaryDirectory temporary;
    const std::filesystem::path source = temporary.path() / "source";
    const std::filesystem::path repository_root = temporary.path() / "repository";
    const std::filesystem::path destination =
        std::filesystem::weakly_canonical(temporary.path()) / "restored";
    test::DatasetBuilder(source).text_file("prior.txt", "prior complete snapshot contents");
    Repository::create(repository_root);
    SnapshotId prior_snapshot{};

    {
        Repository repository = Repository::open(repository_root, OpenMode::read_write);
        prior_snapshot = SnapshotEngine(repository).create_snapshot(source, {}).snapshot_id;
        add_files(source, 200);

        SnapshotEngine engine(repository);
        M5PipelineTestAccess::set_queue_limits(engine, 1, 1, 1024U * 1024U);
        std::stop_source stop;
        std::mutex gate_mutex;
        std::condition_variable gate_changed;
        std::size_t scanner_discovered = 0;
        ProgressEvent cancellation_event;
        std::atomic<std::int64_t> clock_tick{0};
        M5PipelineTestAccess::set_progress_clock(engine, [&] {
            return std::chrono::steady_clock::time_point{
                std::chrono::seconds(clock_tick.fetch_add(2))};
        });
        M5PipelineTestAccess::set_scan_entry_hook(engine, [&] {
            std::unique_lock lock(gate_mutex);
            ++scanner_discovered;
            if (scanner_discovered == 100U) {
                gate_changed.wait(lock, [&] { return stop.stop_requested(); });
            }
        });

        try {
            (void)engine.create_snapshot(source, SnapshotOptions{.worker_count = 4},
                                         stop.get_token(), [&](const ProgressEvent& event) {
                                             if (!event.total_entries.has_value() &&
                                                 event.discovered_entries >= 100U &&
                                                 !stop.stop_requested()) {
                                                 cancellation_event = event;
                                                 stop.request_stop();
                                                 gate_changed.notify_all();
                                             }
                                         });
            FAIL() << "during-scan cancellation unexpectedly succeeded";
        } catch (const LocalVaultError& error) {
            EXPECT_EQ(error.code(), ErrorCode::cancelled);
        }

        EXPECT_GE(cancellation_event.discovered_entries, 100U);
        EXPECT_FALSE(cancellation_event.total_entries.has_value());
        Database database(repository_root / "repository.db");
        auto latest =
            database.statement("SELECT id, status FROM snapshots ORDER BY id DESC LIMIT 1");
        ASSERT_TRUE(latest.step());
        const SnapshotId cancelled_snapshot = latest.column_int64(0);
        EXPECT_EQ(latest.column_text(1), "cancelled");
        EXPECT_NE(cancelled_snapshot, prior_snapshot);
        EXPECT_EQ(
            scalar(database, "SELECT COUNT(*) FROM snapshots WHERE status = 'complete' AND id != " +
                                 std::to_string(prior_snapshot)),
            0);
        EXPECT_EQ(scalar(database, "SELECT COUNT(*) FROM entries WHERE snapshot_id = " +
                                       std::to_string(cancelled_snapshot)),
                  0);
        EXPECT_EQ(scalar(database, "SELECT COUNT(*) FROM snapshot_warnings WHERE snapshot_id = " +
                                       std::to_string(cancelled_snapshot)),
                  0);
        EXPECT_TRUE(std::filesystem::is_empty(repository_root / "temporary" / "objects"));
    }

    Repository reopened = Repository::open(repository_root, OpenMode::read_write);
    std::filesystem::create_directory(destination);
    const RestoreResult restored = RestoreEngine(reopened).restore({
        .snapshot_id = prior_snapshot,
        .relative_paths = {},
        .destination_root = destination,
        .conflict_resolver = {},
    });
    EXPECT_EQ(restored.restored_files, 1U);
    EXPECT_EQ(test::read_all_bytes(destination / "prior.txt"),
              test::read_all_bytes(source / "prior.txt"));
    Database recovered_database(repository_root / "repository.db");
    EXPECT_EQ(scalar(recovered_database, "SELECT COUNT(*) FROM snapshots WHERE status = 'pending'"),
              0);
    EXPECT_EQ(
        scalar(recovered_database, "SELECT COUNT(*) FROM snapshots WHERE status = 'complete'"), 1);
}

TEST(M5PipelineTest, ProgressIsBoundedMonotonicAndPublishesExactTotalsAfterScanning) {
    test::TemporaryDirectory temporary;
    const std::filesystem::path source = temporary.path() / "source";
    const std::filesystem::path repository_root = temporary.path() / "repository";
    add_files(source, 100);
    Repository::create(repository_root);
    Repository repository = Repository::open(repository_root, OpenMode::read_write);
    SnapshotEngine engine(repository);
    M5PipelineTestAccess::set_queue_limits(engine, 1, 1, 1024U * 1024U);
    M5PipelineTestAccess::freeze_progress_clock(engine);
    std::vector<ProgressEvent> events;
    std::mutex callback_thread_mutex;
    std::thread::id writer_thread;
    bool callback_ran_on_writer = false;
    M5PipelineTestAccess::set_hooks(engine, {}, {}, [&] {
        std::lock_guard lock(callback_thread_mutex);
        writer_thread = std::this_thread::get_id();
    });

    const SnapshotResult snapshot = engine.create_snapshot(
        source, SnapshotOptions{.worker_count = 4}, {}, [&](const ProgressEvent& event) {
            events.push_back(event);
            if (event.phase == OperationPhase::writing_metadata) {
                std::lock_guard lock(callback_thread_mutex);
                callback_ran_on_writer = writer_thread == std::this_thread::get_id();
            }
        });

    ASSERT_FALSE(events.empty());
    EXPECT_NE(writer_thread, std::thread::id{});
    EXPECT_FALSE(callback_ran_on_writer);
    EXPECT_LE(events.size(), 10U);
    EXPECT_EQ(events.back().phase, OperationPhase::complete);
    const std::uint64_t expected_entries = snapshot.file_count + snapshot.directory_count;
    bool totals_arrived = false;
    for (std::size_t index = 0; index < events.size(); ++index) {
        const ProgressEvent& event = events[index];
        if (index != 0) {
            EXPECT_LE(events[index - 1].discovered_entries, event.discovered_entries);
            EXPECT_LE(events[index - 1].processed_entries, event.processed_entries);
            EXPECT_LE(events[index - 1].processed_bytes, event.processed_bytes);
            EXPECT_LE(events[index - 1].new_chunks, event.new_chunks);
            EXPECT_LE(events[index - 1].reused_chunks, event.reused_chunks);
        }
        if (!totals_arrived && event.total_entries.has_value()) {
            totals_arrived = true;
        }
        if (totals_arrived) {
            ASSERT_TRUE(event.total_entries.has_value());
            ASSERT_TRUE(event.total_bytes.has_value());
            EXPECT_EQ(*event.total_entries, expected_entries);
            EXPECT_EQ(*event.total_bytes, snapshot.logical_bytes);
        } else {
            EXPECT_FALSE(event.total_entries.has_value());
            EXPECT_FALSE(event.total_bytes.has_value());
        }
    }
    EXPECT_TRUE(totals_arrived);
    EXPECT_EQ(events.back().discovered_entries, expected_entries);
    EXPECT_EQ(events.back().processed_entries, expected_entries);
    EXPECT_EQ(events.back().processed_bytes, snapshot.logical_bytes);
    EXPECT_EQ(events.back().new_chunks, snapshot.new_chunks);
    EXPECT_EQ(events.back().reused_chunks, snapshot.reused_chunks);
}

TEST(M5PipelineTest, ProgressCallbackFailureStopsPipelineAndLeavesSnapshotIncomplete) {
    test::TemporaryDirectory temporary;
    const std::filesystem::path source = temporary.path() / "source";
    const std::filesystem::path repository_root = temporary.path() / "repository";
    add_files(source, 100);
    Repository::create(repository_root);
    Repository repository = Repository::open(repository_root, OpenMode::read_write);
    SnapshotEngine engine(repository);
    M5PipelineTestAccess::set_queue_limits(engine, 1, 1, 1024U * 1024U);

    try {
        (void)engine.create_snapshot(
            source, SnapshotOptions{.worker_count = 4}, {}, [](const ProgressEvent& event) {
                if (event.phase == OperationPhase::writing_metadata) {
                    throw std::runtime_error("injected pipeline progress failure");
                }
            });
        FAIL() << "progress callback failure unexpectedly succeeded";
    } catch (const LocalVaultError& error) {
        EXPECT_EQ(error.code(), ErrorCode::internal_error);
        EXPECT_NE(std::string_view(error.what()).find("injected pipeline progress failure"),
                  std::string_view::npos);
    }

    Database database(repository_root / "repository.db");
    EXPECT_EQ(scalar(database, "SELECT COUNT(*) FROM snapshots WHERE status = 'failed'"), 1);
    EXPECT_EQ(scalar(database, "SELECT COUNT(*) FROM snapshots WHERE status = 'complete'"), 0);
    EXPECT_EQ(scalar(database, "SELECT COUNT(*) FROM entries"), 0);
}

TEST(M5PipelineTest, CompleteProgressCallbackFailureCannotRetractPublishedSnapshot) {
    test::TemporaryDirectory temporary;
    const std::filesystem::path source = temporary.path() / "source";
    const std::filesystem::path repository_root = temporary.path() / "repository";
    add_files(source, 10);
    Repository::create(repository_root);
    Repository repository = Repository::open(repository_root, OpenMode::read_write);

    const SnapshotResult snapshot =
        SnapshotEngine(repository).create_snapshot(source, {}, {}, [](const ProgressEvent& event) {
            if (event.phase == OperationPhase::complete) {
                throw std::runtime_error("injected complete callback failure");
            }
        });

    EXPECT_EQ(snapshot.file_count, 10U);

    Database database(repository_root / "repository.db");
    EXPECT_EQ(scalar(database, "SELECT COUNT(*) FROM snapshots WHERE status = 'failed'"), 0);
    EXPECT_EQ(scalar(database, "SELECT COUNT(*) FROM snapshots WHERE status = 'complete'"), 1);
    EXPECT_EQ(scalar(database, "SELECT COUNT(*) FROM entries"), 11);
    EXPECT_EQ(scalar(database, "SELECT COUNT(*) FROM snapshot_warnings"), 0);
}

TEST(M5PipelineTest, UnstableWarningDoesNotIncrementProcessedOrChunkCounters) {
    test::TemporaryDirectory temporary;
    const std::filesystem::path source = temporary.path() / "source";
    const std::filesystem::path repository_root = temporary.path() / "repository";
    test::DatasetBuilder(source).text_file("changing.txt", "first");
    Repository::create(repository_root);
    Repository repository = Repository::open(repository_root, OpenMode::read_write);
    SnapshotEngine engine(repository);
    M5PipelineTestAccess::set_file_read_hook(
        engine, [](const std::filesystem::path& path, std::size_t attempt) {
            std::ofstream(path, std::ios::binary | std::ios::trunc)
                << (attempt == 0U ? "changed once" : "changed twice and still unstable");
        });
    ProgressEvent completion;

    const SnapshotResult snapshot = engine.create_snapshot(
        source, SnapshotOptions{.worker_count = 1}, {}, [&](const ProgressEvent& event) {
            if (event.phase == OperationPhase::complete) {
                completion = event;
            }
        });

    ASSERT_EQ(snapshot.skipped_entries.size(), 1U);
    EXPECT_EQ(snapshot.file_count, 0U);
    EXPECT_EQ(completion.phase, OperationPhase::complete);
    EXPECT_EQ(completion.discovered_entries, 2U);
    EXPECT_EQ(completion.processed_entries, 1U);
    EXPECT_EQ(completion.processed_bytes, 0U);
    EXPECT_EQ(completion.new_chunks, 0U);
    EXPECT_EQ(completion.reused_chunks, 0U);
    ASSERT_TRUE(completion.total_entries.has_value());
    EXPECT_EQ(*completion.total_entries, 2U);
}

TEST(M5PipelineTest, CancellationFromFinalizingCallbackPreventsPublication) {
    test::TemporaryDirectory temporary;
    const std::filesystem::path source = temporary.path() / "source";
    const std::filesystem::path repository_root = temporary.path() / "repository";
    add_files(source, 10);
    Repository::create(repository_root);
    Repository repository = Repository::open(repository_root, OpenMode::read_write);
    SnapshotEngine engine(repository);
    std::stop_source stop;

    try {
        (void)engine.create_snapshot(source, {}, stop.get_token(), [&](const ProgressEvent& event) {
            if (event.phase == OperationPhase::finalizing) {
                stop.request_stop();
            }
        });
        FAIL() << "finalizing cancellation unexpectedly published the snapshot";
    } catch (const LocalVaultError& error) {
        EXPECT_EQ(error.code(), ErrorCode::cancelled);
    }

    Database database(repository_root / "repository.db");
    EXPECT_EQ(scalar(database, "SELECT COUNT(*) FROM snapshots WHERE status = 'cancelled'"), 1);
    EXPECT_EQ(scalar(database, "SELECT COUNT(*) FROM snapshots WHERE status = 'complete'"), 0);
    EXPECT_EQ(scalar(database, "SELECT COUNT(*) FROM entries"), 0);
}

} // namespace
} // namespace localvault
