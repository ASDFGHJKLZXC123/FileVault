#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "database/database.hpp"
#include "database/metadata_store.hpp"
#include "database/statement.hpp"
#include "filesystem/platform/platform_lock.hpp"
#include "localvault/error.hpp"
#include "localvault/repository.hpp"
#include "localvault/restore_engine.hpp"
#include "localvault/snapshot_engine.hpp"
#include "support/test_filesystem.hpp"

namespace localvault {
namespace {

constexpr int crash_helper_exit_code = 86;

struct ChildResult {
    bool launched{false};
    int exit_code{-1};
    std::string error;
};

[[nodiscard]] std::string_view failure_point_name(FailurePoint point) {
    switch (point) {
    case FailurePoint::after_temp_object_write:
        return "after_temp_object_write";
    case FailurePoint::after_object_fsync:
        return "after_object_fsync";
    case FailurePoint::after_object_rename:
        return "after_object_rename";
    case FailurePoint::before_metadata_batch_commit:
        return "before_metadata_batch_commit";
    case FailurePoint::before_snapshot_publish:
        return "before_snapshot_publish";
    case FailurePoint::during_restore_write:
        break;
    }
    throw std::runtime_error("restore failure point is not a snapshot crash point");
}

#ifdef _WIN32
[[nodiscard]] std::wstring quote_windows_argument(const std::wstring& argument) {
    std::wstring result{L'"'};
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'"') {
            result.append(backslashes * 2U + 1U, L'\\');
            result.push_back(L'"');
            backslashes = 0;
        } else {
            result.append(backslashes, L'\\');
            result.push_back(character);
            backslashes = 0;
        }
    }
    result.append(backslashes * 2U, L'\\');
    result.push_back(L'"');
    return result;
}

[[nodiscard]] ChildResult run_crash_helper(FailurePoint point,
                                           const std::filesystem::path& repository_root,
                                           const std::filesystem::path& source) {
    const std::filesystem::path helper_path{LOCALVAULT_M4_CRASH_HELPER_PATH};
    const std::string_view point_name = failure_point_name(point);
    const std::wstring wide_point_name(point_name.begin(), point_name.end());
    std::wstring command_line = quote_windows_argument(helper_path.wstring()) + L" " +
                                quote_windows_argument(wide_point_name) + L" " +
                                quote_windows_argument(repository_root.wstring()) + L" " +
                                quote_windows_argument(source.wstring());

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    PROCESS_INFORMATION process_info{};
    if (::CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                         &startup_info, &process_info) == 0) {
        return {false, -1, "CreateProcessW failed with error " + std::to_string(::GetLastError())};
    }

    const DWORD wait_result = ::WaitForSingleObject(process_info.hProcess, 20'000);
    DWORD exit_code = 0;
    std::string error;
    if (wait_result == WAIT_TIMEOUT) {
        error = "crash helper timed out";
        (void)::TerminateProcess(process_info.hProcess, 125);
        (void)::WaitForSingleObject(process_info.hProcess, INFINITE);
    } else if (wait_result != WAIT_OBJECT_0) {
        error = "waiting for crash helper failed with result " + std::to_string(wait_result) +
                " and error " + std::to_string(::GetLastError());
        (void)::TerminateProcess(process_info.hProcess, 125);
        (void)::WaitForSingleObject(process_info.hProcess, INFINITE);
    } else if (::GetExitCodeProcess(process_info.hProcess, &exit_code) == 0) {
        error = "GetExitCodeProcess failed with error " + std::to_string(::GetLastError());
    }
    (void)::CloseHandle(process_info.hThread);
    (void)::CloseHandle(process_info.hProcess);
    return {true, error.empty() ? static_cast<int>(exit_code) : -1, std::move(error)};
}
#else
[[nodiscard]] ChildResult run_crash_helper(FailurePoint point,
                                           const std::filesystem::path& repository_root,
                                           const std::filesystem::path& source) {
    const std::string point_name(failure_point_name(point));
    const pid_t child = ::fork();
    if (child == -1) {
        return {false, -1, "fork failed: " + std::generic_category().message(errno)};
    }
    if (child == 0) {
        (void)::alarm(20);
        ::execl(LOCALVAULT_M4_CRASH_HELPER_PATH, LOCALVAULT_M4_CRASH_HELPER_PATH,
                point_name.c_str(), repository_root.c_str(), source.c_str(),
                static_cast<char*>(nullptr));
        _exit(127);
    }

    int status = 0;
    pid_t wait_result = 0;
    do {
        wait_result = ::waitpid(child, &status, 0);
    } while (wait_result == -1 && errno == EINTR);
    if (wait_result == -1) {
        return {true, -1, "waitpid failed: " + std::generic_category().message(errno)};
    }
    if (!WIFEXITED(status)) {
        if (WIFSIGNALED(status)) {
            if (WTERMSIG(status) == SIGALRM) {
                return {true, -1, "crash helper timed out"};
            }
            return {true, -1,
                    "crash helper terminated by signal " + std::to_string(WTERMSIG(status))};
        }
        return {true, -1, "crash helper terminated without a normal exit"};
    }
    if (WEXITSTATUS(status) == 127) {
        return {false, -1, "crash helper executable could not be launched (exit 127)"};
    }
    return {true, WEXITSTATUS(status), {}};
}
#endif

