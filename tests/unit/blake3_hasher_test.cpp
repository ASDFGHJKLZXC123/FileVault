#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "storage/blake3_hasher.hpp"

namespace localvault {
namespace {

struct OfficialVector {
    std::size_t input_length;
    std::string_view hash;
};

// Source: BLAKE3-team/BLAKE3 v1.8.5 test_vectors/test_vectors.json. It constructs every input by
// repeating the 251-byte sequence 0, 1, ..., 250. These are its first 32 hash output bytes.
constexpr std::array official_vectors{
    OfficialVector{0, "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"},
    OfficialVector{1, "2d3adedff11b61f14c886e35afa036736dcd87a74d27b5c1510225d0f592e213"},
    OfficialVector{3, "e1be4d7a8ab5560aa4199eea339849ba8e293d55ca0a81006726d184519e647f"},
    OfficialVector{102400, "bc3e3d41a1146b069abffad3c0d44860cf664390afce4d9661f7902e7943e085"},
};

[[nodiscard]] std::vector<std::byte> official_input(std::size_t size) {
    std::vector<std::byte> input(size);
    for (std::size_t index = 0; index < input.size(); ++index) {
        input[index] = static_cast<std::byte>(index % 251U);
    }
    return input;
}

[[nodiscard]] std::string hash(std::span<const std::byte> input) {
    Blake3Hasher hasher;
    hasher.update(input);
    return Blake3Hasher::to_hex(hasher.finalize());
}

TEST(Blake3HasherTest, MatchesOfficialVectors) {
    for (const OfficialVector& vector : official_vectors) {
        SCOPED_TRACE(vector.input_length);
        EXPECT_EQ(hash(official_input(vector.input_length)), vector.hash);
    }
}

TEST(Blake3HasherTest, IncrementalHashMatchesOneShotAboveFourMiB) {
    constexpr std::size_t input_size = 4U * 1024U * 1024U + 17U;
    const std::vector<std::byte> input = official_input(input_size);

    Blake3Hasher incremental;
    constexpr std::size_t update_size = 7777;
    for (std::size_t offset = 0; offset < input.size(); offset += update_size) {
        incremental.update(
            std::span(input).subspan(offset, std::min(update_size, input.size() - offset)));
    }

    EXPECT_EQ(incremental.finalize(), [&] {
        Blake3Hasher one_shot;
        one_shot.update(input);
        return one_shot.finalize();
    }());
}

TEST(Blake3HasherTest, AcceptsEmptyAndBinaryInputContainingZeroBytes) {
    Blake3Hasher empty;
    empty.update({});
    EXPECT_EQ(Blake3Hasher::to_hex(empty.finalize()), official_vectors.front().hash);

    constexpr std::array binary{std::byte{0x00}, std::byte{0x7f}, std::byte{0x00}, std::byte{0xff},
                                std::byte{0x01}};
    Blake3Hasher incremental;
    incremental.update(std::span(binary).first(2));
    incremental.update(std::span(binary).subspan(2));
    EXPECT_EQ(incremental.finalize(), [&] {
        Blake3Hasher one_shot;
        one_shot.update(binary);
        return one_shot.finalize();
    }());
    EXPECT_NE(Blake3Hasher::to_hex(incremental.finalize()), official_vectors.front().hash);
}

TEST(Blake3HasherTest, FinalizeDoesNotMutateIncrementalState) {
    constexpr std::array prefix{std::byte{'a'}};
    constexpr std::array suffix{std::byte{'b'}, std::byte{'c'}};
    Blake3Hasher hasher;
    hasher.update(prefix);
    const Blake3Hasher& const_hasher = hasher;
    EXPECT_EQ(const_hasher.finalize(), hasher.finalize());

    hasher.update(suffix);
    EXPECT_EQ(Blake3Hasher::to_hex(hasher.finalize()),
              "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85");
}

TEST(Blake3HasherTest, HexEncodingIsExactlyLowercase) {
    Blake3Hasher::Digest digest{};
    for (std::size_t index = 0; index < digest.size(); ++index) {
        digest[index] = static_cast<std::byte>(index);
    }
    EXPECT_EQ(Blake3Hasher::to_hex(digest),
              "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
}

TEST(Blake3HasherTest, MoveConstructionAndAssignmentPreserveState) {
    static_assert(std::is_nothrow_move_constructible_v<Blake3Hasher>);
    static_assert(std::is_nothrow_move_assignable_v<Blake3Hasher>);
    static_assert(!std::is_copy_constructible_v<Blake3Hasher>);

    constexpr std::array prefix{std::byte{'a'}, std::byte{'b'}};
    constexpr std::array suffix{std::byte{'c'}};
    Blake3Hasher original;
    original.update(prefix);
    Blake3Hasher moved(std::move(original));
    moved.update(suffix);

    Blake3Hasher assigned;
    assigned = std::move(moved);
    EXPECT_EQ(Blake3Hasher::to_hex(assigned.finalize()),
              "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85");
    EXPECT_THROW((void)original.finalize(), std::logic_error);
    EXPECT_THROW(moved.update({}), std::logic_error);
}

} // namespace
} // namespace localvault
