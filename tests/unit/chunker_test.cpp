#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <istream>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <streambuf>
#include <string>
#include <utility>
#include <vector>

#include "localvault/error.hpp"
#include "storage/chunker.hpp"
#include "support/test_filesystem.hpp"

namespace localvault {
namespace {

constexpr std::size_t kChunkSize = 4U * 1024U * 1024U;

struct ChunkLayout {
    std::vector<ByteCount> offsets;
    std::vector<std::size_t> lengths;
};

[[nodiscard]] ChunkLayout layout_for(const std::filesystem::path& path,
                                     ByteCount chunk_size = kChunkSize) {
    ChunkLayout result;
    Chunker(chunk_size)
        .for_each_chunk(path, {}, [&](ByteCount offset, std::span<const std::byte> bytes) {
            result.offsets.push_back(offset);
            result.lengths.push_back(bytes.size());
        });
    return result;
}

class ControlledStreamBuffer final : public std::streambuf {
  public:
    explicit ControlledStreamBuffer(std::string bytes,
                                    std::optional<std::size_t> failing_read = std::nullopt)
        : bytes_(std::move(bytes)), failing_read_(failing_read) {}

    [[nodiscard]] std::size_t read_calls() const noexcept {
        return read_calls_;
    }

  protected:
    std::streamsize xsgetn(char* destination, std::streamsize requested) override {
        ++read_calls_;
        if (failing_read_ == read_calls_) {
            throw std::runtime_error("injected read failure");
        }
        const std::size_t available = bytes_.size() - offset_;
        const std::size_t count = std::min(static_cast<std::size_t>(requested), available);
        if (count > 0) {
            std::memcpy(destination, bytes_.data() + offset_, count);
        }
        offset_ += count;
        return static_cast<std::streamsize>(count);
    }