class ThrowAtFailurePoint final : public FailureInjector {
  public:
    explicit ThrowAtFailurePoint(FailurePoint target) : target_(target) {}

    void hit(FailurePoint point) override {
        if (point == target_) {
            throw std::runtime_error("injected M4 crash point");
        }
    }

  private:
    FailurePoint target_;
};

class CountingFailureInjector final : public FailureInjector {
  public:
    void hit(FailurePoint point) override {
        hits_.insert(point);
    }

    [[nodiscard]] bool saw(FailurePoint point) const {
        return hits_.contains(point);
    }

  private:
    std::set<FailurePoint> hits_;
};

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

[[nodiscard]] std::int64_t scalar_int(Database& database, std::string_view sql) {
    auto query = database.statement(sql);
    if (!query.step()) {
        throw std::runtime_error("test scalar query returned no row");
    }
    const std::int64_t value = query.column_int64(0);
    if (query.step()) {
        throw std::runtime_error("test scalar query returned multiple rows");
    }
    return value;
}

[[nodiscard]] std::vector<std::filesystem::path>
regular_files_below(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files);
    return files;
}

[[nodiscard]] std::vector<std::filesystem::path>
object_paths_for_snapshot(Database& database, SnapshotId snapshot_id,
                          const std::filesystem::path& repository_root) {
    auto query = database.statement("SELECT DISTINCT c.object_path FROM chunks AS c "
                                    "JOIN entry_chunks AS ec ON ec.chunk_hash = c.hash "
                                    "JOIN entries AS e ON e.id = ec.entry_id "
                                    "WHERE e.snapshot_id = :snapshot_id ORDER BY c.object_path");
    query.bind(":snapshot_id", snapshot_id);
    std::vector<std::filesystem::path> paths;
    while (query.step()) {
        paths.push_back(repository_root / query.column_text(0));
    }
    return paths;
}

void expect_repository_temporary_tree_empty(const std::filesystem::path& repository_root) {
    EXPECT_TRUE(std::filesystem::is_empty(repository_root / "temporary" / "objects"));
    EXPECT_TRUE(std::filesystem::is_empty(repository_root / "temporary" / "restores"));
    EXPECT_EQ(regular_files_below(repository_root / "temporary").size(), 0U);
}

