#include "filesystem/ignore_rules.hpp"

#include <algorithm>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

#include "localvault/error.hpp"

namespace localvault {
namespace {

[[nodiscard]] std::string normalize_separators(std::string_view path) {
    std::string normalized(path);
    for (char& character : normalized) {
        if (character == '\\') {
            character = '/';
        }
    }
    return normalized;
}

[[nodiscard]] bool equal_literal(char left, char right,
                                 IgnoreCaseSensitivity case_sensitivity) noexcept {
    if (left == right || case_sensitivity == IgnoreCaseSensitivity::sensitive) {
        return left == right;
    }
    const auto fold_ascii = [](char character) {
        return character >= 'A' && character <= 'Z' ? static_cast<char>(character + ('a' - 'A'))
                                                    : character;
    };
    return fold_ascii(left) == fold_ascii(right);
}

[[nodiscard]] std::size_t next_character(std::string_view text, std::size_t offset) noexcept {
    if (offset >= text.size()) {
        return offset;
    }
    const unsigned char first = static_cast<unsigned char>(text[offset]);
    std::size_t length = 1;
    if ((first & 0xE0U) == 0xC0U) {
        length = 2;
    } else if ((first & 0xF0U) == 0xE0U) {
        length = 3;
    } else if ((first & 0xF8U) == 0xF0U) {
        length = 4;
    }
    return offset + std::min(length, text.size() - offset);
}

[[nodiscard]] bool match_component(std::string_view pattern, std::string_view name,
                                   IgnoreCaseSensitivity case_sensitivity) noexcept {
    std::size_t pattern_offset = 0;
    std::size_t name_offset = 0;
    std::size_t star_pattern_offset = std::string_view::npos;
    std::size_t star_name_offset = 0;

    while (name_offset < name.size()) {
        if (pattern_offset < pattern.size() && pattern[pattern_offset] == '?') {
            ++pattern_offset;
            name_offset = next_character(name, name_offset);
        } else if (pattern_offset < pattern.size() && pattern[pattern_offset] == '*') {
            star_pattern_offset = ++pattern_offset;
            star_name_offset = name_offset;
        } else if (pattern_offset < pattern.size() &&
                   equal_literal(pattern[pattern_offset], name[name_offset], case_sensitivity)) {
            ++pattern_offset;
            ++name_offset;
        } else if (star_pattern_offset != std::string_view::npos) {
            pattern_offset = star_pattern_offset;
            star_name_offset = next_character(name, star_name_offset);
            name_offset = star_name_offset;
        } else {
            return false;
        }
    }
    while (pattern_offset < pattern.size() && pattern[pattern_offset] == '*') {
        ++pattern_offset;
    }
    return pattern_offset == pattern.size();
}

[[nodiscard]] bool match_path(std::string_view pattern, std::string_view path,
                              IgnoreCaseSensitivity case_sensitivity) noexcept {
    std::size_t pattern_offset = 0;
    std::size_t path_offset = 0;
    while (true) {
        const std::size_t pattern_separator = pattern.find('/', pattern_offset);
        const std::size_t path_separator = path.find('/', path_offset);
        const std::string_view pattern_component =
            pattern.substr(pattern_offset, pattern_separator == std::string_view::npos
                                               ? pattern.size() - pattern_offset
                                               : pattern_separator - pattern_offset);
        const std::string_view path_component = path.substr(
            path_offset, path_separator == std::string_view::npos ? path.size() - path_offset
                                                                  : path_separator - path_offset);
        if (!match_component(pattern_component, path_component, case_sensitivity)) {
            return false;
        }
        if (pattern_separator == std::string_view::npos ||
            path_separator == std::string_view::npos) {
            return pattern_separator == path_separator;
        }
        pattern_offset = pattern_separator + 1;
        path_offset = path_separator + 1;
    }
}

[[nodiscard]] std::string_view name_of(std::string_view path) noexcept {
    const std::size_t separator = path.rfind('/');
    return path.substr(separator == std::string_view::npos ? 0 : separator + 1);
}

[[nodiscard]] std::vector<std::string> read_lines(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw LocalVaultError(ErrorCode::filesystem_error, "failed to open ignore file", path);
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        lines.push_back(std::move(line));
    }
    if (input.bad()) {
        throw LocalVaultError(ErrorCode::filesystem_error, "failed to read ignore file", path);
    }
    return lines;
}

} // namespace

IgnoreRules::IgnoreRules(std::vector<Pattern> patterns, IgnoreCaseSensitivity case_sensitivity)
    : patterns_(std::move(patterns)), case_sensitivity_(case_sensitivity) {}

IgnoreRules IgnoreRules::load(const std::filesystem::path& source_root,
                              const std::optional<std::filesystem::path>& replacement_file,
                              IgnoreCaseSensitivity case_sensitivity) {
    const std::filesystem::path path =
        replacement_file.has_value() ? *replacement_file : source_root / ".localvaultignore";
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::status(path, error);
    if (error) {
        if (!replacement_file.has_value() && error == std::errc::no_such_file_or_directory) {
            return IgnoreRules({}, case_sensitivity);
        }
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to inspect ignore file: " + error.message(), path);
    }
    if (!std::filesystem::exists(status)) {
        if (!replacement_file.has_value()) {
            return IgnoreRules({}, case_sensitivity);
        }
        throw LocalVaultError(ErrorCode::filesystem_error, "ignore file does not exist", path);
    }
    if (!std::filesystem::is_regular_file(status)) {
        throw LocalVaultError(ErrorCode::filesystem_error, "ignore file is not a regular file",
                              path);
    }
    std::vector<Pattern> patterns;
    for (std::string& line : read_lines(path)) {
        line = normalize_separators(line);
        const bool directory_only = !line.empty() && line.back() == '/';
        if (directory_only) {
            line.pop_back();
        }
        if (!line.empty()) {
            const bool name_only = line.find('/') == std::string::npos;
            patterns.push_back({std::move(line), name_only, directory_only});
        }
    }
    return IgnoreRules(std::move(patterns), case_sensitivity);
}

IgnoreMatchResult IgnoreRules::match(std::string_view repository_relative_path,
                                     bool is_directory) const {
    const std::string normalized = normalize_separators(repository_relative_path);
    for (const Pattern& pattern : patterns_) {
        if (pattern.directory_only && !is_directory) {
            continue;
        }
        const std::string_view candidate = pattern.name_only ? name_of(normalized) : normalized;
        if (match_path(pattern.text, candidate, case_sensitivity_)) {
            return {.ignored = true, .prune_directory = is_directory};
        }
    }
    return {};
}

bool IgnoreRules::empty() const noexcept {
    return patterns_.empty();
}

} // namespace localvault
