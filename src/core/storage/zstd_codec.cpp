#include "storage/zstd_codec.hpp"

#include <string>
#include <string_view>

#include <zstd.h>

#include "localvault/error.hpp"

namespace localvault {
namespace {

[[noreturn]] void throw_zstd_error(std::string_view operation, std::size_t result) {
    throw LocalVaultError(ErrorCode::compression_error,
                          std::string(operation) + " failed: " + ZSTD_getErrorName(result));
}

[[nodiscard]] std::size_t compression_bound(std::size_t raw_size) {
    const std::size_t bound = ZSTD_compressBound(raw_size);
    if (ZSTD_isError(bound) != 0U) {
        throw_zstd_error("zstd compression bound calculation", bound);
    }
    return bound;
}

} // namespace

ZstdCodec::ZstdCodec(int compression_level) : compression_level_(compression_level) {}

std::vector<std::byte> ZstdCodec::compress(std::span<const std::byte> raw) const {
    std::vector<std::byte> compressed(compression_bound(raw.size()));
    const std::size_t compressed_size = ZSTD_compress(compressed.data(), compressed.size(),
                                                      raw.data(), raw.size(), compression_level_);
    if (ZSTD_isError(compressed_size) != 0U) {
        throw_zstd_error("zstd compression", compressed_size);
    }
    compressed.resize(compressed_size);
    return compressed;
}

std::vector<std::byte> ZstdCodec::decompress(std::span<const std::byte> compressed,
                                             std::size_t expected_raw_size,
                                             std::size_t maximum_raw_size) const {
    if (expected_raw_size > maximum_raw_size) {
        throw LocalVaultError(ErrorCode::compression_error,
                              "expected raw size exceeds repository chunk-size limit");
    }

    const std::size_t maximum_compressed_size = compression_bound(maximum_raw_size);
    if (compressed.size() > maximum_compressed_size) {
        throw LocalVaultError(ErrorCode::compression_error,
                              "compressed input exceeds configured safety limit");
    }

    const std::size_t frame_size =
        ZSTD_findFrameCompressedSize(compressed.data(), compressed.size());
    if (ZSTD_isError(frame_size) != 0U) {
        throw_zstd_error("zstd frame validation", frame_size);
    }
    if (frame_size != compressed.size()) {
        throw LocalVaultError(ErrorCode::compression_error,
                              "compressed input must contain exactly one zstd frame");
    }

    std::vector<std::byte> raw(expected_raw_size);
    const std::size_t raw_size =
        ZSTD_decompress(raw.data(), raw.size(), compressed.data(), compressed.size());
    if (ZSTD_isError(raw_size) != 0U) {
        throw_zstd_error("zstd decompression", raw_size);
    }
    if (raw_size != expected_raw_size) {
        throw LocalVaultError(ErrorCode::compression_error,
                              "decompressed size does not match expected raw size");
    }
    return raw;
}

} // namespace localvault
