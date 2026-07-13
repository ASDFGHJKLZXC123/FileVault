#include "storage/object_store.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <system_error>
#include <utility>

#include "database/metadata_store.hpp"
#include "filesystem/platform/file_metadata.hpp"
#include "filesystem/platform/repository_support.hpp"
#include "localvault/error.hpp"
#include "storage/blake3_hasher.hpp"

namespace localvault {
namespace {

constexpr ByteCount compressed_size_allowance = 64ULL * 1024ULL;

void require_valid_hash(std::string_view hash_hex, ErrorCode error_code) {
    if (hash_hex.size() != Blake3Hasher::digest_size * 2U ||
        !std::ranges::all_of(hash_hex, [](char character) {
            return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
        })) {
        throw LocalVaultError(error_code, "object identifier is not lowercase BLAKE3 hexadecimal");
    }
}

[[nodiscard]] std::filesystem::path derived_path(std::string_view hash_hex) {
    return std::filesystem::path("objects") / std::string(hash_hex.substr(0, 2)) /
           (std::string(hash_hex) + ".zst");
}

[[nodiscard]] std::string hash_raw(std::span<const std::byte> raw_bytes) {
    Blake3Hasher hasher;
    hasher.update(raw_bytes);
    return Blake3Hasher::to_hex(hasher.finalize());
}

[[nodiscard]] std::size_t checked_maximum_chunk_size(ByteCount size) {
    if (size == 0 || size > ObjectStore::permanent_maximum_chunk_size ||
        size > static_cast<ByteCount>((std::numeric_limits<std::size_t>::max)())) {
        throw LocalVaultError(ErrorCode::invalid_argument,
                              "repository chunk-size limit must be between 1 byte and 4 MiB");
    }
    return static_cast<std::size_t>(size);
}

[[nodiscard]] ByteCount checked_byte_count(std::size_t size) {
    if (size > static_cast<std::size_t>((std::numeric_limits<ByteCount>::max)())) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "object size is outside the supported range");
    }
    return static_cast<ByteCount>(size);
}

void ensure_directory(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::create_directories(path, error);
    if (error) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to create object directory: " + error.message(), path);
    }
    const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_directory(status)) {
        throw LocalVaultError(ErrorCode::filesystem_error, "object directory is not a directory",
                              path);
    }
}

[[nodiscard]] std::optional<ByteCount> regular_file_size(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory ||
        status.type() == std::filesystem::file_type::not_found) {
        return std::nullopt;
    }
    if (error) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to inspect stored object: " + error.message(), path);
    }
    if (!std::filesystem::is_regular_file(status)) {
        throw LocalVaultError(ErrorCode::object_corrupt, "stored object path is not a regular file",
                              path);
    }

    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error == std::errc::no_such_file_or_directory) {
        return std::nullopt;
    }
    if (error) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to read stored object size: " + error.message(), path);
    }
    if (size > static_cast<std::uintmax_t>((std::numeric_limits<ByteCount>::max)())) {
        throw LocalVaultError(ErrorCode::object_corrupt,
                              "stored object size is outside the supported range", path);
    }
    return static_cast<ByteCount>(size);
}

[[nodiscard]] std::vector<std::byte> read_exact(const std::filesystem::path& path,
                                                std::size_t size) {
    std::vector<std::byte> bytes(size);
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw LocalVaultError(ErrorCode::object_missing, "stored object cannot be opened", path);
    }
    std::size_t offset = 0;
    constexpr std::size_t maximum_read = 64U * 1024U;
    while (offset < bytes.size()) {
        const std::size_t count = std::min(maximum_read, bytes.size() - offset);
        input.read(reinterpret_cast<char*>(bytes.data() + offset),
                   static_cast<std::streamsize>(count));
        if (input.gcount() != static_cast<std::streamsize>(count)) {
            throw LocalVaultError(ErrorCode::object_corrupt, "stored object is truncated", path);
        }
        offset += count;
    }
    return bytes;
}

[[nodiscard]] std::int64_t now_nanoseconds() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

ObjectStore::ObjectStore(std::filesystem::path repository_root, MetadataStore& metadata,
                         ByteCount maximum_chunk_size, int compression_level)
    : ObjectStore(std::move(repository_root), maximum_chunk_size, compression_level) {
    metadata_ = &metadata;
}