TEST(M4CrashSafety, ProcessExitAtSnapshotFailurePointsRecoversCommittedState) {
    // An abrupt restore exit can leave its atomic temp beside an arbitrary external destination;
    // repository recovery cannot safely discover that path. The throwing matrix below therefore
    // retains the during_restore_write cleanup assertion while true process exits cover all five
    // repository-owned snapshot/publication points.
    constexpr std::array snapshot_failure_points{
        FailurePoint::after_temp_object_write, FailurePoint::after_object_fsync,
        FailurePoint::after_object_rename,     FailurePoint::before_metadata_batch_commit,
        FailurePoint::before_snapshot_publish,
    };

    const std::string old_bytes = "prior complete snapshot survives process exit\n";

    for (std::size_t index = 0; index < snapshot_failure_points.size(); ++index) {
        const FailurePoint point = snapshot_failure_points[index];
        SCOPED_TRACE(failure_point_name(point));
        test::TemporaryDirectory temporary;
        const std::filesystem::path repository_root = temporary.path() / "repository";
        const std::filesystem::path old_source = temporary.path() / "old-source";
        const std::filesystem::path source =
            temporary.path() / ("interrupted-source-" + std::to_string(index));
        const std::filesystem::path destination =
            std::filesystem::weakly_canonical(temporary.path()) /
            ("restored-old-" + std::to_string(index));
        test::DatasetBuilder(old_source).text_file("old.txt", old_bytes);
        test::DatasetBuilder(source).text_file("new.txt",
                                               "unique interrupted bytes " + std::to_string(index));
        Repository::create(repository_root);

        SnapshotId old_snapshot_id{};
        std::vector<std::filesystem::path> old_object_paths;
        {
            Repository repository = Repository::open(repository_root, OpenMode::read_write);
            old_snapshot_id =
                SnapshotEngine(repository).create_snapshot(old_source, {}).snapshot_id;
            Database database(repository_root / "repository.db");
            old_object_paths =
                object_paths_for_snapshot(database, old_snapshot_id, repository_root);
        }
        ASSERT_FALSE(old_object_paths.empty());

        const ChildResult child = run_crash_helper(point, repository_root, source);
        ASSERT_TRUE(child.launched) << child.error;
        ASSERT_TRUE(child.error.empty()) << child.error;
        ASSERT_EQ(child.exit_code, crash_helper_exit_code)
            << "the helper must terminate from its selected std::_Exit injection";

        SnapshotId interrupted_snapshot_id{};
        {
            Database database(repository_root / "repository.db");
            auto pending = database.statement(
                "SELECT id FROM snapshots WHERE id > :old_snapshot_id AND status = 'pending' "
                "ORDER BY id DESC LIMIT 1");
            pending.bind(":old_snapshot_id", old_snapshot_id);
            ASSERT_TRUE(pending.step());
            interrupted_snapshot_id = pending.column_int64(0);
            ASSERT_FALSE(pending.step());
            EXPECT_EQ(
                scalar_int(database, "SELECT COUNT(*) FROM snapshots WHERE status = 'complete'"),
                1);

            auto entry_count =
                database.statement("SELECT COUNT(*) FROM entries WHERE snapshot_id = :snapshot_id");
            entry_count.bind(":snapshot_id", interrupted_snapshot_id);
            ASSERT_TRUE(entry_count.step());
            if (point == FailurePoint::before_snapshot_publish) {
                EXPECT_GT(entry_count.column_int64(0), 0);
            } else {
                EXPECT_EQ(entry_count.column_int64(0), 0);
            }
        }

        {
            Repository reopened = Repository::open(repository_root, OpenMode::read_write);
            const RestoreResult result = RestoreEngine(reopened).restore({
                .snapshot_id = old_snapshot_id,
                .relative_paths = {},
                .destination_root = destination,
                .conflict_resolver = {},
            });
            EXPECT_EQ(result.restored_files, 1U);
        }

        EXPECT_EQ(read_text(destination / "old.txt"), old_bytes);
        expect_repository_temporary_tree_empty(repository_root);
        for (const std::filesystem::path& object_path : old_object_paths) {
            EXPECT_TRUE(std::filesystem::is_regular_file(object_path));
        }

        Database database(repository_root / "repository.db");
        MetadataStore metadata(database);
        auto recovered = database.statement("SELECT status FROM snapshots WHERE id = :snapshot_id");
        recovered.bind(":snapshot_id", interrupted_snapshot_id);
        ASSERT_TRUE(recovered.step());
        EXPECT_EQ(recovered.column_text(0), "failed");
        ASSERT_FALSE(recovered.step());
        auto residue = database.statement(
            "SELECT (SELECT COUNT(*) FROM entries WHERE snapshot_id = :snapshot_id) + "
            "(SELECT COUNT(*) FROM snapshot_warnings WHERE snapshot_id = :snapshot_id)");
        residue.bind(":snapshot_id", interrupted_snapshot_id);
        ASSERT_TRUE(residue.step());
        EXPECT_EQ(residue.column_int64(0), 0);
        EXPECT_NO_THROW(metadata.quick_relationship_check());
    }
}

