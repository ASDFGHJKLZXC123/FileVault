#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "database/database.hpp"
#include "database/metadata_store.hpp"
#include "database/statement.hpp"
#include "filesystem/file_scanner.hpp"
#include "filesystem/platform/platform_lock.hpp"
#include "localvault/diff_engine.hpp"
#include "localvault/error.hpp"
#include "localvault/repository.hpp"
#include "localvault/snapshot_engine.hpp"
#include "support/test_filesystem.hpp"

namespace localvault {
namespace {

struct FixtureEntry {
    std::string path;
    EntryType type{EntryType::regular_file};
    char hash{'a'};
    std::optional<std::string> target{};
    std::int64_t mtime{1};
    std::uint32_t mode{0644};
    std::optional<std::int64_t> attributes{};
};

[[nodiscard]] std::string utf8(const std::filesystem::path& path) {
    const auto bytes = path.generic_u8string();
    return {bytes.begin(), bytes.end()};
}

class DiffEngineTest : public ::testing::Test {
  protected:
    DiffEngineTest() {
        Repository::create(root());
        repository_.emplace(Repository::open(root()));
        database_ = std::make_unique<Database>(root() / "repository.db");
    }

    [[nodiscard]] std::filesystem::path root() const {
        return temporary_.path() / "repository";
    }

    [[nodiscard]] SnapshotId snapshot(const std::vector<FixtureEntry>& entries = {}) {
        MetadataStore store(*database_);
        const auto id = store.create_pending_snapshot("/source", "diff fixture", 1);
        for (const auto& input : entries) {
            const auto separator = input.path.rfind('/');
            ScannedEntry entry;
            entry.relative_path = input.path;
            entry.parent_path =
                separator == std::string::npos ? "" : input.path.substr(0, separator);
            entry.name = input.path.substr(separator == std::string::npos ? 0 : separator + 1);
            entry.type = input.type;
            entry.modified_time_ns = input.mtime;
            entry.posix_mode = input.mode;
            entry.symlink_target = input.target;
            const auto entry_id = store.insert_entry(id, entry);
            if (input.type == EntryType::regular_file) {
                store.set_regular_file_hash(entry_id, std::string(64, input.hash));
            }
            if (input.attributes) {
                auto update = database_->statement(
                    "UPDATE entries SET windows_attributes = :attributes WHERE id = :id");
                update.bind(":attributes", *input.attributes);
                update.bind(":id", entry_id);
                update.execute();
            }
        }
        store.mark_snapshot_complete(id, {}, 2);
        return id;
    }

    [[nodiscard]] std::vector<DiffEntry> diff(SnapshotId before, SnapshotId after,
                                              const DiffOptions& options = {}) {
        std::vector<DiffEntry> entries;
        DiffEngine(*repository_).diff(before, after, options, [&](const DiffEntry& entry) {
            entries.push_back(entry);
        });
        return entries;
    }

