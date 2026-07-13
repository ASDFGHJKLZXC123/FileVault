#include "localvault/restore_engine.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "database/metadata_store.hpp"
#include "filesystem/platform/file_metadata.hpp"
#include "filesystem/platform/path_safety.hpp"
#include "filesystem/platform/platform_lock.hpp"
#include "filesystem/platform/repository_support.hpp"
#include "localvault/error.hpp"
#include "localvault/repository.hpp"
#include "storage/object_store.hpp"

namespace localvault {
namespace {

enum class RestoreAction {
    create,
    merge_directory,
    replace,
    skip,
};

struct PlannedEntry {
    const EntryInfo* entry{};
    RestoreAction action{RestoreAction::create};
    bool metadata_allowed{true};
};

class TemporaryPath final {
  public:
    explicit TemporaryPath(std::filesystem::path path) : path_(std::move(path)) {}
    ~TemporaryPath() noexcept {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    TemporaryPath(const TemporaryPath&) = delete;
    TemporaryPath& operator=(const TemporaryPath&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept {
    const auto byte = [&text](std::size_t index) {
        return static_cast<unsigned char>(text[index]);
    };
    for (std::size_t index = 0; index < text.size();) {
        const unsigned char first = byte(index);
        if (first <= 0x7FU) {
            ++index;
            continue;
        }
        std::size_t length = 0;
        if (first >= 0xC2U && first <= 0xDFU) {
            length = 2;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            length = 3;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            length = 4;
        } else {
            return false;
        }
        if (index + length > text.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset < length; ++offset) {
            if ((byte(index + offset) & 0xC0U) != 0x80U) {
                return false;
            }
        }
        if ((first == 0xE0U && byte(index + 1) < 0xA0U) ||
            (first == 0xEDU && byte(index + 1) > 0x9FU) ||
            (first == 0xF0U && byte(index + 1) < 0x90U) ||
            (first == 0xF4U && byte(index + 1) > 0x8FU)) {
            return false;
        }
        index += length;
    }
    return true;
}

[[nodiscard]] std::string path_key(const std::filesystem::path& path) {
    const std::u8string encoded = path.generic_u8string();
    std::string result;
    result.reserve(encoded.size());
    for (const char8_t character : encoded) {
        result.push_back(static_cast<char>(character));
    }
    return result;
}

[[nodiscard]] std::filesystem::path
normalized_destination(const std::filesystem::path& requested,
                       const std::filesystem::path& repository_root) {
    if (requested.empty()) {
        throw LocalVaultError(ErrorCode::invalid_argument,
                              "restore destination root must not be empty");
    }
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(requested, error);
    if (error) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to make restore destination absolute: " + error.message(),
                              requested);
    }
    const std::filesystem::path lexical = absolute.lexically_normal();

    std::filesystem::path current = lexical.root_path();
    for (const std::filesystem::path& component : lexical.relative_path()) {
        current /= component;
        const NoFollowPathType type = inspect_path_no_follow(current);
        if (type == NoFollowPathType::not_found) {
            break;
        }
        if (type == NoFollowPathType::indirection) {
            throw LocalVaultError(ErrorCode::unsafe_restore_path,
                                  "restore destination has a link or reparse-point ancestor",
                                  current);
        }
        if (type != NoFollowPathType::directory) {
            throw LocalVaultError(ErrorCode::destination_exists,
                                  "restore destination has a non-directory ancestor", current);
        }
    }

    error.clear();
    const std::filesystem::path resolved_destination =
        std::filesystem::weakly_canonical(lexical, error);
    if (error) {
        throw LocalVaultError(
            ErrorCode::filesystem_error,
            "failed to resolve restore destination for containment: " + error.message(), lexical);
    }
    const std::filesystem::path resolved_repository =
        std::filesystem::weakly_canonical(repository_root, error);
    if (error) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to resolve repository root: " + error.message(),
                              repository_root);
    }
    if (platform_path_is_component_prefix(resolved_repository, resolved_destination) ||
        platform_path_is_component_prefix(resolved_destination, resolved_repository)) {
        throw LocalVaultError(ErrorCode::unsafe_restore_path,
                              "restore destination and repository must not contain each other",
                              lexical);
    }
    return lexical;
}

[[nodiscard]] bool is_safe_stored_path(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_path() ||
        path.lexically_normal() != path) {
        return false;
    }
    for (const std::filesystem::path& component : path) {
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
    }
    try {
        return is_valid_utf8(path_key(path));
    } catch (const std::exception&) {
        return false;
    }
}