TEST(M4CrashSafety, EveryFailurePointPreservesPriorSnapshotAndCleansTemporaryResidue) {
    for (const FailurePoint point : all_failure_points) {
        SCOPED_TRACE(static_cast<int>(point));
        test::TemporaryDirectory temporary;
        const std::filesystem::path repository_root = temporary.path() / "repository";
        const std::filesystem::path old_source = temporary.path() / "old-source";
        const std::filesystem::path new_source = temporary.path() / "new-source";
        const std::filesystem::path destination =
            std::filesystem::weakly_canonical(temporary.path()) / "destination";
        const std::string old_bytes = "prior complete snapshot bytes\n";
        test::DatasetBuilder(old_source).text_file("old.txt", old_bytes);
        test::DatasetBuilder(new_source)
            .text_file("new.txt", "new object bytes for failure point " +
                                      std::to_string(static_cast<int>(point)));
        Repository::create(repository_root);

        SnapshotId old_snapshot_id{};
        std::vector<std::filesystem::path> old_object_paths;
        {
            Repository repository = Repository::open(repository_root, OpenMode::read_write);
            old_snapshot_id =
                SnapshotEngine(repository).create_snapshot(old_source, {}).snapshot_id;
            Database database(repository_root / "repository.db");
            old_object_paths =
                object_paths_for_snapshot(database, old_snapshot_id, repository_root);
            ASSERT_FALSE(old_object_paths.empty());

            repository.set_failure_injector(std::make_shared<ThrowAtFailurePoint>(point));
            if (point == FailurePoint::during_restore_write) {
                EXPECT_THROW((void)RestoreEngine(repository)
                                 .restore({
                                     .snapshot_id = old_snapshot_id,
                                     .relative_paths = {},
                                     .destination_root = destination,
                                     .conflict_resolver = {},
                                 }),
                             std::runtime_error);
                ASSERT_TRUE(std::filesystem::is_directory(destination));
                EXPECT_TRUE(std::filesystem::is_empty(destination));
            } else {
                EXPECT_THROW((void)SnapshotEngine(repository).create_snapshot(new_source, {}),
                             LocalVaultError);
            }
        }

        {
            Database database(repository_root / "repository.db");
            auto invisible =
                database.statement("SELECT COUNT(*) FROM snapshots WHERE id > :old_snapshot_id "
                                   "AND status = 'complete'");
            invisible.bind(":old_snapshot_id", old_snapshot_id);
            ASSERT_TRUE(invisible.step());
            EXPECT_EQ(invisible.column_int64(0), 0);
        }

        std::error_code ignored;
        std::filesystem::remove_all(destination, ignored);
        {
            Repository reopened = Repository::open(repository_root, OpenMode::read_write);
            const RestoreResult result = RestoreEngine(reopened).restore({
                .snapshot_id = old_snapshot_id,
                .relative_paths = {},
                .destination_root = destination,
                .conflict_resolver = {},
            });
            EXPECT_EQ(result.restored_files, 1U);
        }

        EXPECT_EQ(read_text(destination / "old.txt"), old_bytes);
        EXPECT_EQ(regular_files_below(destination),
                  (std::vector<std::filesystem::path>{destination / "old.txt"}));
        expect_repository_temporary_tree_empty(repository_root);
        for (const std::filesystem::path& old_object_path : old_object_paths) {
            EXPECT_TRUE(std::filesystem::is_regular_file(old_object_path));
        }

        Database database(repository_root / "repository.db");
        MetadataStore metadata(database);
        EXPECT_NO_THROW(metadata.quick_relationship_check());
        EXPECT_EQ(scalar_int(database, "SELECT COUNT(*) FROM snapshots WHERE status = 'complete'"),
                  1);
        EXPECT_EQ(scalar_int(database, "SELECT COUNT(*) FROM entries AS e JOIN snapshots AS s "
                                       "ON s.id = e.snapshot_id WHERE s.status <> 'complete'"),
                  0);
        auto failed = database.statement(
            "SELECT COUNT(*) FROM snapshots WHERE id > :old_snapshot_id AND status = 'failed'");
        failed.bind(":old_snapshot_id", old_snapshot_id);
        ASSERT_TRUE(failed.step());
        EXPECT_EQ(failed.column_int64(0), point == FailurePoint::during_restore_write ? 0 : 1);
    }
}

