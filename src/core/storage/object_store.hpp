#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "localvault/types.hpp"

namespace localvault {

struct StoredObject {
    std::string hash_hex;
    std::filesystem::path relative_path;
    ByteCount raw_size{};
    ByteCount stored_size{};
    bool newly_stored{};
};

class ObjectStore final {
  public:
    explicit ObjectStore(std::filesystem::path repository_root);

    [[nodiscard]] StoredObject store(std::span<const std::byte> raw_bytes) const;
    [[nodiscard]] std::vector<std::byte> read_verified(std::string_view hash_hex,
                                                       const std::filesystem::path& relative_path,
                                                       ByteCount expected_raw_size,
                                                       ByteCount expected_stored_size) const;

  private:
    std::filesystem::path repository_root_;
};

} // namespace localvault
