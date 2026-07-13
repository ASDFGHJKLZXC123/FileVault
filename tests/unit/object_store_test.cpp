#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

#include "localvault/error.hpp"
#include "storage/chunker.hpp"
#include "storage/object_store.hpp"
#include "support/test_filesystem.hpp"

namespace localvault {
namespace {

TEST(ChunkerTest, EmitsBoundedOrderedChunksAndNoChunkForAnEmptyFile) {
    test::TemporaryDirectory temporary;
    constexpr std::array<std::byte, 9> bytes{std::byte{0}, std::byte{1}, std::byte{2},
                                             std::byte{3}, std::byte{4}, std::byte{5},
                                             std::byte{6}, std::byte{7}, std::byte{8}};
    test::DatasetBuilder(temporary.path())
        .binary_file("data.bin", bytes)
        .text_file("empty.bin", "");

    std::vector<ByteCount> offsets;
    std::vector<std::size_t> sizes;
    std::vector<std::byte> reconstructed;
    Chunker(4).for_each_chunk(
        temporary.path() / "data.bin", {}, [&](ByteCount offset, std::span<const std::byte> chunk) {
            offsets.push_back(offset);
            sizes.push_back(chunk.size());
            reconstructed.insert(reconstructed.end(), chunk.begin(), chunk.end());
        });
    EXPECT_EQ(offsets, (std::vector<ByteCount>{0, 4, 8}));
    EXPECT_EQ(sizes, (std::vector<std::size_t>{4, 4, 1}));
    EXPECT_EQ(reconstructed, (std::vector<std::byte>(bytes.begin(), bytes.end())));

    std::size_t empty_chunk_count = 0;
    Chunker(4).for_each_chunk(temporary.path() / "empty.bin", {},
                              [&](ByteCount, std::span<const std::byte>) { ++empty_chunk_count; });
    EXPECT_EQ(empty_chunk_count, 0U);
    EXPECT_THROW((void)Chunker(0), LocalVaultError);
}

TEST(ChunkerTest, HonorsCancellationBeforeReading) {
    test::TemporaryDirectory temporary;
    test::DatasetBuilder(temporary.path()).text_file("data.bin", "contents");
    std::stop_source stop;
    stop.request_stop();

    try {
        Chunker(4).for_each_chunk(temporary.path() / "data.bin", stop.get_token(),
                                  [](ByteCount, std::span<const std::byte>) {});
        FAIL() << "cancelled chunking unexpectedly succeeded";
    } catch (const LocalVaultError& error) {
        EXPECT_EQ(error.code(), ErrorCode::cancelled);
    }
}

TEST(ObjectStoreTest, StoresRealBlake3AtDeterministicRawPathAndReusesIt) {
    test::TemporaryDirectory temporary;
    std::filesystem::create_directories(temporary.path() / "temporary" / "objects");
    constexpr std::array<std::byte, 3> abc{std::byte{static_cast<unsigned char>('a')},
                                           std::byte{static_cast<unsigned char>('b')},
                                           std::byte{static_cast<unsigned char>('c')}};
    ObjectStore objects(temporary.path());

    const StoredObject first = objects.store(abc);
    EXPECT_EQ(first.hash_hex, "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85");
    EXPECT_EQ(first.relative_path.generic_string(),
              "objects/64/6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85.raw");
    EXPECT_EQ(first.raw_size, 3U);
    EXPECT_EQ(first.stored_size, 3U);
    EXPECT_TRUE(first.newly_stored);
    EXPECT_EQ(objects.read_verified(first.hash_hex, first.relative_path, 3, 3),
              (std::vector<std::byte>(abc.begin(), abc.end())));

    const StoredObject reused = objects.store(abc);
    EXPECT_FALSE(reused.newly_stored);
    EXPECT_EQ(reused.hash_hex, first.hash_hex);
    EXPECT_TRUE(std::filesystem::is_empty(temporary.path() / "temporary" / "objects"));
    EXPECT_THROW((void)objects.store({}), LocalVaultError);
}

TEST(ObjectStoreTest, RejectsMissingMisdirectedWrongSizedAndCorruptObjects) {
    test::TemporaryDirectory temporary;
    std::filesystem::create_directories(temporary.path() / "temporary" / "objects");
    constexpr std::array<std::byte, 4> bytes{std::byte{0x10}, std::byte{0x20}, std::byte{0x30},
                                             std::byte{0x40}};
    ObjectStore objects(temporary.path());
    const StoredObject stored = objects.store(bytes);

    EXPECT_THROW((void)objects.read_verified(stored.hash_hex, "objects/wrong.raw", 4, 4),
                 LocalVaultError);
    EXPECT_THROW((void)objects.read_verified(stored.hash_hex, stored.relative_path, 3, 4),
                 LocalVaultError);
    EXPECT_THROW((void)objects.read_verified(stored.hash_hex, stored.relative_path, 0, 0),
                 LocalVaultError);

    const std::string missing_hash(64, 'b');
    const std::filesystem::path missing_path =
        std::filesystem::path("objects") / "bb" / (missing_hash + ".raw");
    try {
        (void)objects.read_verified(missing_hash, missing_path, 1, 1);
        FAIL() << "missing object unexpectedly read successfully";
    } catch (const LocalVaultError& error) {
        EXPECT_EQ(error.code(), ErrorCode::object_missing);
    }

    test::corrupt_byte(temporary.path() / stored.relative_path, 1);
    try {
        (void)objects.read_verified(stored.hash_hex, stored.relative_path, 4, 4);
        FAIL() << "corrupt object unexpectedly verified";
    } catch (const LocalVaultError& error) {
        EXPECT_EQ(error.code(), ErrorCode::object_corrupt);
    }
}

} // namespace
} // namespace localvault