[[nodiscard]] std::size_t path_depth(const std::filesystem::path& path) {
    return static_cast<std::size_t>(std::distance(path.begin(), path.end()));
}

[[nodiscard]] bool is_component_prefix(const std::filesystem::path& prefix,
                                       const std::filesystem::path& path) {
    auto prefix_component = prefix.begin();
    auto path_component = path.begin();
    while (prefix_component != prefix.end() && path_component != path.end()) {
        if (*prefix_component != *path_component) {
            return false;
        }
        ++prefix_component;
        ++path_component;
    }
    return prefix_component == prefix.end();
}

void validate_entries(const std::vector<EntryInfo>& entries) {
    std::size_t root_count = 0;
    for (const EntryInfo& entry : entries) {
        if (entry.relative_path.empty()) {
            ++root_count;
            if (entry.type != EntryType::directory) {
                throw LocalVaultError(ErrorCode::database_error,
                                      "snapshot root entry is not a directory");
            }
            continue;
        }
        if (!is_safe_stored_path(entry.relative_path)) {
            throw LocalVaultError(ErrorCode::unsafe_restore_path,
                                  "snapshot contains an unsafe stored relative path",
                                  entry.relative_path);
        }
    }
    if (root_count != 1U) {
        throw LocalVaultError(ErrorCode::database_error,
                              "snapshot must contain exactly one root entry");
    }
}

[[nodiscard]] std::vector<const EntryInfo*>
select_entries(const std::vector<EntryInfo>& entries,
               const std::vector<std::filesystem::path>& requested_paths) {
    std::map<std::string, const EntryInfo*> by_path;
    const EntryInfo* root = nullptr;
    for (const EntryInfo& entry : entries) {
        if (entry.relative_path.empty()) {
            root = &entry;
        } else {
            by_path.emplace(path_key(entry.relative_path), &entry);
        }
    }

    if (requested_paths.empty()) {
        std::vector<const EntryInfo*> selected;
        selected.reserve(entries.size());
        for (const EntryInfo& entry : entries) {
            selected.push_back(&entry);
        }
        return selected;
    }

    std::set<std::string> selected_keys;
    for (const std::filesystem::path& requested : requested_paths) {
        if (!is_safe_stored_path(requested)) {
            throw LocalVaultError(ErrorCode::invalid_argument,
                                  "selected restore path must be a safe nonempty relative path",
                                  requested);
        }
        const std::filesystem::path normalized = requested.lexically_normal();
        const auto found = by_path.find(path_key(normalized));
        if (found == by_path.end()) {
            throw LocalVaultError(ErrorCode::invalid_argument,
                                  "selected restore path does not exist in the snapshot",
                                  requested);
        }
        selected_keys.insert(found->first);
        if (found->second->type == EntryType::directory) {
            for (const auto& [candidate_key, candidate] : by_path) {
                if (is_component_prefix(normalized, candidate->relative_path)) {
                    selected_keys.insert(candidate_key);
                }
            }
        }
    }

    std::vector<std::string> descendant_keys(selected_keys.begin(), selected_keys.end());
    for (const std::string& key : descendant_keys) {
        std::filesystem::path parent = by_path.at(key)->relative_path.parent_path();
        while (!parent.empty()) {
            const auto found = by_path.find(path_key(parent));
            if (found == by_path.end() || found->second->type != EntryType::directory) {
                throw LocalVaultError(ErrorCode::database_error,
                                      "selected entry is missing a directory ancestor",
                                      by_path.at(key)->relative_path);
            }
            selected_keys.insert(found->first);
            parent = parent.parent_path();
        }
    }

    std::vector<const EntryInfo*> selected;
    selected.reserve(selected_keys.size() + 1U);
    selected.push_back(root);
    for (const std::string& key : selected_keys) {
        selected.push_back(by_path.at(key));
    }
    return selected;
}

