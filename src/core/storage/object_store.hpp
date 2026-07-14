#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "localvault/types.hpp"
#include "storage/zstd_codec.hpp"

namespace localvault {

class MetadataStore;
class FailureInjector;
class ObjectStoreTestAccess;

struct StoredObject {
    std::string hash_hex;
    std::filesystem::path relative_path;
    ByteCount raw_size{};
    ByteCount stored_size{};
    bool newly_stored{};
};

struct ObjectStoreSynchronization final {
    std::array<std::mutex, 256> object_mutexes;
    std::function<void(bool)> stripe_test_hook;
};

class ObjectStore final {
  public:
    static constexpr ByteCount permanent_maximum_chunk_size = 4ULL * 1024ULL * 1024ULL;

    ObjectStore(std::filesystem::path repository_root, MetadataStore& metadata,
                ByteCount maximum_chunk_size, int compression_level,
                std::shared_ptr<FailureInjector> failure_injector,
                std::shared_ptr<ObjectStoreSynchronization> synchronization = {});
    ObjectStore(std::filesystem::path repository_root, ByteCount maximum_chunk_size,
                int compression_level, std::shared_ptr<FailureInjector> failure_injector,
                std::shared_ptr<ObjectStoreSynchronization> synchronization = {});

    [[nodiscard]] static std::filesystem::path object_relative_path(std::string_view hash_hex);
    [[nodiscard]] StoredObject store(std::span<const std::byte> raw_bytes,
                                     std::stop_token stop_token = {});
    void ensure_metadata(const StoredObject& object);
    [[nodiscard]] std::vector<std::byte> read_verified(std::string_view hash_hex,
                                                       const std::filesystem::path& relative_path,
                                                       ByteCount expected_raw_size,
                                                       ByteCount expected_stored_size) const;

  private:
    friend class ObjectStoreTestAccess;

    static constexpr std::size_t maximum_known_objects = 64;

    [[nodiscard]] std::optional<StoredObject>
    find_existing(std::string_view hash_hex, const std::filesystem::path& relative_path,
                  ByteCount raw_size);
    [[nodiscard]] std::optional<StoredObject>
    find_existing_file(std::string_view hash_hex, const std::filesystem::path& relative_path,
                       ByteCount raw_size);
    [[nodiscard]] StoredObject require_reusable(const StoredObject& known,
                                                std::string_view expected_hash,
                                                const std::filesystem::path& expected_relative_path,
                                                ByteCount expected_raw_size) const;
    void remember(const StoredObject& object);

    std::filesystem::path repository_root_;
    MetadataStore* metadata_{};
    std::shared_ptr<FailureInjector> failure_injector_;
    std::shared_ptr<ObjectStoreSynchronization> synchronization_;
    std::size_t maximum_chunk_size_{};
    ZstdCodec codec_;
    std::unordered_map<std::string, StoredObject> known_objects_;
};

} // namespace localvault
