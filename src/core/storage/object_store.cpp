#include "storage/object_store.hpp"

#include <blake3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>

#include "filesystem/platform/repository_support.hpp"
#include "localvault/error.hpp"

namespace localvault {
namespace {

constexpr std::size_t digest_size = BLAKE3_OUT_LEN;

[[nodiscard]] std::string hash_bytes(std::span<const std::byte> bytes) {
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    if (!bytes.empty()) {
        blake3_hasher_update(&hasher, bytes.data(), bytes.size());
    }
    std::array<std::uint8_t, digest_size> digest{};
    blake3_hasher_finalize(&hasher, digest.data(), digest.size());

    constexpr std::string_view hexadecimal = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2U);
    for (const std::uint8_t byte : digest) {
        result.push_back(hexadecimal[byte >> 4U]);
        result.push_back(hexadecimal[byte & 0x0FU]);
    }
    return result;
}

[[nodiscard]] bool is_valid_hash(std::string_view hash) {
    if (hash.size() != digest_size * 2U) {
        return false;
    }
    return std::ranges::all_of(hash, [](char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

[[nodiscard]] std::filesystem::path object_relative_path(std::string_view hash) {
    return std::filesystem::path("objects") / std::string(hash.substr(0, 2)) /
           (std::string(hash) + ".raw");
}

[[nodiscard]] ByteCount checked_byte_count(std::size_t size) {
    if (size > static_cast<std::size_t>((std::numeric_limits<ByteCount>::max)())) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "object size is outside the supported range");
    }
    return static_cast<ByteCount>(size);
}

[[nodiscard]] std::vector<std::byte> read_file(const std::filesystem::path& path,
                                               ByteCount expected_size) {
    if (expected_size > static_cast<ByteCount>((std::numeric_limits<std::size_t>::max)())) {
        throw LocalVaultError(ErrorCode::object_corrupt,
                              "stored object is larger than the supported memory range", path);
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(expected_size));
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

[[nodiscard]] std::filesystem::path
create_unique_temporary_file(const std::filesystem::path& directory, std::string_view hash) {
    static std::atomic<std::uint64_t> sequence{0};
    for (int attempt = 0; attempt < 100; ++attempt) {
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::filesystem::path candidate =
            directory / (std::string(hash) + "." + std::to_string(timestamp) + "." +
                         std::to_string(sequence.fetch_add(1)) + ".tmp");
        try {
            create_exclusive_file(candidate);
        } catch (const LocalVaultError&) {
            std::error_code exists_error;
            if (!std::filesystem::exists(candidate, exists_error) || exists_error) {
                throw;
            }
            continue;
        }
        try {
            apply_restrictive_file_permissions(candidate);
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(candidate, ignored);
            throw;
        }
        return candidate;
    }
    throw LocalVaultError(ErrorCode::filesystem_error,
                          "failed to create a unique temporary object file", directory);
}

void ensure_directory(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::create_directories(path, error);
    if (error) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to create object directory: " + error.message(), path);
    }
}

class TemporaryObject final {
  public:
    explicit TemporaryObject(std::filesystem::path path) : path_(std::move(path)) {}
    ~TemporaryObject() {
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }

    TemporaryObject(const TemporaryObject&) = delete;
    TemporaryObject& operator=(const TemporaryObject&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }
    void published() noexcept {
        path_.clear();
    }

  private:
    std::filesystem::path path_;
};

} // namespace

ObjectStore::ObjectStore(std::filesystem::path repository_root)
    : repository_root_(std::move(repository_root)) {}

StoredObject ObjectStore::store(std::span<const std::byte> raw_bytes) const {
    if (raw_bytes.empty()) {
        throw LocalVaultError(ErrorCode::invalid_argument,
                              "empty files must not create stored objects");
    }
    const std::string hash = hash_bytes(raw_bytes);
    const std::filesystem::path relative_path = object_relative_path(hash);
    const std::filesystem::path final_path = repository_root_ / relative_path;
    const ByteCount raw_size = checked_byte_count(raw_bytes.size());

    std::error_code status_error;
    const std::filesystem::file_status existing_status =
        std::filesystem::symlink_status(final_path, status_error);
    if (!status_error && existing_status.type() != std::filesystem::file_type::not_found) {
        if (!std::filesystem::is_regular_file(existing_status)) {
            throw LocalVaultError(ErrorCode::object_corrupt,
                                  "stored object path is not a regular file", final_path);
        }
        (void)read_verified(hash, relative_path, raw_size, raw_size);
        return {hash, relative_path, raw_size, raw_size, false};
    }
    if (status_error && status_error != std::errc::no_such_file_or_directory) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to inspect object path: " + status_error.message(),
                              final_path);
    }

    ensure_directory(final_path.parent_path());
    const std::filesystem::path temporary_directory = repository_root_ / "temporary" / "objects";
    ensure_directory(temporary_directory);
    TemporaryObject temporary(create_unique_temporary_file(temporary_directory, hash));
    {
        std::ofstream output(temporary.path(), std::ios::binary | std::ios::trunc);
        if (!output) {
            throw LocalVaultError(ErrorCode::filesystem_error,
                                  "failed to open temporary object file", temporary.path());
        }
        std::size_t offset = 0;
        constexpr std::size_t maximum_write = 64U * 1024U;
        while (offset < raw_bytes.size()) {
            const std::size_t count = std::min(maximum_write, raw_bytes.size() - offset);
            output.write(reinterpret_cast<const char*>(raw_bytes.data() + offset),
                         static_cast<std::streamsize>(count));
            if (!output) {
                throw LocalVaultError(ErrorCode::filesystem_error,
                                      "failed to write temporary object file", temporary.path());
            }
            offset += count;
        }
        output.close();
        if (!output) {
            throw LocalVaultError(ErrorCode::filesystem_error,
                                  "failed to close temporary object file", temporary.path());
        }
    }

    std::error_code rename_error;
    std::filesystem::rename(temporary.path(), final_path, rename_error);
    if (rename_error) {
        std::error_code final_error;
        const auto final_status = std::filesystem::symlink_status(final_path, final_error);
        if (!final_error && std::filesystem::is_regular_file(final_status)) {
            (void)read_verified(hash, relative_path, raw_size, raw_size);
            return {hash, relative_path, raw_size, raw_size, false};
        }
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to publish object: " + rename_error.message(), final_path);
    }
    temporary.published();
    return {hash, relative_path, raw_size, raw_size, true};
}

