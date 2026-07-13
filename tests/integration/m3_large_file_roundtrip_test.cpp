#include <gtest/gtest.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "database/database.hpp"
#include "database/metadata_store.hpp"
#include "localvault/repository.hpp"
#include "localvault/restore_engine.hpp"
#include "localvault/snapshot_engine.hpp"
#include "support/test_filesystem.hpp"

namespace localvault {
namespace {

[[nodiscard]] std::uint64_t manifest_number(std::string_view manifest, std::string_view key) {
    const std::string quoted_key = "\"" + std::string(key) + "\"";
    const std::size_t key_offset = manifest.find(quoted_key);
    if (key_offset == std::string_view::npos) {
        throw std::runtime_error("dataset manifest is missing " + std::string(key));
    }
    const std::size_t colon = manifest.find(':', key_offset + quoted_key.size());
    if (colon == std::string_view::npos) {
        throw std::runtime_error("dataset manifest has an invalid " + std::string(key));
    }
    const std::size_t first_digit = manifest.find_first_of("0123456789", colon + 1U);
    if (first_digit == std::string_view::npos) {
        throw std::runtime_error("dataset manifest has an invalid " + std::string(key));
    }
    std::uint64_t value = 0;
    const char* begin = manifest.data() + first_digit;
    const char* end = manifest.data() + manifest.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{}) {
        throw std::runtime_error("dataset manifest has an invalid " + std::string(key));
    }
    return value;
}

[[nodiscard]] std::string manifest_string(std::string_view manifest, std::string_view key) {
    const std::string quoted_key = "\"" + std::string(key) + "\"";
    const std::size_t key_offset = manifest.find(quoted_key);
    if (key_offset == std::string_view::npos) {
        throw std::runtime_error("dataset manifest is missing " + std::string(key));
    }
    const std::size_t colon = manifest.find(':', key_offset + quoted_key.size());
    if (colon == std::string_view::npos) {
        throw std::runtime_error("dataset manifest has an invalid " + std::string(key));
    }
    const std::size_t opening_quote = manifest.find('"', colon + 1U);
    if (opening_quote == std::string_view::npos) {
        throw std::runtime_error("dataset manifest has an invalid " + std::string(key));
    }
    const std::size_t closing_quote = manifest.find('"', opening_quote + 1U);
    if (closing_quote == std::string_view::npos) {
        throw std::runtime_error("dataset manifest has an invalid " + std::string(key));
    }
    return std::string(manifest.substr(opening_quote + 1U, closing_quote - opening_quote - 1U));
}

[[nodiscard]] bool is_lowercase_blake3_hex(std::string_view value) {
    return value.size() == 64U && std::ranges::all_of(value, [](char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

TEST(M3LargeFileRoundTripTest, ExternalDatasetRoundTripsWithBoundedChunks) {
    const char* configured_dataset = std::getenv("LOCALVAULT_M3_LARGE_DATASET");
    if (configured_dataset == nullptr || *configured_dataset == '\0') {
        GTEST_SKIP() << "LOCALVAULT_M3_LARGE_DATASET is not set";
    }
    const std::filesystem::path dataset =
        std::filesystem::absolute(configured_dataset).lexically_normal();
    ASSERT_TRUE(std::filesystem::is_directory(dataset));
    std::ifstream manifest_input(dataset / "manifest.json", std::ios::binary);
    ASSERT_TRUE(manifest_input);
    const std::string manifest{std::istreambuf_iterator<char>(manifest_input),
                               std::istreambuf_iterator<char>()};
    ASSERT_EQ(manifest_number(manifest, "generator_version"), 1U);
    static_cast<void>(manifest_number(manifest, "seed"));
    ASSERT_EQ(manifest_string(manifest, "profile"), "large-files");
    const std::uint64_t declared_file_count = manifest_number(manifest, "file_count");
    const std::uint64_t declared_logical_bytes = manifest_number(manifest, "logical_bytes");
    ASSERT_GE(declared_logical_bytes, 2ULL * 1024ULL * 1024ULL * 1024ULL);
    std::uint64_t actual_file_count = 0;
    std::uint64_t actual_logical_bytes = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dataset)) {
        if (entry.is_regular_file() && entry.path().filename() != "manifest.json") {
            ++actual_file_count;
            actual_logical_bytes += entry.file_size();
        }
    }
    ASSERT_EQ(actual_file_count, declared_file_count);
    ASSERT_EQ(actual_logical_bytes, declared_logical_bytes);

    test::TemporaryDirectory temporary;
    const std::filesystem::path repository_root = temporary.path() / "repository";
    const std::filesystem::path restored =
        std::filesystem::weakly_canonical(temporary.path()) / "restored";
    Repository::create(repository_root);
    Repository repository = Repository::open(repository_root, OpenMode::read_write);
    const SnapshotResult snapshot = SnapshotEngine(repository).create_snapshot(dataset, {});
    EXPECT_GE(snapshot.logical_bytes, declared_logical_bytes);

    Database database(repository_root / "repository.db");
    MetadataStore metadata(database);
    const std::vector<EntryInfo> entries = metadata.list_entries(snapshot.snapshot_id);
    bool saw_multi_chunk_file = false;
    for (const EntryInfo& entry : entries) {
        if (entry.type != EntryType::regular_file) {
            continue;
        }
        ASSERT_TRUE(entry.file_hash_hex.has_value()) << entry.relative_path;
        EXPECT_TRUE(is_lowercase_blake3_hex(*entry.file_hash_hex)) << entry.relative_path;
        const std::vector<ChunkReferenceInfo> chunks = metadata.list_entry_chunks(entry.id);
        saw_multi_chunk_file = saw_multi_chunk_file || chunks.size() > 1U;
        ByteCount offset = 0;
        for (const ChunkReferenceInfo& chunk : chunks) {
            EXPECT_EQ(chunk.raw_offset, offset);
            EXPECT_GT(chunk.raw_length, 0U);
            EXPECT_LE(chunk.raw_length, repository.info().chunk_size_bytes);
            EXPECT_LE(chunk.raw_size, repository.info().chunk_size_bytes);
            EXPECT_GT(chunk.stored_size, 0U);
            EXPECT_LE(chunk.stored_size, repository.info().chunk_size_bytes +
                                             repository.info().chunk_size_bytes / 8U + 64U * 1024U);
            EXPECT_EQ(chunk.object_path.extension(), ".zst");
            EXPECT_EQ(std::filesystem::file_size(repository_root / chunk.object_path),
                      chunk.stored_size);
            ASSERT_LE(chunk.raw_length, (std::numeric_limits<ByteCount>::max)() - offset);
            offset += chunk.raw_length;
        }
        EXPECT_EQ(offset, entry.logical_size);
    }
    EXPECT_TRUE(saw_multi_chunk_file);

    const RestoreResult result = RestoreEngine(repository)
                                     .restore({.snapshot_id = snapshot.snapshot_id,
                                               .relative_paths = {},
                                               .destination_root = restored,
                                               .overwrite_policy = OverwritePolicy::never,
                                               .conflict_resolver = {}});
    EXPECT_EQ(result.restored_files, snapshot.file_count);
    EXPECT_EQ(result.restored_bytes, snapshot.logical_bytes);
    EXPECT_TRUE(result.skipped_entries.empty());
    test::expect_tree_equal(
        dataset, restored,
        {.compare_modification_time = false, .posix_mode = test::PosixModePolicy::ignore});
}

} // namespace
} // namespace localvault
