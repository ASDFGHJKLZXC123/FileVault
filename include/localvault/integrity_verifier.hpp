#pragma once

#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

#include "localvault/progress.hpp"
#include "localvault/types.hpp"

namespace localvault {

class Repository;

struct VerificationIssue {
    enum class Kind {
        missing_object,
        corrupt_object,
        invalid_chunk_size,
        invalid_entry_relationship,
        invalid_snapshot_state,
        stale_temporary_file,
        invalid_database,
        invalid_repository_info,
        stale_chunk_metadata,
        orphan_object,
        file_hash_mismatch
    };
    enum class Severity { note, error };

    Kind kind{};
    std::filesystem::path path;
    std::string detail;
    Severity severity{Severity::error};
};

struct VerificationResult {
    std::uint64_t checked_snapshots{};
    std::uint64_t checked_entries{};
    std::uint64_t checked_objects{};
    ByteCount checked_stored_bytes{};
    // Complete regular files attempted and raw bytes successfully reconstructed.
    std::uint64_t checked_files{};
    ByteCount checked_file_bytes{};
    std::vector<VerificationIssue> issues;

    [[nodiscard]] bool ok() const noexcept;
};

class IntegrityVerifier final {
  public:
    explicit IntegrityVerifier(Repository& repository);

    [[nodiscard]] VerificationResult verify(VerifyMode mode, std::stop_token stop_token = {},
                                            ProgressCallback progress = {},
                                            bool verify_files = false);

  private:
    Repository& repository_;
};

} // namespace localvault