ObjectStore::ObjectStore(std::filesystem::path repository_root, ByteCount maximum_chunk_size,
                         int compression_level)
    : repository_root_(std::move(repository_root)),
      maximum_chunk_size_(checked_maximum_chunk_size(maximum_chunk_size)),
      codec_(compression_level) {}

std::filesystem::path ObjectStore::object_relative_path(std::string_view hash_hex) {
    require_valid_hash(hash_hex, ErrorCode::invalid_argument);
    return derived_path(hash_hex);
}

StoredObject ObjectStore::require_reusable(const StoredObject& known,
                                           std::string_view expected_hash,
                                           const std::filesystem::path& expected_relative_path,
                                           ByteCount expected_raw_size) const {
    const std::filesystem::path final_path = repository_root_ / expected_relative_path;
    if (known.hash_hex != expected_hash ||
        known.relative_path.generic_string() != expected_relative_path.generic_string()) {
        throw LocalVaultError(ErrorCode::object_corrupt,
                              "stored object metadata does not match its BLAKE3 identifier",
                              final_path);
    }
    if (known.raw_size != expected_raw_size) {
        throw LocalVaultError(ErrorCode::object_corrupt,
                              "existing chunk metadata has a conflicting raw size", final_path);
    }
    if (known.stored_size == 0 || known.stored_size > static_cast<ByteCount>(maximum_chunk_size_) +
                                                          compressed_size_allowance) {
        throw LocalVaultError(ErrorCode::object_corrupt,
                              "stored object compressed size exceeds the safety limit", final_path);
    }
    const std::optional<ByteCount> actual_size = regular_file_size(final_path);
    if (!actual_size.has_value()) {
        throw LocalVaultError(ErrorCode::object_missing, "stored object is missing", final_path);
    }
    if (*actual_size != known.stored_size) {
        throw LocalVaultError(ErrorCode::object_corrupt,
                              "stored object size does not match metadata", final_path);
    }
    StoredObject reused = known;
    reused.newly_stored = false;
    return reused;
}

std::optional<StoredObject> ObjectStore::find_existing(std::string_view hash_hex,
                                                       const std::filesystem::path& relative_path,
                                                       ByteCount raw_size) {
    // M3 storage is single-threaded, with no concurrent metadata mutation or GC. Cache hits can
    // therefore skip SQLite, but immutable final-file existence and size are always rechecked.
    const auto cached = known_objects_.find(std::string(hash_hex));
    if (cached != known_objects_.end()) {
        return require_reusable(cached->second, hash_hex, relative_path, raw_size);
    }

    const std::optional<StoredChunkInfo> metadata = metadata_->find_chunk(hash_hex);
    if (metadata.has_value()) {
        StoredObject known{metadata->hash_hex, metadata->object_path, metadata->raw_size,
                           metadata->stored_size, false};
        StoredObject reused = require_reusable(known, hash_hex, relative_path, raw_size);
        known_objects_.insert_or_assign(reused.hash_hex, reused);
        return reused;
    }

    const std::filesystem::path final_path = repository_root_ / relative_path;
    const std::optional<ByteCount> orphan_size = regular_file_size(final_path);
    if (!orphan_size.has_value()) {
        return std::nullopt;
    }
    (void)read_verified(hash_hex, relative_path, raw_size, *orphan_size);
    metadata_->ensure_chunk({
        .hash_hex = std::string(hash_hex),
        .raw_size = raw_size,
        .stored_size = *orphan_size,
        .object_path = relative_path,
        .created_at_ns = now_nanoseconds(),
    });
    StoredObject adopted{std::string(hash_hex), relative_path, raw_size, *orphan_size, false};
    known_objects_.insert_or_assign(adopted.hash_hex, adopted);
    return adopted;
}

