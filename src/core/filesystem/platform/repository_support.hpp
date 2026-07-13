#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace localvault {

[[nodiscard]] std::optional<std::string>
platform_filesystem_name(const std::filesystem::path& existing_path);
[[nodiscard]] std::array<std::uint8_t, 16> secure_random_uuid_bytes();
void apply_restrictive_repository_permissions(const std::filesystem::path& root);
void apply_restrictive_file_permissions(const std::filesystem::path& path);
void create_exclusive_file(const std::filesystem::path& path);
[[nodiscard]] bool
repository_storage_is_proven_read_only(const std::filesystem::path& root) noexcept;
void flush_containing_directory(const std::filesystem::path& path);

} // namespace localvault
