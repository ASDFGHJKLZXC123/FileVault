#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "localvault/error.hpp"
#include "storage/zstd_codec.hpp"

namespace localvault {
namespace {

constexpr std::size_t maximum_chunk_size = 4U * 1024U * 1024U;

[[nodiscard]] std::vector<std::byte> text_bytes(std::string_view text) {
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (char character : text) {
        bytes.push_back(std::byte{static_cast<unsigned char>(character)});
    }
    return bytes;
}

[[nodiscard]] std::vector<std::byte> deterministic_random_bytes(std::size_t size) {
    std::mt19937 generator(0x5A17D3U);
    std::vector<std::byte> bytes(size);
    for (std::byte& byte : bytes) {
        byte = std::byte{static_cast<unsigned char>(generator())};
    }
    return bytes;
}

TEST(ZstdCodecTest, EmptyInputUsesAValidEmptyFrame) {
    const ZstdCodec codec(3);

    const std::vector<std::byte> compressed = codec.compress({});

    EXPECT_FALSE(compressed.empty());
    EXPECT_TRUE(codec.decompress(compressed, 0, maximum_chunk_size).empty());
}

TEST(ZstdCodecTest, RoundTripsSmallTextAndRandomBinary) {
    const ZstdCodec codec(3);
    const std::vector<std::byte> text = text_bytes("LocalVault zstd round trip\n");
    const std::vector<std::byte> binary = deterministic_random_bytes(64U * 1024U);

    EXPECT_EQ(codec.decompress(codec.compress(text), text.size(), maximum_chunk_size), text);
    EXPECT_EQ(codec.decompress(codec.compress(binary), binary.size(), maximum_chunk_size), binary);
}

TEST(ZstdCodecTest, IncompressibleDataStillUsesOneZstdFrame) {
    const ZstdCodec codec(3);
    const std::vector<std::byte> raw = deterministic_random_bytes(256U * 1024U);

    const std::vector<std::byte> compressed = codec.compress(raw);

    EXPECT_GT(compressed.size(), raw.size());
    EXPECT_EQ(codec.decompress(compressed, raw.size(), maximum_chunk_size), raw);

    std::vector<std::byte> two_frames = compressed;
    two_frames.insert(two_frames.end(), compressed.begin(), compressed.end());
    EXPECT_THROW((void)codec.decompress(two_frames, raw.size(), maximum_chunk_size),
                 LocalVaultError);
}

TEST(ZstdCodecTest, RoundTripsMaximumAllowedChunkSize) {
    const ZstdCodec codec(3);
    std::vector<std::byte> raw(maximum_chunk_size);
    for (std::size_t index = 0; index < raw.size(); ++index) {
        raw[index] = std::byte{static_cast<unsigned char>(index % 251U)};
    }

    EXPECT_EQ(codec.decompress(codec.compress(raw), raw.size(), maximum_chunk_size), raw);
}

TEST(ZstdCodecTest, RejectsRawAndCompressedSizesAboveConfiguredBounds) {
    const ZstdCodec codec(3);
    EXPECT_THROW((void)codec.decompress({}, 17, 16), LocalVaultError);

    const std::vector<std::byte> oversized_compressed(1024);
    EXPECT_THROW((void)codec.decompress(oversized_compressed, 16, 16), LocalVaultError);
}

TEST(ZstdCodecTest, RejectsInvalidAndTruncatedFramesWithZstdErrorNames) {
    const ZstdCodec codec(3);
    constexpr std::array invalid{std::byte{0x13}, std::byte{0x37}, std::byte{0x00},
                                 std::byte{0x42}};

    try {
        (void)codec.decompress(invalid, 3, maximum_chunk_size);
        FAIL() << "invalid zstd input unexpectedly decompressed";
    } catch (const LocalVaultError& error) {
        EXPECT_EQ(error.code(), ErrorCode::compression_error);
        const std::string message = error.what();
        EXPECT_NE(message.find("zstd frame validation failed: "), std::string::npos);
        EXPECT_NE(message.find("Unknown frame descriptor"), std::string::npos);
    }

    std::vector<std::byte> truncated = codec.compress(text_bytes("truncated frame"));
    truncated.pop_back();
    EXPECT_THROW((void)codec.decompress(truncated, 15, maximum_chunk_size), LocalVaultError);
}

TEST(ZstdCodecTest, RejectsExpectedRawSizeMismatch) {
    const ZstdCodec codec(3);
    const std::vector<std::byte> raw = text_bytes("size mismatch");
    const std::vector<std::byte> compressed = codec.compress(raw);

    try {
        (void)codec.decompress(compressed, raw.size() + 1U, maximum_chunk_size);
        FAIL() << "mismatched expected size unexpectedly decompressed";
    } catch (const LocalVaultError& error) {
        EXPECT_EQ(error.code(), ErrorCode::compression_error);
        EXPECT_NE(std::string(error.what()).find("does not match expected raw size"),
                  std::string::npos);
    }
}

} // namespace
} // namespace localvault
