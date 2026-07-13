#include "storage/blake3_hasher.hpp"

#include <blake3.h>

#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace localvault {

static_assert(Blake3Hasher::digest_size == BLAKE3_OUT_LEN);

struct Blake3Hasher::Impl final {
    Impl() {
        blake3_hasher_init(&state);
    }

    blake3_hasher state{};
};

Blake3Hasher::Blake3Hasher() : impl_(std::make_unique<Impl>()) {}

Blake3Hasher::~Blake3Hasher() = default;

Blake3Hasher::Blake3Hasher(Blake3Hasher&&) noexcept = default;

Blake3Hasher& Blake3Hasher::operator=(Blake3Hasher&&) noexcept = default;

void Blake3Hasher::update(std::span<const std::byte> bytes) {
    if (!impl_) {
        throw std::logic_error("cannot update a moved-from BLAKE3 hasher");
    }
    if (!bytes.empty()) {
        blake3_hasher_update(&impl_->state, bytes.data(), bytes.size());
    }
}

Blake3Hasher::Digest Blake3Hasher::finalize() const {
    if (!impl_) {
        throw std::logic_error("cannot finalize a moved-from BLAKE3 hasher");
    }
    Digest digest{};
    blake3_hasher_finalize(&impl_->state, reinterpret_cast<std::uint8_t*>(digest.data()),
                           digest.size());
    return digest;
}

std::string Blake3Hasher::to_hex(const Digest& digest) {
    constexpr std::string_view hexadecimal = "0123456789abcdef";
    std::string result(digest.size() * 2U, '\0');
    for (std::size_t index = 0; index < digest.size(); ++index) {
        const auto value = std::to_integer<unsigned int>(digest[index]);
        result[index * 2U] = hexadecimal[value >> 4U];
        result[index * 2U + 1U] = hexadecimal[value & 0x0FU];
    }
    return result;
}

} // namespace localvault
