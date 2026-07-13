#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "localvault/types.hpp"
#include "storage/zstd_codec.hpp"

namespace localvault {

class MetadataStore;

struct StoredObject {
    std::string hash_hex;
    std::filesystem::path relative_path;
    ByteCount raw_size{};
    ByteCount stored_size{};
    bool newly_stored{};
};

class ObjectStore final {
  public:
    static constexpr ByteCount permanent_maximum_chunk_size = 4ULL * 1024ULL * 1024ULL;

    ObjectStore(std::filesystem::path repository_root, MetadataStore& metadata,
                ByteCount maximum_chunk_size, int compression_level);
    ObjectStore(std::filesystem::path repository_root, ByteCount maximum_chunk_size,
                int compression_level);

    [[nodiscard]] static std::filesystem::path object_relative_path(std::string_view hash_hex);
    [[nodiscard]] StoredObject store(std::span<const std::byte> raw_bytes);
    [[nodiscard]] std::vector<std::byte> read_verified(std::string_view hash_hex,
                                                       const std::filesystem::path& relative_path,
                                                       ByteCount expected_raw_size,
                                                       ByteCount expected_stored_size) const;

  private:
    [[nodiscard]] std::optional<StoredObject>
    find_existing(std::string_view hash_hex, const std::filesystem::path& relative_path,
                  ByteCount raw_size);
    [[nodiscard]] StoredObject require_reusable(const StoredObject& known,
                                                std::string_view expected_hash,
                                                const std::filesystem::path& expected_relative_path,
                                                ByteCount expected_raw_size) const;

    std::filesystem::path repository_root_;
    MetadataStore* metadata_{};
    std::size_t maximum_chunk_size_{};
    ZstdCodec codec_;
    // M3 is single-threaded. M5 adds synchronization; do not add stripe locks here.
    std::unordered_map<std::string, StoredObject> known_objects_;
};

} // namespace localvault