[[nodiscard]] std::optional<std::filesystem::file_status>
existing_status(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory ||
        status.type() == std::filesystem::file_type::not_found) {
        return std::nullopt;
    }
    if (error) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to inspect restore path: " + error.message(), path);
    }
    return status;
}

void require_safe_ancestors(const std::filesystem::path& root,
                            const std::filesystem::path& relative_path) {
    if (inspect_path_no_follow(root) != NoFollowPathType::directory) {
        throw LocalVaultError(ErrorCode::unsafe_restore_path,
                              "restore root is no longer a real directory", root);
    }
    std::filesystem::path current = root;
    for (const std::filesystem::path& component : relative_path.parent_path()) {
        current /= component;
        const NoFollowPathType type = inspect_path_no_follow(current);
        if (type == NoFollowPathType::not_found) {
            break;
        }
        if (type == NoFollowPathType::indirection) {
            throw LocalVaultError(ErrorCode::unsafe_restore_path,
                                  "existing link or reparse-point ancestor makes restore unsafe",
                                  current);
        }
        if (type != NoFollowPathType::directory) {
            throw LocalVaultError(ErrorCode::destination_exists,
                                  "restore path has a non-directory ancestor", current);
        }
    }
}

void require_real_directory(const std::filesystem::path& path) {
    if (inspect_path_no_follow(path) != NoFollowPathType::directory) {
        throw LocalVaultError(ErrorCode::unsafe_restore_path,
                              "restored directory is no longer a real directory", path);
    }
}

[[nodiscard]] RestoreAction conflict_action(const RestoreRequest& request, const EntryInfo& entry,
                                            const std::filesystem::path& destination,
                                            const std::filesystem::file_status& existing) {
    if (entry.type == EntryType::directory && std::filesystem::is_directory(existing) &&
        !std::filesystem::is_symlink(existing)) {
        return RestoreAction::merge_directory;
    }
    if (request.overwrite_policy == OverwritePolicy::never) {
        return RestoreAction::skip;
    }

    ConflictDecision decision = ConflictDecision::replace;
    if (request.overwrite_policy == OverwritePolicy::prompt) {
        if (!request.conflict_resolver) {
            throw LocalVaultError(ErrorCode::invalid_argument,
                                  "prompt overwrite policy requires a conflict resolver");
        }
        decision = request.conflict_resolver(destination, entry.type);
    }
    if (decision == ConflictDecision::cancel) {
        throw LocalVaultError(ErrorCode::cancelled, "restore cancelled during conflict handling",
                              destination);
    }
    if (decision == ConflictDecision::skip) {
        return RestoreAction::skip;
    }
    if (entry.type == EntryType::directory || std::filesystem::is_directory(existing)) {
        throw LocalVaultError(ErrorCode::destination_exists,
                              "replacing a directory with a different entry type is unsupported",
                              destination);
    }
    return RestoreAction::replace;
}

[[nodiscard]] std::vector<PlannedEntry> build_plan(const RestoreRequest& request,
                                                   const std::filesystem::path& root,
                                                   std::vector<const EntryInfo*> selected) {
    std::ranges::sort(selected, [](const EntryInfo* left, const EntryInfo* right) {
        const std::size_t left_depth = path_depth(left->relative_path);
        const std::size_t right_depth = path_depth(right->relative_path);
        return left_depth != right_depth
                   ? left_depth < right_depth
                   : path_key(left->relative_path) < path_key(right->relative_path);
    });

    std::vector<PlannedEntry> plan;
    plan.reserve(selected.size());
    std::vector<std::filesystem::path> skipped_directories;
    for (const EntryInfo* entry : selected) {
        if (entry->relative_path.empty()) {
            plan.push_back({entry, RestoreAction::merge_directory, true});
            continue;
        }
        bool below_skipped_directory = false;
        for (const std::filesystem::path& skipped : skipped_directories) {
            if (is_component_prefix(skipped, entry->relative_path)) {
                below_skipped_directory = true;
                break;
            }
        }
        if (below_skipped_directory) {
            plan.push_back({entry, RestoreAction::skip, false});
            continue;
        }

        require_safe_ancestors(root, entry->relative_path);
        const std::filesystem::path destination = root / entry->relative_path;
        const auto status = existing_status(destination);
        const RestoreAction action = status.has_value()
                                         ? conflict_action(request, *entry, destination, *status)
                                         : RestoreAction::create;
        if (action == RestoreAction::skip && entry->type == EntryType::directory) {
            skipped_directories.push_back(entry->relative_path);
        }
        plan.push_back({entry, action, action != RestoreAction::skip});
    }
    return plan;
}

