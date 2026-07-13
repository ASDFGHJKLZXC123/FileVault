#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

[[nodiscard]] std::uint64_t count_zstd_objects(const std::filesystem::path& repository_root) {
    std::uint64_t count = 0;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(repository_root / "objects")) {
        if (entry.is_regular_file() && entry.path().extension() == ".zst") {
            ++count;
        }
    }
    return count;
}

void overwrite_repeated_region(const std::filesystem::path& path, std::uint64_t offset,
                               std::uint64_t size, char value) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(file) << path;
    file.seekp(static_cast<std::streamoff>(offset));
    ASSERT_TRUE(file) << path;
    std::array<char, 64U * 1024U> bytes{};
    bytes.fill(value);
    while (size > 0) {
        const std::size_t count =
            static_cast<std::size_t>(std::min<std::uint64_t>(size, bytes.size()));
        file.write(bytes.data(), static_cast<std::streamsize>(count));
        ASSERT_TRUE(file) << path;
        size -= count;
    }
    file.flush();
    ASSERT_TRUE(file) << path;
}

TEST(SnapshotRestoreAcceptanceTest, RestoresDeletedFixtureIntoNonemptyAlternateDestination) {
    constexpr std::size_t chunk_size = 4U * 1024U * 1024U;
    constexpr std::size_t spanning_file_size = 2U * chunk_size + 17U;
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
        .repeated_file("snapshot subtree/exact chunk.bin", chunk_size, std::byte{0x5A})
        .repeated_file("snapshot subtree/nested/spanning.bin", spanning_file_size, std::byte{0xA5});

    const std::filesystem::path spanning_path = payload / "nested/spanning.bin";
    overwrite_repeated_region(spanning_path, chunk_size, chunk_size, '\x3C');
    overwrite_repeated_region(spanning_path, 2U * chunk_size, 17U, '\x7E');

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
    EXPECT_EQ(snapshot.file_count, 8U);
    EXPECT_EQ(snapshot.directory_count, 5U);
    EXPECT_EQ(snapshot.logical_bytes,
              spanning_file_size + chunk_size + std::string_view{"small text\n"}.size() +
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
        EXPECT_GE(metadata.list_entry_chunks(spanning->id).size(), 3U);
        const EntryInfo* exact = find_entry(entries, "snapshot subtree/exact chunk.bin");
        ASSERT_NE(exact, nullptr);
        EXPECT_EQ(metadata.list_entry_chunks(exact->id).size(), 1U);
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
                                           .relative_paths = {},
                                           .destination_root = requested_destination,
                                           .overwrite_policy = OverwritePolicy::never,
                                           .conflict_resolver = {},
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

TEST(SnapshotRestoreAcceptanceTest,
     ReusesIdenticalChunksAcrossUnchangedCopyAndOneAlignedRegionChange) {
    constexpr std::uint64_t chunk_size = 4ULL * 1024ULL * 1024ULL;
    constexpr std::uint64_t file_size = 3ULL * chunk_size;
    test::TemporaryDirectory temporary;
    const std::filesystem::path source = temporary.path() / "dedup-source";
    const std::filesystem::path repository_root = temporary.path() / "repository";
    const std::filesystem::path original = source / "original.bin";
    test::DatasetBuilder(source).repeated_file("original.bin", file_size, std::byte{0x11});
    overwrite_repeated_region(original, chunk_size, chunk_size, '\x22');
    overwrite_repeated_region(original, 2U * chunk_size, chunk_size, '\x33');

    Repository::create(repository_root);
    Repository repository = Repository::open(repository_root, OpenMode::read_write);
    SnapshotEngine snapshots(repository);
    const SnapshotResult first = snapshots.create_snapshot(source, {});
    ASSERT_EQ(first.new_chunks, 3U);
    const std::uint64_t first_object_count = count_zstd_objects(repository_root);
    EXPECT_EQ(first_object_count, 3U);

    const SnapshotResult unchanged = snapshots.create_snapshot(source, {});
    EXPECT_EQ(unchanged.new_chunks, 0U);
    EXPECT_EQ(unchanged.new_stored_bytes, 0U);
    EXPECT_EQ(count_zstd_objects(repository_root), first_object_count);

    ASSERT_TRUE(std::filesystem::copy_file(original, source / "copy.bin"));
    const SnapshotResult copied = snapshots.create_snapshot(source, {});
    EXPECT_EQ(copied.logical_bytes, first.logical_bytes + file_size);
    EXPECT_EQ(copied.new_chunks, 0U);
    EXPECT_EQ(copied.new_stored_bytes, 0U);
    EXPECT_EQ(count_zstd_objects(repository_root), first_object_count);

    Database database(repository_root / "repository.db");
    MetadataStore metadata(database);
    const std::vector<EntryInfo> copied_entries = metadata.list_entries(copied.snapshot_id);
    const EntryInfo* copied_original = find_entry(copied_entries, "original.bin");
    const EntryInfo* copied_copy = find_entry(copied_entries, "copy.bin");
    ASSERT_NE(copied_original, nullptr);
    ASSERT_NE(copied_copy, nullptr);
    const auto original_chunks = metadata.list_entry_chunks(copied_original->id);
    const auto copy_chunks = metadata.list_entry_chunks(copied_copy->id);
    ASSERT_EQ(original_chunks.size(), 3U);
    ASSERT_EQ(copy_chunks.size(), original_chunks.size());
    for (std::size_t index = 0; index < original_chunks.size(); ++index) {
        EXPECT_EQ(copy_chunks[index].hash_hex, original_chunks[index].hash_hex);
    }

    overwrite_repeated_region(source / "copy.bin", chunk_size, chunk_size, '\x44');
    const SnapshotResult modified = snapshots.create_snapshot(source, {});
    EXPECT_EQ(modified.new_chunks, 1U);
    EXPECT_GT(modified.new_stored_bytes, 0U);
    EXPECT_EQ(count_zstd_objects(repository_root), first_object_count + 1U);

    const std::vector<EntryInfo> modified_entries = metadata.list_entries(modified.snapshot_id);
    const EntryInfo* modified_original = find_entry(modified_entries, "original.bin");
    const EntryInfo* modified_copy = find_entry(modified_entries, "copy.bin");
    ASSERT_NE(modified_original, nullptr);
    ASSERT_NE(modified_copy, nullptr);
    const auto unchanged_chunks = metadata.list_entry_chunks(modified_original->id);
    const auto changed_chunks = metadata.list_entry_chunks(modified_copy->id);
    ASSERT_EQ(changed_chunks.size(), unchanged_chunks.size());
    std::size_t different_chunks = 0;
    for (std::size_t index = 0; index < unchanged_chunks.size(); ++index) {
        different_chunks += changed_chunks[index].hash_hex != unchanged_chunks[index].hash_hex;
    }
    EXPECT_EQ(different_chunks, 1U);
    EXPECT_EQ(changed_chunks[0].hash_hex, unchanged_chunks[0].hash_hex);
    EXPECT_NE(changed_chunks[1].hash_hex, unchanged_chunks[1].hash_hex);
    EXPECT_EQ(changed_chunks[2].hash_hex, unchanged_chunks[2].hash_hex);
}

} // namespace
} // namespace localvault
