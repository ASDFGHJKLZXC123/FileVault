#include "storage/chunker.hpp"

#include <fstream>
#include <limits>
#include <vector>

#include "localvault/error.hpp"

namespace localvault {
namespace {

constexpr ByteCount kMaximumSupportedChunkSize = 4U * 1024U * 1024U;

} // namespace

Chunker::Chunker(ByteCount maximum_chunk_size) {
    if (maximum_chunk_size == 0 || maximum_chunk_size > kMaximumSupportedChunkSize ||
        maximum_chunk_size > static_cast<ByteCount>((std::numeric_limits<std::size_t>::max)()) ||
        maximum_chunk_size >
            static_cast<ByteCount>((std::numeric_limits<std::streamsize>::max)())) {
        throw LocalVaultError(ErrorCode::invalid_argument,
                              "maximum chunk size is outside the supported range");
    }
    maximum_chunk_size_ = static_cast<std::size_t>(maximum_chunk_size);
}

void Chunker::for_each_chunk(const std::filesystem::path& source, std::stop_token stop_token,
                             const ChunkCallback& callback) const {
    if (!callback) {
        throw LocalVaultError(ErrorCode::invalid_argument, "chunk callback must not be empty");
    }
    std::ifstream input(source, std::ios::binary);
    if (!input) {
        throw LocalVaultError(ErrorCode::filesystem_error, "failed to open source file", source);
    }
    for_each_chunk(input, source, stop_token, callback);
}

void Chunker::for_each_chunk(std::istream& input, std::stop_token stop_token,
                             const ChunkCallback& callback) const {
    for_each_chunk(input, {}, stop_token, callback);
}

void Chunker::for_each_chunk(std::istream& input, const std::filesystem::path& source,
                             std::stop_token stop_token, const ChunkCallback& callback) const {
    if (!callback) {
        throw LocalVaultError(ErrorCode::invalid_argument, "chunk callback must not be empty");
    }

    std::vector<std::byte> buffer(maximum_chunk_size_);
    ByteCount raw_offset = 0;
    while (true) {
        if (stop_token.stop_requested()) {
            throw LocalVaultError(ErrorCode::cancelled, "snapshot cancelled", source);
        }
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const std::streamsize read_count = input.gcount();
        const bool reached_eof = input.eof();
        if (read_count < 0 || input.bad() || (input.fail() && !reached_eof)) {
            throw LocalVaultError(ErrorCode::filesystem_error, "failed to read source file",
                                  source);
        }
        if (read_count == 0) {
            if (reached_eof) {
                break;
            }
            throw LocalVaultError(ErrorCode::filesystem_error, "failed to read source file",
                                  source);
        }

        const auto chunk_size = static_cast<std::size_t>(read_count);
        const ByteCount chunk_byte_count = static_cast<ByteCount>(chunk_size);
        if (raw_offset > (std::numeric_limits<ByteCount>::max)() - chunk_byte_count) {
            throw LocalVaultError(ErrorCode::filesystem_error,
                                  "source file size is outside the supported range", source);
        }
        callback(raw_offset, std::span<const std::byte>(buffer.data(), chunk_size));
        raw_offset += chunk_byte_count;

        if (reached_eof) {
            break;
        }
    }
}

} // namespace localvault
