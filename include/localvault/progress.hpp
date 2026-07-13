#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace localvault {

enum class OperationPhase {
    preparing,
    scanning,
    reading,
    hashing,
    compressing,
    writing_objects,
    writing_metadata,
    restoring,
    verifying,
    garbage_collecting,
    finalizing,
    complete
};

struct ProgressEvent {
    OperationPhase phase{OperationPhase::preparing};
    std::filesystem::path current_path;
    std::uint64_t discovered_entries{};
    std::uint64_t processed_entries{};
    std::uint64_t processed_bytes{};
    std::optional<std::uint64_t> total_entries;
    std::optional<std::uint64_t> total_bytes;
    std::uint64_t new_chunks{};
    std::uint64_t reused_chunks{};
    std::string message;
};

using ProgressCallback = std::function<void(const ProgressEvent&)>;

} // namespace localvault