std::vector<std::byte> ObjectStore::read_verified(std::string_view hash_hex,
                                                  const std::filesystem::path& relative_path,
                                                  ByteCount expected_raw_size,
                                                  ByteCount expected_stored_size) const {
    if (!is_valid_hash(hash_hex)) {
        throw LocalVaultError(ErrorCode::object_corrupt,
                              "stored object identifier is not lowercase BLAKE3 hexadecimal");
    }
    const std::filesystem::path derived = object_relative_path(hash_hex);
    if (relative_path.generic_string() != derived.generic_string()) {
        throw LocalVaultError(ErrorCode::object_corrupt,
                              "stored object path does not match its BLAKE3 identifier",
                              relative_path);
    }
    if (expected_raw_size != expected_stored_size) {
        throw LocalVaultError(ErrorCode::object_corrupt,
                              "raw M2 object has conflicting raw and stored sizes",
                              repository_root_ / derived);
    }
    if (expected_raw_size == 0) {
        throw LocalVaultError(ErrorCode::object_corrupt,
                              "empty files must not reference stored objects",
                              repository_root_ / derived);
    }

    const std::filesystem::path object_path = repository_root_ / derived;
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::symlink_status(object_path, error);
    if (error == std::errc::no_such_file_or_directory ||
        status.type() == std::filesystem::file_type::not_found) {
        throw LocalVaultError(ErrorCode::object_missing, "stored object is missing", object_path);
    }
    if (error) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to inspect stored object: " + error.message(), object_path);
    }
    if (!std::filesystem::is_regular_file(status)) {
        throw LocalVaultError(ErrorCode::object_corrupt, "stored object path is not a regular file",
                              object_path);
    }
    const std::uintmax_t actual_size = std::filesystem::file_size(object_path, error);
    if (error) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to read stored object size: " + error.message(), object_path);
    }
    if (actual_size != expected_stored_size) {
        throw LocalVaultError(ErrorCode::object_corrupt,
                              "stored object size does not match metadata", object_path);
    }

    std::vector<std::byte> bytes = read_file(object_path, expected_stored_size);
    if (bytes.size() != expected_raw_size || hash_bytes(bytes) != hash_hex) {
        throw LocalVaultError(ErrorCode::object_corrupt, "stored object failed BLAKE3 verification",
                              object_path);
    }
    return bytes;
}

} // namespace localvault