  private:
    std::string bytes_;
    std::optional<std::size_t> failing_read_;
    std::size_t offset_{0};
    std::size_t read_calls_{0};
};

TEST(ChunkerTest, EmptyFileEmitsNoChunksAndInvalidLimitsAreRejected) {
    test::TemporaryDirectory temporary;
    test::DatasetBuilder(temporary.path()).text_file("empty.bin", "");

    const ChunkLayout layout = layout_for(temporary.path() / "empty.bin");
    EXPECT_TRUE(layout.offsets.empty());
    EXPECT_TRUE(layout.lengths.empty());
    EXPECT_THROW((void)Chunker(0), LocalVaultError);
    EXPECT_THROW((void)Chunker(kChunkSize + 1U), LocalVaultError);
}

TEST(ChunkerTest, OneByteAndShortFilesEmitOneBinaryChunk) {
    test::TemporaryDirectory temporary;
    constexpr std::array<std::byte, 1> one_byte{std::byte{0}};
    constexpr std::array<std::byte, 7> short_bytes{
        std::byte{0}, std::byte{1}, std::byte{2}, std::byte{3},
        std::byte{4}, std::byte{5}, std::byte{6},
    };
    test::DatasetBuilder(temporary.path())
        .binary_file("one.bin", one_byte)
        .binary_file("short.bin", short_bytes);

    const ChunkLayout one = layout_for(temporary.path() / "one.bin");
    EXPECT_EQ(one.offsets, (std::vector<ByteCount>{0}));
    EXPECT_EQ(one.lengths, (std::vector<std::size_t>{1}));

    std::vector<std::byte> reconstructed;
    Chunker(kChunkSize)
        .for_each_chunk(temporary.path() / "short.bin", {},
                        [&](ByteCount offset, std::span<const std::byte> bytes) {
                            EXPECT_EQ(offset, 0U);
                            reconstructed.assign(bytes.begin(), bytes.end());
                        });
    EXPECT_EQ(reconstructed, (std::vector<std::byte>(short_bytes.begin(), short_bytes.end())));
}

TEST(ChunkerTest, ExactMaximumAndMaximumPlusOneHaveNoTrailingEmptyChunk) {
    test::TemporaryDirectory temporary;
    test::DatasetBuilder(temporary.path())
        .repeated_file("exact.bin", kChunkSize, std::byte{0x2A})
        .repeated_file("plus-one.bin", kChunkSize + 1U, std::byte{0x2B});

    const ChunkLayout exact = layout_for(temporary.path() / "exact.bin");
    EXPECT_EQ(exact.offsets, (std::vector<ByteCount>{0}));
    EXPECT_EQ(exact.lengths, (std::vector<std::size_t>{kChunkSize}));

    const ChunkLayout plus_one = layout_for(temporary.path() / "plus-one.bin");
    EXPECT_EQ(plus_one.offsets, (std::vector<ByteCount>{0, kChunkSize}));
    EXPECT_EQ(plus_one.lengths, (std::vector<std::size_t>{kChunkSize, 1}));
}

TEST(ChunkerTest, MultipleFullChunksAndShortTailPreserveOffsetsAndLengths) {
    test::TemporaryDirectory temporary;
    test::DatasetBuilder(temporary.path())
        .repeated_file("full.bin", 2U * kChunkSize, std::byte{0x3A})
        .repeated_file("tail.bin", 9, std::byte{0x3B});

    const ChunkLayout full = layout_for(temporary.path() / "full.bin");
    EXPECT_EQ(full.offsets, (std::vector<ByteCount>{0, kChunkSize}));
    EXPECT_EQ(full.lengths, (std::vector<std::size_t>{kChunkSize, kChunkSize}));

    const ChunkLayout tail = layout_for(temporary.path() / "tail.bin", 4);
    EXPECT_EQ(tail.offsets, (std::vector<ByteCount>{0, 4, 8}));
    EXPECT_EQ(tail.lengths, (std::vector<std::size_t>{4, 4, 1}));
}

TEST(ChunkerTest, ReportsAnInjectedReadErrorAfterACompletedChunk) {
    ControlledStreamBuffer buffer(std::string(8, 'x'), 2);
    std::istream input(&buffer);
    std::size_t callback_count = 0;

    try {
        Chunker(4).for_each_chunk(input, {},
                                  [&](ByteCount, std::span<const std::byte>) { ++callback_count; });
        FAIL() << "chunking unexpectedly ignored an injected read error";
    } catch (const LocalVaultError& error) {
        EXPECT_EQ(error.code(), ErrorCode::filesystem_error);
    }
    EXPECT_EQ(callback_count, 1U);
    EXPECT_EQ(buffer.read_calls(), 2U);
}

TEST(ChunkerTest, CancellationIsCheckedBeforeReadingAndBetweenChunks) {
    ControlledStreamBuffer pre_cancelled_buffer(std::string(8, 'x'));
    std::istream pre_cancelled_input(&pre_cancelled_buffer);
    std::stop_source pre_cancelled;
    pre_cancelled.request_stop();
    EXPECT_THROW(Chunker(4).for_each_chunk(pre_cancelled_input, pre_cancelled.get_token(),
                                           [](ByteCount, std::span<const std::byte>) {}),
                 LocalVaultError);
    EXPECT_EQ(pre_cancelled_buffer.read_calls(), 0U);

    ControlledStreamBuffer buffer(std::string(8, 'x'));
    std::istream input(&buffer);
    std::stop_source stop;
    std::size_t callback_count = 0;
    try {
        Chunker(4).for_each_chunk(input, stop.get_token(),
                                  [&](ByteCount, std::span<const std::byte>) {
                                      ++callback_count;
                                      stop.request_stop();
                                  });
        FAIL() << "chunking unexpectedly continued after cancellation";
    } catch (const LocalVaultError& error) {
        EXPECT_EQ(error.code(), ErrorCode::cancelled);
    }
    EXPECT_EQ(callback_count, 1U);
    EXPECT_EQ(buffer.read_calls(), 1U);
}

} // namespace
} // namespace localvault
