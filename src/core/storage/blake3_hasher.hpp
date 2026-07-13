#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string>

namespace localvault {

class Blake3Hasher final {
  public:
    static constexpr std::size_t digest_size = 32;
    using Digest = std::array<std::byte, digest_size>;

    Blake3Hasher();
    ~Blake3Hasher();

    Blake3Hasher(Blake3Hasher&&) noexcept;
    Blake3Hasher& operator=(Blake3Hasher&&) noexcept;

    Blake3Hasher(const Blake3Hasher&) = delete;
    Blake3Hasher& operator=(const Blake3Hasher&) = delete;

    void update(std::span<const std::byte> bytes);
    [[nodiscard]] Digest finalize() const;
    [[nodiscard]] static std::string to_hex(const Digest& digest);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace localvault