void check_cancelled(std::stop_token stop_token, const std::filesystem::path& path = {}) {
    if (stop_token.stop_requested()) {
        throw LocalVaultError(ErrorCode::cancelled, "restore cancelled", path);
    }
}

[[nodiscard]] ByteCount checked_add(ByteCount left, ByteCount right,
                                    const std::filesystem::path& path) {
    if (right > (std::numeric_limits<ByteCount>::max)() - left) {
        throw LocalVaultError(ErrorCode::object_corrupt, "restored byte offset overflow", path);
    }
    return left + right;
}

[[nodiscard]] ByteCount maximum_stored_size(ByteCount chunk_size) {
    constexpr ByteCount overhead = 64U * 1024U;
    constexpr ByteCount maximum = (std::numeric_limits<ByteCount>::max)();
    const ByteCount fractional_overhead = chunk_size / 8U;
    if (fractional_overhead > maximum - chunk_size) {
        return maximum;
    }
    const ByteCount subtotal = chunk_size + fractional_overhead;
    if (overhead > maximum - subtotal) {
        return (std::numeric_limits<ByteCount>::max)();
    }
    return subtotal + overhead;
}

void validate_chunks(const EntryInfo& entry, const std::vector<ChunkReferenceInfo>& chunks,
                     ByteCount chunk_size) {
    ByteCount expected_offset = 0;
    std::int64_t expected_sequence = 0;
    const ByteCount stored_limit = maximum_stored_size(chunk_size);
    for (const ChunkReferenceInfo& chunk : chunks) {
        if (chunk.sequence_number != expected_sequence || chunk.raw_offset != expected_offset ||
            chunk.raw_length == 0 || chunk.raw_length != chunk.raw_size ||
            chunk.raw_length > chunk_size || chunk.raw_size > chunk_size ||
            chunk.stored_size == 0 || chunk.stored_size > stored_limit ||
            chunk.stored_size > static_cast<ByteCount>((std::numeric_limits<std::size_t>::max)())) {
            throw LocalVaultError(ErrorCode::object_corrupt,
                                  "entry has invalid or non-contiguous chunk metadata",
                                  entry.relative_path);
        }
        expected_offset = checked_add(expected_offset, chunk.raw_length, entry.relative_path);
        if (expected_sequence == (std::numeric_limits<std::int64_t>::max)()) {
            throw LocalVaultError(ErrorCode::object_corrupt, "entry has too many chunks",
                                  entry.relative_path);
        }
        ++expected_sequence;
    }
    if (expected_offset != entry.logical_size || (entry.logical_size == 0 && !chunks.empty()) ||
        (entry.logical_size != 0 && chunks.empty())) {
        throw LocalVaultError(ErrorCode::object_corrupt,
                              "entry chunks do not match the stored logical size",
                              entry.relative_path);
    }
}

[[nodiscard]] std::filesystem::path
unique_temporary_path(const std::filesystem::path& destination) {
    static std::atomic<std::uint64_t> sequence{0};
    for (int attempt = 0; attempt < 100; ++attempt) {
        const std::string name =
            ".localvault-restore-" + std::to_string(sequence.fetch_add(1)) + ".tmp";
        const std::filesystem::path candidate = destination.parent_path() / name;
        if (!existing_status(candidate).has_value()) {
            return candidate;
        }
    }
    throw LocalVaultError(ErrorCode::filesystem_error,
                          "failed to choose a unique restore temporary path", destination);
}

void report_progress(const ProgressCallback& callback, OperationPhase phase,
                     const std::filesystem::path& path, const RestoreResult& result,
                     std::uint64_t total_entries) {
    if (!callback) {
        return;
    }
    callback(ProgressEvent{
        .phase = phase,
        .current_path = path,
        .processed_entries = result.restored_files + result.restored_directories +
                             result.restored_symlinks + result.skipped_entries.size(),
        .processed_bytes = result.restored_bytes,
        .total_entries = total_entries,
    });
}

} // namespace

