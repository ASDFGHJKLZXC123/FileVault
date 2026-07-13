#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace localvault {

class ZstdCodec final {
  public:
    explicit ZstdCodec(int compression_level);

    [[nodiscard]] std::vector<std::byte> compress(std::span<const std::byte> raw) const;
    [[nodiscard]] std::vector<std::byte> decompress(std::span<const std::byte> compressed,
                                                    std::size_t expected_raw_size,
                                                    std::size_t maximum_raw_size) const;

  private:
    int compression_level_;
};

} // namespace localvault
