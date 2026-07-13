#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <span>
#include <stop_token>

#include "localvault/types.hpp"

namespace localvault {

using ChunkCallback =
    std::function<void(ByteCount raw_offset, std::span<const std::byte> raw_bytes)>;

class Chunker final {
  public:
    explicit Chunker(ByteCount maximum_chunk_size);

    void for_each_chunk(const std::filesystem::path& source, std::stop_token stop_token,
                        const ChunkCallback& callback) const;
    // Narrow stream seam for deterministic read-failure tests; production uses the path overload.
    void for_each_chunk(std::istream& input, std::stop_token stop_token,
                        const ChunkCallback& callback) const;

  private:
    void for_each_chunk(std::istream& input, const std::filesystem::path& source,
                        std::stop_token stop_token, const ChunkCallback& callback) const;

    std::size_t maximum_chunk_size_{};
};

} // namespace localvault