RestoreEngine::RestoreEngine(Repository& repository) : repository_(repository) {}

RestoreResult RestoreEngine::restore(const RestoreRequest& request, std::stop_token stop_token,
                                     ProgressCallback progress) {
    if (repository_.open_mode() != OpenMode::read_write) {
        throw LocalVaultError(ErrorCode::invalid_argument,
                              "restore requires a read-write repository", repository_.root());
    }
    if (request.overwrite_policy == OverwritePolicy::prompt && !request.conflict_resolver) {
        throw LocalVaultError(ErrorCode::invalid_argument,
                              "prompt overwrite policy requires a conflict resolver");
    }
    check_cancelled(stop_token, request.destination_root);
    const std::filesystem::path destination_root =
        normalized_destination(request.destination_root, repository_.root());

    RepositoryLock writer_lock =
        RepositoryLock::acquire_exclusive(repository_.root() / "repository.lock");
    MetadataStore metadata(repository_.database());
    (void)metadata.require_complete_snapshot(request.snapshot_id);
    const std::vector<EntryInfo> entries = metadata.list_entries(request.snapshot_id);
    validate_entries(entries);
    std::vector<const EntryInfo*> selected = select_entries(entries, request.relative_paths);

    std::map<std::int64_t, std::vector<ChunkReferenceInfo>> chunks_by_entry;
    for (const EntryInfo* entry : selected) {
        if (entry->type != EntryType::regular_file) {
            continue;
        }
        std::vector<ChunkReferenceInfo> chunks = metadata.list_entry_chunks(entry->id);
        validate_chunks(*entry, chunks, repository_.info().chunk_size_bytes);
        chunks_by_entry.emplace(entry->id, std::move(chunks));
    }

    std::error_code error;
    std::filesystem::create_directories(destination_root, error);
    if (error) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to create restore destination: " + error.message(),
                              destination_root);
    }
    std::vector<PlannedEntry> plan = build_plan(request, destination_root, std::move(selected));
    const std::uint64_t total_entries = plan.size();
    RestoreResult result;
    ObjectStore objects(repository_.root());
    report_progress(progress, OperationPhase::restoring, destination_root, result, total_entries);

    for (const PlannedEntry& planned : plan) {
        check_cancelled(stop_token, planned.entry->relative_path);
        if (planned.entry->type != EntryType::directory) {
            continue;
        }
        if (planned.action == RestoreAction::skip) {
            result.skipped_entries.push_back(
                {planned.entry->relative_path, "destination directory conflicts"});
            continue;
        }
        const std::filesystem::path destination = destination_root / planned.entry->relative_path;
        if (planned.action == RestoreAction::create) {
            require_safe_ancestors(destination_root, planned.entry->relative_path);
            if (!std::filesystem::create_directory(destination, error) || error) {
                throw LocalVaultError(ErrorCode::filesystem_error,
                                      "failed to create restored directory: " + error.message(),
                                      destination);
            }
        }
        ++result.restored_directories;
    }

    for (const PlannedEntry& planned : plan) {
        const EntryInfo& entry = *planned.entry;
        if (entry.type != EntryType::regular_file) {
            continue;
        }
        check_cancelled(stop_token, entry.relative_path);
        if (planned.action == RestoreAction::skip) {
            result.skipped_entries.push_back({entry.relative_path, "destination file conflicts"});
            continue;
        }
        const std::filesystem::path destination = destination_root / entry.relative_path;
        require_safe_ancestors(destination_root, entry.relative_path);
        TemporaryOutputFile output(destination.parent_path());
        ByteCount written = 0;
        for (const ChunkReferenceInfo& chunk : chunks_by_entry.at(entry.id)) {
            check_cancelled(stop_token, entry.relative_path);
            const std::vector<std::byte> raw = objects.read_verified(
                chunk.hash_hex, chunk.object_path, chunk.raw_size, chunk.stored_size);
            if (raw.size() != static_cast<std::size_t>(chunk.raw_length)) {
                throw LocalVaultError(ErrorCode::object_corrupt,
                                      "verified object size does not match entry chunk",
                                      entry.relative_path);
            }
            output.write(raw);
            written = checked_add(written, chunk.raw_length, entry.relative_path);
        }
        if (written != entry.logical_size) {
            throw LocalVaultError(ErrorCode::object_corrupt,
                                  "restored file size does not match snapshot metadata",
                                  entry.relative_path);
        }
        output.sync();
        std::optional<std::string> metadata_warning;
        try {
            output.apply_metadata(entry.modified_time_ns, entry.posix_mode);
        } catch (const LocalVaultError& metadata_error) {
            metadata_warning = "content restored; metadata not fully restored: " +
                               std::string(metadata_error.what());
        }
        require_safe_ancestors(destination_root, entry.relative_path);
        const RestorePublishResult published =
            output.publish(destination, planned.action == RestoreAction::replace);
        if (published == RestorePublishResult::destination_exists) {
            result.skipped_entries.push_back(
                {entry.relative_path, "destination appeared during restore"});
            continue;
        }
        flush_containing_directory(destination);
        ++result.restored_files;
        result.restored_bytes =
            checked_add(result.restored_bytes, entry.logical_size, entry.relative_path);
        if (metadata_warning.has_value()) {
            result.skipped_entries.push_back({entry.relative_path, *metadata_warning});
        }
        report_progress(progress, OperationPhase::restoring, entry.relative_path, result,
                        total_entries);
    }

    for (const PlannedEntry& planned : plan) {
        const EntryInfo& entry = *planned.entry;
        if (entry.type != EntryType::symbolic_link) {
            continue;
        }
        check_cancelled(stop_token, entry.relative_path);
        if (planned.action == RestoreAction::skip) {
            result.skipped_entries.push_back(
                {entry.relative_path, "destination symbolic link conflicts"});
            continue;
        }
        if (!entry.symlink_target.has_value()) {
            throw LocalVaultError(ErrorCode::database_error,
                                  "symbolic-link entry has no saved target", entry.relative_path);
        }
        const std::filesystem::path destination = destination_root / entry.relative_path;
        require_safe_ancestors(destination_root, entry.relative_path);
        TemporaryPath temporary(unique_temporary_path(destination));
        if (!create_restored_symlink(*entry.symlink_target, temporary.path())) {
            result.skipped_entries.push_back(
                {entry.relative_path, "symbolic-link creation privilege is unavailable"});
            continue;
        }
        require_safe_ancestors(destination_root, entry.relative_path);
        const RestorePublishResult published = publish_restored_path(
            temporary.path(), destination, planned.action == RestoreAction::replace);
        if (published == RestorePublishResult::destination_exists) {
            result.skipped_entries.push_back(
                {entry.relative_path, "destination appeared during restore"});
            continue;
        }
        flush_containing_directory(destination);
        ++result.restored_symlinks;
    }

    std::ranges::sort(plan, [](const PlannedEntry& left, const PlannedEntry& right) {
        const std::size_t left_depth = path_depth(left.entry->relative_path);
        const std::size_t right_depth = path_depth(right.entry->relative_path);
        return left_depth != right_depth
                   ? left_depth > right_depth
                   : path_key(left.entry->relative_path) < path_key(right.entry->relative_path);
    });
    for (const PlannedEntry& planned : plan) {
        if (planned.entry->type != EntryType::directory || !planned.metadata_allowed) {
            continue;
        }
        const std::filesystem::path destination = destination_root / planned.entry->relative_path;
        require_safe_ancestors(destination_root, planned.entry->relative_path);
        require_real_directory(destination);
        try {
            apply_restored_metadata(destination, planned.entry->modified_time_ns,
                                    planned.entry->posix_mode);
        } catch (const LocalVaultError& metadata_error) {
            result.skipped_entries.push_back(
                {planned.entry->relative_path, "directory restored; metadata not fully restored: " +
                                                   std::string(metadata_error.what())});
        }
    }

    report_progress(progress, OperationPhase::complete, destination_root, result, total_entries);
    (void)writer_lock;
    (void)request.verify_final_file_hash;
    return result;
}

} // namespace localvault