TEST(M4CrashSafety, ActualSnapshotRestoreAndDeletingLifecycleHitsEveryFailurePoint) {
    test::TemporaryDirectory temporary;
    const std::filesystem::path repository_root = temporary.path() / "repository";
    const std::filesystem::path source = temporary.path() / "source";
    const std::filesystem::path destination =
        std::filesystem::weakly_canonical(temporary.path()) / "destination";
    test::DatasetBuilder(source).text_file("file.txt", "count every M4 failure point");
    Repository::create(repository_root);
    Repository repository = Repository::open(repository_root, OpenMode::read_write);
    const auto injector = std::make_shared<CountingFailureInjector>();
    repository.set_failure_injector(injector);

    const SnapshotId snapshot_id =
        SnapshotEngine(repository).create_snapshot(source, {}).snapshot_id;
    (void)RestoreEngine(repository)
        .restore({
            .snapshot_id = snapshot_id,
            .relative_paths = {},
            .destination_root = destination,
            .conflict_resolver = {},
        });
    {
        RepositoryLock writer_lock =
            RepositoryLock::acquire_exclusive(repository_root / "repository.lock");
        Database database(repository_root / "repository.db");
        MetadataStore metadata(database);
        metadata.transition_snapshot_to_deleting(snapshot_id, *injector);
        metadata.delete_deleting_snapshot(snapshot_id, *injector, 1);
        (void)writer_lock;
    }

    for (const FailurePoint point : all_failure_points) {
        EXPECT_TRUE(injector->saw(point)) << static_cast<int>(point);
    }
}

TEST(M4CrashSafety, RenameBeforeChunkMetadataLeavesRecoverableUnreferencedOrphan) {
    test::TemporaryDirectory temporary;
    const std::filesystem::path repository_root = temporary.path() / "repository";
    const std::filesystem::path source = temporary.path() / "source";
    const std::filesystem::path recovery_source = temporary.path() / "recovery-source";
    test::DatasetBuilder(source).text_file("orphan.txt", "durable orphan object bytes");
    std::filesystem::create_directory(recovery_source);
    Repository::create(repository_root);

    {
        Repository repository = Repository::open(repository_root, OpenMode::read_write);
        repository.set_failure_injector(
            std::make_shared<ThrowAtFailurePoint>(FailurePoint::after_object_rename));
        EXPECT_THROW((void)SnapshotEngine(repository).create_snapshot(source, {}), LocalVaultError);
    }

    const std::vector<std::filesystem::path> orphan_objects =
        regular_files_below(repository_root / "objects");
    ASSERT_EQ(orphan_objects.size(), 1U);
    EXPECT_EQ(orphan_objects.front().extension(), ".zst");
    {
        Database database(repository_root / "repository.db");
        EXPECT_EQ(scalar_int(database, "SELECT COUNT(*) FROM chunks"), 0);
    }

    {
        Repository reopened = Repository::open(repository_root, OpenMode::read_write);
        EXPECT_NO_THROW((void)SnapshotEngine(reopened).create_snapshot(recovery_source, {}));
    }

    EXPECT_TRUE(std::filesystem::is_regular_file(orphan_objects.front()));
    expect_repository_temporary_tree_empty(repository_root);
    Database database(repository_root / "repository.db");
    MetadataStore metadata(database);
    EXPECT_EQ(scalar_int(database, "SELECT COUNT(*) FROM chunks"), 0);
    EXPECT_NO_THROW(metadata.quick_relationship_check());
}

} // namespace
} // namespace localvault