StoredObject ObjectStore::store(std::span<const std::byte> raw_bytes) {
    if (metadata_ == nullptr) {
        throw LocalVaultError(ErrorCode::invalid_argument,
                              "object storage requires writable metadata");
    }
    if (raw_bytes.empty()) {
        throw LocalVaultError(ErrorCode::invalid_argument,
                              "empty files must not create stored objects");
    }
    if (raw_bytes.size() > maximum_chunk_size_) {
        throw LocalVaultError(ErrorCode::invalid_argument,
                              "raw chunk exceeds the repository chunk-size limit");
    }

    const ByteCount raw_size = checked_byte_count(raw_bytes.size());
    const std::string hash_hex = hash_raw(raw_bytes);
    const std::filesystem::path relative_path = derived_path(hash_hex);
    if (std::optional<StoredObject> existing = find_existing(hash_hex, relative_path, raw_size)) {
        return std::move(*existing);
    }

    const std::vector<std::byte> compressed = codec_.compress(raw_bytes);
    const ByteCount stored_size = checked_byte_count(compressed.size());
    if (stored_size == 0 ||
        stored_size > static_cast<ByteCount>(maximum_chunk_size_) + compressed_size_allowance) {
        throw LocalVaultError(ErrorCode::compression_error,
                              "compressed object exceeds the configured safety limit");
    }

    const std::filesystem::path final_path = repository_root_ / relative_path;
    ensure_directory(final_path.parent_path());
    const std::filesystem::path temporary_directory = repository_root_ / "temporary" / "objects";
    ensure_directory(temporary_directory);

    TemporaryOutputFile temporary(temporary_directory);
    temporary.write(compressed);
    temporary.sync();
    if (std::optional<StoredObject> existing = find_existing(hash_hex, relative_path, raw_size)) {
        return std::move(*existing);
    }
    const RestorePublishResult publication = temporary.publish(final_path, false);
    if (publication == RestorePublishResult::destination_exists) {
        std::optional<StoredObject> existing = find_existing(hash_hex, relative_path, raw_size);
        if (!existing.has_value()) {
            throw LocalVaultError(ErrorCode::filesystem_error,
                                  "duplicate object disappeared during publication", final_path);
        }
        return std::move(*existing);
    }

    flush_containing_directory(final_path);
    metadata_->ensure_chunk({
        .hash_hex = hash_hex,
        .raw_size = raw_size,
        .stored_size = stored_size,
        .object_path = relative_path,
        .created_at_ns = now_nanoseconds(),
    });
    known_objects_.insert_or_assign(
        hash_hex, StoredObject{hash_hex, relative_path, raw_size, stored_size, false});
    return {hash_hex, relative_path, raw_size, stored_size, true};
}

std::vector<std::byte> ObjectStore::read_verified(std::string_view hash_hex,
                                                  const std::filesystem::path& relative_path,
                                                  ByteCount expected_raw_size,
                                                  ByteCount expected_stored_size) const {
    require_valid_hash(hash_hex, ErrorCode::object_corrupt);
    const std::filesystem::path derived = derived_path(hash_hex);
    if (relative_path.generic_string() != derived.generic_string()) {
        throw LocalVaultError(ErrorCode::object_corrupt,
                              "stored object path does not match its BLAKE3 identifier",
                              relative_path);
    }
    const std::filesystem::path object_path = repository_root_ / derived;
    if (expected_raw_size == 0 || expected_raw_size > maximum_chunk_size_ ||
        expected_raw_size > static_cast<ByteCount>((std::numeric_limits<std::size_t>::max)())) {
        throw LocalVaultError(ErrorCode::object_corrupt,
                              "stored object raw size exceeds the repository chunk-size limit",
                              object_path);
    }
    if (expected_stored_size == 0 ||
        expected_stored_size >
            static_cast<ByteCount>(maximum_chunk_size_) + compressed_size_allowance ||
        expected_stored_size > static_cast<ByteCount>((std::numeric_limits<std::size_t>::max)())) {
        throw LocalVaultError(ErrorCode::object_corrupt,
                              "stored object compressed size exceeds the safety limit",
                              object_path);
    }

    const std::optional<ByteCount> actual_size = regular_file_size(object_path);
    if (!actual_size.has_value()) {
        throw LocalVaultError(ErrorCode::object_missing, "stored object is missing", object_path);
    }
    if (*actual_size != expected_stored_size) {
        throw LocalVaultError(ErrorCode::object_corrupt,
                              "stored object size does not match metadata", object_path);
    }

    const std::vector<std::byte> compressed =
        read_exact(object_path, static_cast<std::size_t>(expected_stored_size));
    std::vector<std::byte> raw;
    try {
        raw = codec_.decompress(compressed, static_cast<std::size_t>(expected_raw_size),
                                maximum_chunk_size_);
    } catch (const LocalVaultError& error) {
        if (error.code() != ErrorCode::compression_error) {
            throw;
        }
        throw LocalVaultError(
            ErrorCode::object_corrupt,
            "stored object failed zstd verification: " + std::string(error.what()), object_path);
    }
    if (hash_raw(raw) != hash_hex) {
        throw LocalVaultError(ErrorCode::object_corrupt, "stored object failed BLAKE3 verification",
                              object_path);
    }
    return raw;
}

} // namespace localvault
