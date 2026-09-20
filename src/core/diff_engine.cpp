#include "localvault/diff_engine.hpp"

#include <limits>
#include <utility>

#include "database/database.hpp"
#include "database/metadata_store.hpp"
#include "database/statement.hpp"
#include "database/transaction.hpp"
#include "localvault/error.hpp"
#include "localvault/repository.hpp"

namespace localvault {
namespace {

void check_cancelled(std::stop_token token) {
    if (token.stop_requested()) {
        throw LocalVaultError(ErrorCode::cancelled, "snapshot diff cancelled");
    }
}

[[nodiscard]] std::filesystem::path utf8_path(std::string_view text) {
    std::u8string encoded;
    encoded.reserve(text.size());
    for (const char character : text) {
        encoded.push_back(static_cast<char8_t>(static_cast<unsigned char>(character)));
    }
    return std::filesystem::path(encoded);
}

template <typename T> [[nodiscard]] T unsigned_column(Statement& query, int index) {
    const auto value = query.column_int64(index);
    if (value < 0 || static_cast<std::uint64_t>(value) > (std::numeric_limits<T>::max)()) {
        throw LocalVaultError(ErrorCode::database_error, "invalid unsigned diff metadata value");
    }
    return static_cast<T>(value);
}

class Cursor final {
  public:
    Cursor(Database& database, SnapshotId id, std::string_view after_path)
        : query_(database.statement(
              "SELECT id, snapshot_id, relative_path, entry_type, logical_size, modified_time_ns, "
              "posix_mode, windows_attributes, file_hash, symlink_target FROM entries "
              "WHERE snapshot_id = :id AND relative_path > :after_path ORDER BY relative_path")) {
        query_.bind(":id", id);
        query_.bind(":after_path", after_path);
    }

    void advance(std::stop_token token) {
        check_cancelled(token);
        entry.reset();
        if (!query_.step()) {
            return;
        }
        EntryInfo value;
        value.id = query_.column_int64(0);
        value.snapshot_id = query_.column_int64(1);
        key = query_.column_text(2);
        value.relative_path = utf8_path(key);
        const auto type = query_.column_text(3);
        if (type == "file") {
            value.type = EntryType::regular_file;
        } else if (type == "directory") {
            value.type = EntryType::directory;
        } else if (type == "symlink") {
            value.type = EntryType::symbolic_link;
        } else {
            throw LocalVaultError(ErrorCode::database_error, "unknown entry type in snapshot diff");
        }
        value.logical_size = unsigned_column<ByteCount>(query_, 4);
        value.modified_time_ns = query_.column_int64(5);
        value.posix_mode = unsigned_column<std::uint32_t>(query_, 6);
        if (!query_.column_is_null(7)) {
            value.windows_attributes = unsigned_column<std::uint32_t>(query_, 7);
        }
        if (!query_.column_is_null(8)) {
            value.file_hash_hex = query_.column_text(8);
        }
        if (!query_.column_is_null(9)) {
            value.symlink_target = utf8_path(query_.column_text(9));
        }
        entry = std::move(value);
    }

    std::string key;
    std::optional<EntryInfo> entry;

  private:
    Statement query_;
};

[[nodiscard]] DiffKind classify(const EntryInfo& before, const EntryInfo& after,
                                const DiffOptions& options) {
    if (before.type != after.type) {
        return DiffKind::type_changed;
    }
    if ((before.type == EntryType::regular_file && before.file_hash_hex != after.file_hash_hex) ||
        (before.type == EntryType::symbolic_link &&
         (before.symlink_target.has_value() != after.symlink_target.has_value() ||
          (before.symlink_target &&
           before.symlink_target->native() != after.symlink_target->native())))) {
        return DiffKind::content_modified;
    }
    const bool directory = before.type == EntryType::directory;
    if (options.compare_metadata &&
        (before.posix_mode != after.posix_mode ||
         (!directory && before.windows_attributes != after.windows_attributes) ||
         ((!directory || !options.ignore_directory_mtime) &&
          before.modified_time_ns != after.modified_time_ns))) {
        return DiffKind::metadata_modified;
    }
    return DiffKind::unchanged;
}

template <typename Callback>
void stream_diff(Database& database, SnapshotId before_id, SnapshotId after_id,
                 const DiffOptions& options, std::string_view after_path, Callback consume,
                 std::stop_token token) {
    check_cancelled(token);
    Transaction transaction(database);
    MetadataStore store(database);
    (void)store.require_complete_snapshot(before_id);
    (void)store.require_complete_snapshot(after_id);
    {
        Cursor before(database, before_id, after_path);
        Cursor after(database, after_id, after_path);
        before.advance(token);
        after.advance(token);
        while (before.entry || after.entry) {
            check_cancelled(token);
            const bool take_before = before.entry && (!after.entry || before.key <= after.key);
            const bool take_after = after.entry && (!before.entry || after.key <= before.key);
            const auto& key = take_before ? before.key : after.key;
            DiffEntry result{
                take_before ? before.entry->relative_path : after.entry->relative_path,
                take_before ? (take_after ? classify(*before.entry, *after.entry, options)
                                          : DiffKind::removed)
                            : DiffKind::added,
                take_before ? before.entry : std::nullopt,
                take_after ? after.entry : std::nullopt,
            };
            if ((options.include_unchanged || result.kind != DiffKind::unchanged) &&
                !consume(result, key)) {
                break;
            }
            if (take_before) {
                before.advance(token);
            }
            if (take_after) {
                after.advance(token);
            }
        }
    }
    check_cancelled(token);
    transaction.commit();
}

} // namespace

void DiffEngine::diff(SnapshotId before_id, SnapshotId after_id, const DiffOptions& options,
                      DiffCallback callback, std::stop_token stop_token) const {
    if (!callback) {
        throw LocalVaultError(ErrorCode::invalid_argument, "snapshot diff requires a callback");
    }
    stream_diff(
        repository_.database(), before_id, after_id, options, {},
        [&callback](const DiffEntry& entry, const std::string&) {
            callback(entry);
            return true;
        },
        stop_token);
}

DiffPage DiffEngine::diff_page(SnapshotId before_id, SnapshotId after_id,
                               const DiffOptions& options, std::size_t page_size,
                               std::string_view after_path, std::stop_token stop_token) const {
    if (page_size == 0) {
        throw LocalVaultError(ErrorCode::invalid_argument, "diff page size must be positive");
    }
    DiffPage page;
    std::string last_key;
    stream_diff(
        repository_.database(), before_id, after_id, options, after_path,
        [&](const DiffEntry& entry, const std::string& key) {
            if (page.entries.size() == page_size) {
                page.next_cursor = last_key;
                return false;
            }
            page.entries.push_back(entry);
            last_key = key;
            return true;
        },
        stop_token);
    return page;
}

} // namespace localvault
