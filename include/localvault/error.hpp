#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

namespace localvault {

enum class ErrorCode {
    invalid_argument,
    repository_not_found,
    invalid_repository,
    unsupported_repository_version,
    repository_busy,
    filesystem_error,
    database_error,
    compression_error,
    hashing_error,
    object_missing,
    object_corrupt,
    unsafe_restore_path,
    destination_exists,
    source_changed,
    cancelled,
    partial_success,
    internal_error
};

class LocalVaultError final : public std::runtime_error {
  public:
    LocalVaultError(ErrorCode code, std::string message, std::filesystem::path path = {})
        : std::runtime_error(std::move(message)), code_(code), path_(std::move(path)) {}

    [[nodiscard]] ErrorCode code() const noexcept {
        return code_;
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    ErrorCode code_;
    std::filesystem::path path_;
};

} // namespace localvault
