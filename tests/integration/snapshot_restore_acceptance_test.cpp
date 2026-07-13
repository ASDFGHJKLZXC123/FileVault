#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "database/database.hpp"
#include "database/metadata_store.hpp"
#include "localvault/repository.hpp"
#include "localvault/restore_engine.hpp"
#include "localvault/snapshot_engine.hpp"
#include "support/test_filesystem.hpp"

namespace localvault {
namespace {

[[nodiscard]] std::string path_utf8(const std::filesystem::path& path) {
    const std::u8string encoded = path.generic_u8string();
    std::string result;
    result.reserve(encoded.size());
    for (const char8_t character : encoded) {
        result.push_back(static_cast<char>(character));
    }
    return result;
}

[[nodiscard]] const EntryInfo* find_entry(const std::vector<EntryInfo>& entries,
                                          std::string_view relative_path) {
    const auto entry = std::ranges::find(entries, relative_path, [](const EntryInfo& value) {
        return path_utf8(value.relative_path);
    });
    return entry == entries.end() ? nullptr : &*entry;
}

TEST(SnapshotRestoreAcceptanceTest, RestoresDeletedFixtureIntoNonemptyAlternateDestination) {
    constexpr std::size_t spanning_file_size = 21;
    test::TemporaryDirectory temporary;
    const std::filesystem::path source = temporary.path() / "source fixture";
    const std::filesystem::path payload = source / "snapshot subtree";
    const std::filesystem::path expected = temporary.path() / "preserved expected";
    const std::filesystem::path repository_root = temporary.path() / "repository";
    const std::filesystem::path destination = temporary.path() / "alternate destination";
    const std::string nfc_name = "nfc-caf\xC3\xA9.txt";
    const std::string nfd_name = "nfd-cafe\xCC\x81.txt";

    test::DatasetBuilder(source)
        .directory("snapshot subtree/empty directory")
        .directory("snapshot subtree/nested/deeper")
        .text_file("snapshot subtree/empty file", "")
        .text_file("snapshot subtree/nested/deeper/text.txt", "small text\n")
        .text_file("snapshot subtree/file with spaces.txt", "spaces")
        .text_file("snapshot subtree/.hidden", "hidden")
        .text_file("snapshot subtree/" + nfc_name, "nfc")
        .text_file("snapshot subtree/" + nfd_name, "nfd")
        .repeated_file("snapshot subtree/nested/spanning.bin", spanning_file_size, std::byte{0xA5});

    bool symlink_created = false;
    try {
        test::DatasetBuilder(source).symlink("snapshot subtree/broken link", "missing target");
        symlink_created = true;
    } catch (const std::filesystem::filesystem_error&) {
#ifndef _WIN32
        throw;
#endif
    }

    std::string on_disk_nfc_name;
    std::string on_disk_nfd_name;
    for (const auto& entry : std::filesystem::directory_iterator(payload)) {
        const std::string name = path_utf8(entry.path().filename());
        if (name.starts_with("nfc-")) {
            on_disk_nfc_name = name;
        } else if (name.starts_with("nfd-")) {
            on_disk_nfd_name = name;
        }
    }
    ASSERT_FALSE(on_disk_nfc_name.empty());
    ASSERT_FALSE(on_disk_nfd_name.empty());
    ASSERT_NE(on_disk_nfc_name, on_disk_nfd_name);

    const auto fixture_time = std::chrono::file_clock::now() - std::chrono::hours{24};
    std::filesystem::last_write_time(payload / "nested/deeper/text.txt", fixture_time);
    std::filesystem::last_write_time(payload / "empty directory",
                                     fixture_time - std::chrono::seconds{1});
    std::filesystem::last_write_time(payload / "nested/deeper",
                                     fixture_time - std::chrono::seconds{2});
    std::filesystem::last_write_time(payload / "nested", fixture_time - std::chrono::seconds{3});
    std::filesystem::last_write_time(payload, fixture_time - std::chrono::seconds{4});
#ifndef _WIN32
    ASSERT_EQ(::chmod((payload / "nested/deeper/text.txt").c_str(), 0640), 0);
    ASSERT_EQ(::chmod((payload / "empty directory").c_str(), 0750), 0);
    const test::MetadataPolicy metadata_policy{
        .compare_modification_time = true,
        .posix_mode = test::PosixModePolicy::require_equal,
    };
#else
    const test::MetadataPolicy metadata_policy{};
#endif

    RepositoryCreateOptions create_options;
    Repository::create(repository_root, create_options);
    Repository repository = Repository::open(repository_root, OpenMode::read_write);
    const SnapshotResult snapshot =
        SnapshotEngine(repository)
            .create_snapshot(source, SnapshotOptions{.message = "snapshot/restore acceptance"});

    EXPECT_GT(snapshot.snapshot_id, 0);
    EXPECT_EQ(snapshot.file_count, 7U);
    EXPECT_EQ(snapshot.directory_count, 5U);
    EXPECT_EQ(snapshot.logical_bytes,
              spanning_file_size + std::string_view{"small text\n"}.size() +
                  std::string_view{"spaces"}.size() + std::string_view{"hidden"}.size() +
                  std::string_view{"nfc"}.size() + std::string_view{"nfd"}.size());
    EXPECT_GT(snapshot.new_chunks, 0U);
    EXPECT_TRUE(snapshot.skipped_entries.empty());

    {
        Database database(repository_root / "repository.db");
        MetadataStore metadata(database);
        const std::vector<SnapshotSummary> snapshots = metadata.list_complete_snapshots();
        ASSERT_EQ(snapshots.size(), 1U);
        EXPECT_EQ(snapshots.front().id, snapshot.snapshot_id);
        EXPECT_EQ(snapshots.front().message, "snapshot/restore acceptance");
        EXPECT_EQ(snapshots.front().file_count, snapshot.file_count);
        EXPECT_EQ(snapshots.front().directory_count, snapshot.directory_count);
        EXPECT_EQ(snapshots.front().logical_size, snapshot.logical_bytes);

        const std::vector<EntryInfo> entries = metadata.list_entries(snapshot.snapshot_id);
        const EntryInfo* empty_file = find_entry(entries, "snapshot subtree/empty file");
        ASSERT_NE(empty_file, nullptr);
        EXPECT_EQ(empty_file->logical_size, 0U);
        EXPECT_TRUE(metadata.list_entry_chunks(empty_file->id).empty());
        ASSERT_NE(find_entry(entries, "snapshot subtree/" + on_disk_nfc_name), nullptr);
        ASSERT_NE(find_entry(entries, "snapshot subtree/" + on_disk_nfd_name), nullptr);

        const EntryInfo* spanning = find_entry(entries, "snapshot subtree/nested/spanning.bin");
        ASSERT_NE(spanning, nullptr);
        EXPECT_EQ(metadata.list_entry_chunks(spanning->id).size(), 1U);
    }

    std::filesystem::rename(source, expected);
    ASSERT_FALSE(std::filesystem::exists(source));

    test::DatasetBuilder(destination).text_file("unrelated.keep", "leave me alone");
    const std::filesystem::path unrelated = destination / "unrelated.keep";
    const std::vector<std::byte> unrelated_bytes = test::read_all_bytes(unrelated);
    const auto unrelated_time = std::filesystem::last_write_time(unrelated);
    const std::filesystem::path requested_destination =
        std::filesystem::weakly_canonical(destination);
    ASSERT_TRUE(requested_destination.is_absolute());

    const RestoreResult restored = RestoreEngine(repository)
                                       .restore({
                                           .snapshot_id = snapshot.snapshot_id,
                                           .destination_root = requested_destination,
                                           .overwrite_policy = OverwritePolicy::never,
                                       });

    EXPECT_EQ(restored.restored_files, snapshot.file_count);
    EXPECT_EQ(restored.restored_directories, snapshot.directory_count);
    EXPECT_EQ(restored.restored_symlinks, symlink_created ? 1U : 0U);
    EXPECT_EQ(restored.restored_bytes, snapshot.logical_bytes);
    EXPECT_TRUE(restored.skipped_entries.empty());
    test::expect_tree_equal(expected / "snapshot subtree", destination / "snapshot subtree",
                            metadata_policy);
    EXPECT_EQ(std::filesystem::file_size(destination / "snapshot subtree/empty file"), 0U);
    EXPECT_EQ(test::read_all_bytes(unrelated), unrelated_bytes);
    EXPECT_EQ(std::filesystem::last_write_time(unrelated), unrelated_time);
}

} // namespace
} // namespace localvault