    test::TemporaryDirectory temporary_;
    std::optional<Repository> repository_;
    std::unique_ptr<Database> database_;
};

TEST_F(DiffEngineTest, ClassifiesAllChangesInSortedStreamingOrder) {
    const auto before = snapshot({{"removed"},
                                  {"unchanged"},
                                  {"file"},
                                  {"type"},
                                  {"link", EntryType::symbolic_link, 'a', "old"},
                                  {"mtime"},
                                  {"mode"},
                                  {"attributes"}});
    const auto after = snapshot({{"unchanged"},
                                 {"added"},
                                 {"file", EntryType::regular_file, 'b'},
                                 {"type", EntryType::directory},
                                 {"link", EntryType::symbolic_link, 'a', "new"},
                                 {"mtime", EntryType::regular_file, 'a', {}, 2},
                                 {"mode", EntryType::regular_file, 'a', {}, 1, 0600},
                                 {"attributes", EntryType::regular_file, 'a', {}, 1, 0644, 2}});
    const auto entries = diff(before, after);
    const std::vector<std::pair<std::string, DiffKind>> expected{
        {"added", DiffKind::added},
        {"attributes", DiffKind::metadata_modified},
        {"file", DiffKind::content_modified},
        {"link", DiffKind::content_modified},
        {"mode", DiffKind::metadata_modified},
        {"mtime", DiffKind::metadata_modified},
        {"removed", DiffKind::removed},
        {"type", DiffKind::type_changed},
        {"unchanged", DiffKind::unchanged}};
    ASSERT_EQ(entries.size(), expected.size());
    for (std::size_t index = 0; index < entries.size(); ++index) {
        EXPECT_EQ(utf8(entries[index].relative_path), expected[index].first);
        EXPECT_EQ(entries[index].kind, expected[index].second);
        EXPECT_EQ(entries[index].before.has_value(), expected[index].second != DiffKind::added);
        EXPECT_EQ(entries[index].after.has_value(), expected[index].second != DiffKind::removed);
        if (entries[index].before) {
            EXPECT_EQ(entries[index].before->snapshot_id, before);
        }
        if (entries[index].after) {
            EXPECT_EQ(entries[index].after->snapshot_id, after);
        }
    }
    const auto content_only = diff(before, after, {.compare_metadata = false});
    EXPECT_EQ(content_only[1].kind, DiffKind::unchanged);
    EXPECT_EQ(content_only[2].kind, DiffKind::content_modified);
    EXPECT_EQ(content_only[4].kind, DiffKind::unchanged);
    EXPECT_EQ(content_only[5].kind, DiffKind::unchanged);
}

TEST_F(DiffEngineTest, SuppressesDirectoryMtimeNoiseButRetainsModeChanges) {
    const auto before = snapshot({{"mtime", EntryType::directory},
                                  {"mode", EntryType::directory},
                                  {"attributes", EntryType::directory}});
    const auto after = snapshot({{"mtime", EntryType::directory, 'a', {}, 2},
                                 {"mode", EntryType::directory, 'a', {}, 1, 0700},
                                 {"attributes", EntryType::directory, 'a', {}, 1, 0644, 2}});
    const auto quiet = diff(before, after);
    ASSERT_EQ(quiet.size(), 3U);
    EXPECT_EQ(quiet[0].kind, DiffKind::unchanged);
    EXPECT_EQ(quiet[1].kind, DiffKind::metadata_modified);
    EXPECT_EQ(quiet[2].kind, DiffKind::unchanged);
    EXPECT_EQ(diff(before, after, {.ignore_directory_mtime = false})[2].kind,
              DiffKind::metadata_modified);
    const auto content_only = diff(before, after, {.compare_metadata = false});
    EXPECT_TRUE(std::ranges::all_of(
        content_only, [](const auto& entry) { return entry.kind == DiffKind::unchanged; }));
}

TEST_F(DiffEngineTest, ComparesLinkTextExactlyAndClassifiesLinkMetadata) {
    const auto before = snapshot({{"text", EntryType::symbolic_link, 'a', "dir//file"},
                                  {"mode", EntryType::symbolic_link, 'a', "same"}});
    const auto after = snapshot({{"text", EntryType::symbolic_link, 'a', "dir/file"},
                                 {"mode", EntryType::symbolic_link, 'a', "same", 1, 0600}});
    const auto entries = diff(before, after);
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries[0].kind, DiffKind::metadata_modified);
    EXPECT_EQ(entries[1].kind, DiffKind::content_modified);
}

TEST_F(DiffEngineTest, EmptySnapshotsAndRootSentinelsProduceNoResults) {
    const auto empty = snapshot();
    const auto rooted = snapshot({{"", EntryType::directory}});
    EXPECT_TRUE(diff(empty, rooted).empty());
    EXPECT_TRUE(diff(rooted, empty).empty());
    EXPECT_TRUE(diff(empty, empty).empty());
    EXPECT_TRUE(DiffEngine(*repository_).diff_page(rooted, rooted).entries.empty());
}

TEST_F(DiffEngineTest, LongAndUnicodePathsFollowSqlByteOrder) {
    std::vector<std::string> names{"z", "a", std::string(16'000, 'x'), "\xEE\x80\x80",
                                   "\xF0\x90\x80\x80"};
    std::vector<FixtureEntry> fixtures;
    for (const auto& name : names) {
        fixtures.push_back({name});
    }
    const auto before = snapshot();
    const auto after = snapshot(fixtures);
    const auto entries = diff(before, after);
    std::ranges::sort(names);
    ASSERT_EQ(entries.size(), names.size());
    for (std::size_t index = 0; index < names.size(); ++index) {
        EXPECT_EQ(utf8(entries[index].relative_path), names[index]);
        EXPECT_EQ(entries[index].kind, DiffKind::added);
    }
}

TEST_F(DiffEngineTest, PagesByExclusiveKeyAndFiltersUnchangedRows) {
    const auto before = snapshot({{"a"}, {"c"}, {"e"}});
    const auto after = snapshot({{"a"}, {"b"}, {"c"}, {"d"}, {"e"}, {"f"}});
    DiffEngine engine(*repository_);
    const DiffOptions changes_only{.include_unchanged = false};
    const auto first = engine.diff_page(before, after, changes_only, 2);
    ASSERT_EQ(first.entries.size(), 2U);
    EXPECT_EQ(utf8(first.entries[0].relative_path), "b");
    EXPECT_EQ(utf8(first.entries[1].relative_path), "d");
    ASSERT_EQ(first.next_cursor, "d");
    const auto second = engine.diff_page(before, after, changes_only, 2, *first.next_cursor);
    ASSERT_EQ(second.entries.size(), 1U);
    EXPECT_EQ(utf8(second.entries[0].relative_path), "f");
    EXPECT_FALSE(second.next_cursor);
    const auto exact_page = engine.diff_page(before, after, {}, 6);
    EXPECT_EQ(exact_page.entries.size(), 6U);
    EXPECT_FALSE(exact_page.next_cursor);
    EXPECT_TRUE(engine.diff_page(before, after, {}, 2, "z").entries.empty());
    EXPECT_THROW((void)engine.diff_page(before, after, {}, 0), LocalVaultError);
}

TEST_F(DiffEngineTest, ReadOnlyDiffDoesNotAcquireWriterLock) {
    const auto before = snapshot();
    const auto after = snapshot({{"file"}});
    repository_.reset();
    repository_.emplace(Repository::open(root(), OpenMode::read_only));
    auto lock = RepositoryLock::acquire_exclusive(root() / "repository.lock");
    const auto entries = diff(before, after);
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries.front().kind, DiffKind::added);
}

TEST_F(DiffEngineTest, RejectsMissingAndEveryIncompleteSnapshotInEitherPosition) {
    const auto complete = snapshot();
    DiffEngine engine(*repository_);
    EXPECT_THROW((void)engine.diff_page(complete, 999'999), LocalVaultError);
    EXPECT_THROW((void)engine.diff_page(-1, complete), LocalVaultError);
    for (const std::string status : {"pending", "failed", "cancelled", "deleting"}) {
        const auto incomplete = snapshot();
        auto update = database_->statement("UPDATE snapshots SET status = :status WHERE id = :id");
        update.bind(":status", status);
        update.bind(":id", incomplete);
        update.execute();
        EXPECT_THROW((void)engine.diff_page(complete, incomplete), LocalVaultError) << status;
        EXPECT_THROW((void)engine.diff_page(incomplete, complete), LocalVaultError) << status;
    }
    EXPECT_THROW(engine.diff(complete, complete, {}, {}), LocalVaultError);
}

TEST_F(DiffEngineTest, CancellationBeforeAndDuringStreamingRollsBackReadTransaction) {
    const auto before = snapshot();
    const auto after = snapshot({{"a"}, {"b"}});
    DiffEngine engine(*repository_);
    std::stop_source stop;
    stop.request_stop();
    try {
        (void)engine.diff_page(before, after, {}, 1, {}, stop.get_token());
        FAIL() << "expected cancellation";
    } catch (const LocalVaultError& error) {
        EXPECT_EQ(error.code(), ErrorCode::cancelled);
    }
    std::stop_source during;
    std::size_t calls = 0;
    try {
        engine.diff(
            before, after, {},
            [&](const DiffEntry&) {
                ++calls;
                during.request_stop();
            },
            during.get_token());
        FAIL() << "expected cancellation";
    } catch (const LocalVaultError& error) {
        EXPECT_EQ(error.code(), ErrorCode::cancelled);
    }
    EXPECT_EQ(calls, 1U);
    EXPECT_EQ(diff(before, after).size(), 2U);
}

TEST_F(DiffEngineTest, CallbackFailurePropagatesAndConnectionRemainsUsable) {
    const auto before = snapshot();
    const auto after = snapshot({{"a"}});
    EXPECT_THROW(DiffEngine(*repository_)
                     .diff(before, after, {},
                           [](const DiffEntry&) { throw std::runtime_error("callback failed"); }),
                 std::runtime_error);
    EXPECT_EQ(diff(before, after).size(), 1U);
}

TEST_F(DiffEngineTest, StreamingUsesOneConsistentViewAcrossConcurrentDeletion) {
    const auto before = snapshot({{"a"}, {"b"}, {"c"}});
    const auto after = snapshot({{"a"}, {"b"}, {"d"}});
    std::vector<std::pair<std::string, DiffKind>> results;
    DiffEngine(*repository_).diff(before, after, {}, [&](const DiffEntry& entry) {
        results.emplace_back(utf8(entry.relative_path), entry.kind);
        if (results.size() == 1) {
            auto remove = database_->statement("DELETE FROM snapshots WHERE id = :id");
            remove.bind(":id", after);
            remove.execute();
        }
    });
    const std::vector<std::pair<std::string, DiffKind>> expected{{"a", DiffKind::unchanged},
                                                                 {"b", DiffKind::unchanged},
                                                                 {"c", DiffKind::removed},
                                                                 {"d", DiffKind::added}};
    EXPECT_EQ(results, expected);
    EXPECT_THROW((void)DiffEngine(*repository_).diff_page(before, after), LocalVaultError);
}

TEST_F(DiffEngineTest, ComparesSnapshotsProducedBySnapshotEngine) {
    const auto source = temporary_.path() / "source";
    test::DatasetBuilder(source).text_file("file", "old").text_file("removed", "removed");
    SnapshotEngine snapshots(*repository_);
    const auto before = snapshots.create_snapshot(source, {}).snapshot_id;
    test::DatasetBuilder(source).text_file("file", "changed").text_file("added", "added");
    ASSERT_TRUE(std::filesystem::remove(source / "removed"));
    const auto after = snapshots.create_snapshot(source, {}).snapshot_id;
    const auto entries = diff(before, after, {.compare_metadata = false});
    ASSERT_EQ(entries.size(), 3U);
    EXPECT_EQ(entries[0].kind, DiffKind::added);
    EXPECT_EQ(entries[1].kind, DiffKind::content_modified);
    EXPECT_EQ(entries[2].kind, DiffKind::removed);
}

} // namespace
} // namespace localvault
