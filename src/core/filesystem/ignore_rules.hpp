#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace localvault {

enum class IgnoreCaseSensitivity {
    sensitive,
    insensitive,
};

struct IgnoreMatchResult {
    bool ignored{false};
    bool prune_directory{false};
};

class IgnoreRules final {
  public:
    [[nodiscard]] static IgnoreRules
    load(const std::filesystem::path& source_root,
         const std::optional<std::filesystem::path>& replacement_file,
         IgnoreCaseSensitivity case_sensitivity);

    [[nodiscard]] IgnoreMatchResult match(std::string_view repository_relative_path,
                                          bool is_directory) const;
    [[nodiscard]] bool empty() const noexcept;

  private:
    struct Pattern {
        std::string text;
        bool name_only{false};
        bool directory_only{false};
    };

    IgnoreRules(std::vector<Pattern> patterns, IgnoreCaseSensitivity case_sensitivity);

    std::vector<Pattern> patterns_;
    IgnoreCaseSensitivity case_sensitivity_;
};

} // namespace localvault
