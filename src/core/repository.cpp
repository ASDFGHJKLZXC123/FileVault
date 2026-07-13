#include "localvault/repository.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "database/database.hpp"
#include "database/migrations.hpp"
#include "database/statement.hpp"
#include "database/transaction.hpp"
#include "filesystem/creation_ledger.hpp"
#include "filesystem/filesystem_classifier.hpp"
#include "filesystem/platform/platform_lock.hpp"
#include "filesystem/platform/repository_support.hpp"
#include "localvault/error.hpp"
#include "localvault/version.hpp"

namespace localvault {
namespace {

constexpr std::uint32_t current_format_version = 1;

[[nodiscard]] std::filesystem::path normalized_root(const std::filesystem::path& root) {
    if (root.empty()) {
        throw LocalVaultError(ErrorCode::invalid_argument, "repository root path must not be empty",
                              root);
    }
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(root, error);
    if (error) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to make repository path absolute: " + error.message(), root);
    }
    const std::filesystem::path normalized = absolute.lexically_normal();
    if (normalized == normalized.root_path()) {
        throw LocalVaultError(ErrorCode::invalid_argument,
                              "a filesystem root cannot be used as a repository root", normalized);
    }
    return normalized;
}

[[nodiscard]] std::int64_t utc_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] std::string make_uuid_v4() {
    std::array<std::uint8_t, 16> bytes = secure_random_uuid_bytes();
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x40U);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U);

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            output << '-';
        }
        output << std::setw(2) << static_cast<unsigned int>(bytes[index]);
    }
    return output.str();
}

[[nodiscard]] bool path_exists(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) {
        return false;
    }
    if (error) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to inspect repository path: " + error.message(), path);
    }
    return status.type() != std::filesystem::file_type::not_found;
}

void require_directory(const std::filesystem::path& path, std::string_view description) {
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status)) {
        const std::string detail = error ? ": " + error.message() : std::string{};
        throw LocalVaultError(ErrorCode::invalid_repository,
                              "repository " + std::string(description) +
                                  " is missing or is not a directory" + detail,
                              path);
    }
}

void require_regular_file(const std::filesystem::path& path, std::string_view description) {
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)) {
        const std::string detail = error ? ": " + error.message() : std::string{};
        throw LocalVaultError(ErrorCode::invalid_repository,
                              "repository " + std::string(description) +
                                  " is missing or is not a regular file" + detail,
                              path);
    }
}

[[nodiscard]] bool regular_file_exists_no_follow(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory ||
        status.type() == std::filesystem::file_type::not_found) {
        return false;
    }
    if (error) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to inspect SQLite sidecar: " + error.message(), path);
    }
    if (!std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)) {
        throw LocalVaultError(ErrorCode::invalid_repository,
                              "SQLite sidecar is not a regular non-symlink file", path);
    }
    return true;
}

void validate_layout(const std::filesystem::path& root) {
    require_directory(root / "objects", "objects directory");
    require_directory(root / "temporary", "temporary directory");
    require_directory(root / "temporary" / "objects", "temporary objects directory");
    require_directory(root / "temporary" / "restores", "temporary restores directory");
    require_directory(root / "logs", "logs directory");
    require_regular_file(root / "repository.lock", "lock file");
    require_regular_file(root / "repository.db", "database file");
}

void validate_creation_root(const std::filesystem::path& root) {
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::symlink_status(root, error);
    if (error || std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)) {
        const std::string detail = error ? ": " + error.message() : std::string{};
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "repository root changed during creation" + detail, root);
    }
}

[[nodiscard]] DatabaseAccess database_access_for_open(const std::filesystem::path& root,
                                                      OpenMode mode) {
    const bool wal_exists = regular_file_exists_no_follow(root / "repository.db-wal");
    const bool shm_exists = regular_file_exists_no_follow(root / "repository.db-shm");
    if (mode == OpenMode::read_write) {
        return DatabaseAccess::read_write;
    }
    if (wal_exists && shm_exists) {
        return DatabaseAccess::read_only;
    }
    if (wal_exists != shm_exists) {
        throw LocalVaultError(ErrorCode::invalid_repository,
                              "read-only open requires both SQLite WAL sidecars or neither",
                              root / "repository.db");
    }
    if (!repository_storage_is_proven_read_only(root)) {
        throw LocalVaultError(ErrorCode::invalid_repository,
                              "read-only open without SQLite WAL sidecars is allowed only on "
                              "proven read-only storage",
                              root / "repository.db");
    }
    return DatabaseAccess::immutable_read_only;
}

[[nodiscard]] std::uint32_t checked_uint32(std::int64_t value, std::string_view field,
                                           const std::filesystem::path& database_path) {
    if (value < 0 || static_cast<std::uint64_t>(value) >
                         static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())) {
        throw LocalVaultError(ErrorCode::invalid_repository,
                              "repository " + std::string(field) + " is out of range",
                              database_path);
    }
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] std::uint64_t checked_uint64(std::int64_t value, std::string_view field,
                                           const std::filesystem::path& database_path) {
    if (value < 0) {
        throw LocalVaultError(ErrorCode::invalid_repository,
                              "repository " + std::string(field) + " is out of range",
                              database_path);
    }
    return static_cast<std::uint64_t>(value);
}

[[nodiscard]] int checked_int(std::int64_t value, std::string_view field,
                              const std::filesystem::path& database_path) {
    if (value < static_cast<std::int64_t>((std::numeric_limits<int>::min)()) ||
        value > static_cast<std::int64_t>((std::numeric_limits<int>::max)())) {
        throw LocalVaultError(ErrorCode::invalid_repository,
                              "repository " + std::string(field) + " is out of range",
                              database_path);
    }
    return static_cast<int>(value);
}

void validate_schema(Database& database, const std::filesystem::path& database_path) {
    auto migration = database.statement("SELECT version, typeof(version) FROM schema_migrations");
    if (!migration.step() || migration.column_text(1) != "integer" ||
        migration.column_int64(0) != 1 || migration.step()) {
        throw LocalVaultError(ErrorCode::invalid_repository,
                              "repository must contain exactly migration version 1", database_path);
    }

    auto tables = database.statement(
        "SELECT COUNT(*) FROM sqlite_schema WHERE type = 'table' AND name IN "
        "('schema_migrations', 'repository_info', 'snapshots', 'entries', 'chunks', "
        "'entry_chunks', 'snapshot_warnings', 'repository_settings')");
    if (!tables.step() || tables.column_int64(0) != 8 || tables.step()) {
        throw LocalVaultError(ErrorCode::invalid_repository,
                              "repository is missing a required table", database_path);
    }

    auto indexes =
        database.statement("SELECT COUNT(*) FROM sqlite_schema WHERE type = 'index' AND name IN "
                           "('idx_snapshots_status_created', 'idx_entries_snapshot_parent_name', "
                           "'idx_entries_snapshot_path', 'idx_entry_chunks_hash', "
                           "'idx_snapshot_warnings_snapshot')");
    if (!indexes.step() || indexes.column_int64(0) != 5 || indexes.step()) {
        throw LocalVaultError(ErrorCode::invalid_repository,
                              "repository is missing a required index", database_path);
    }
}

[[nodiscard]] RepositoryInfo load_repository_info(Database& database,
                                                  const std::filesystem::path& database_path) {
    try {
        validate_schema(database, database_path);
        auto query = database.statement(
            "SELECT repository_uuid, format_version, chunk_size_bytes, zstd_level, "
            "hash_algorithm, path_encoding, singleton_id, created_at_ns, application_version, "
            "typeof(repository_uuid), typeof(format_version), typeof(chunk_size_bytes), "
            "typeof(zstd_level), typeof(hash_algorithm), typeof(path_encoding), "
            "typeof(singleton_id), typeof(created_at_ns), typeof(application_version) "
            "FROM repository_info");
        if (!query.step()) {
            throw LocalVaultError(ErrorCode::invalid_repository,
                                  "repository_info must contain exactly one row", database_path);
        }

        const std::string uuid = query.column_text(0);
        const std::int64_t format_version = query.column_int64(1);
        const std::int64_t chunk_size = query.column_int64(2);
        const std::int64_t zstd_level = query.column_int64(3);
        const std::string hash_algorithm = query.column_text(4);
        const std::string path_encoding = query.column_text(5);
        const std::int64_t singleton_id = query.column_int64(6);
        (void)query.column_int64(7);
        (void)query.column_text(8);
        const std::array storage_types{
            query.column_text(9),  query.column_text(10), query.column_text(11),
            query.column_text(12), query.column_text(13), query.column_text(14),
            query.column_text(15), query.column_text(16), query.column_text(17)};
        if (query.step()) {
            throw LocalVaultError(ErrorCode::invalid_repository,
                                  "repository_info must contain exactly one row", database_path);
        }

        constexpr std::array<std::string_view, 9> expected_storage_types{
            "text", "integer", "integer", "integer", "text", "text", "integer", "integer", "text"};
        for (std::size_t index = 0; index < storage_types.size(); ++index) {
            if (storage_types[index] != expected_storage_types[index]) {
                throw LocalVaultError(ErrorCode::invalid_repository,
                                      "repository_info contains an invalid SQLite storage type",
                                      database_path);
            }
        }

        if (singleton_id != 1) {
            throw LocalVaultError(ErrorCode::invalid_repository,
                                  "repository_info singleton identifier is invalid", database_path);
        }

        if (format_version != static_cast<std::int64_t>(current_format_version)) {
            throw LocalVaultError(ErrorCode::unsupported_repository_version,
                                  "unsupported repository format version " +
                                      std::to_string(format_version) +
                                      "; this application supports format version " +
                                      std::to_string(current_format_version),
                                  database_path);
        }
        if (uuid.empty()) {
            throw LocalVaultError(ErrorCode::invalid_repository,
                                  "repository UUID must not be empty", database_path);
        }
        if (chunk_size <= 0) {
            throw LocalVaultError(ErrorCode::invalid_repository,
                                  "repository chunk size must be positive", database_path);
        }
        if (hash_algorithm != "blake3") {
            throw LocalVaultError(ErrorCode::invalid_repository,
                                  "repository hash algorithm is not supported: " + hash_algorithm,
                                  database_path);
        }
        if (path_encoding != "utf-8") {
            throw LocalVaultError(ErrorCode::invalid_repository,
                                  "repository path encoding is not supported: " + path_encoding,
                                  database_path);
        }

        return RepositoryInfo{
            .repository_uuid = uuid,
            .format_version = checked_uint32(format_version, "format version", database_path),
            .chunk_size_bytes = checked_uint64(chunk_size, "chunk size", database_path),
            .zstd_level = checked_int(zstd_level, "zstd level", database_path),
            .hash_algorithm = hash_algorithm,
        };
    } catch (const LocalVaultError& error) {
        if (error.code() != ErrorCode::database_error) {
            throw;
        }
        throw LocalVaultError(ErrorCode::invalid_repository,
                              "repository database has an invalid format: " +
                                  std::string(error.what()),
                              database_path);
    }
}

void create_directory(CreationLedger& ledger, std::size_t index) {
    const std::filesystem::path& path = ledger[index].path;
    std::error_code error;
    if (!std::filesystem::create_directory(path, error)) {
        const std::string detail = error ? error.message() : "path already exists";
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to create repository directory: " + detail, path);
    }
    ledger.mark_created(index);
}

[[nodiscard]] std::vector<std::filesystem::path>
missing_root_paths(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> missing;
    std::filesystem::path candidate = root;
    while (!path_exists(candidate)) {
        missing.push_back(candidate);
        const std::filesystem::path parent = candidate.parent_path();
        if (parent == candidate || parent.empty()) {
            break;
        }
        candidate = parent;
    }
    std::ranges::reverse(missing);
    return missing;
}

void create_empty_file(CreationLedger& ledger, std::size_t index) {
    create_exclusive_file(ledger[index].path);
    ledger.mark_created(index);
    apply_restrictive_file_permissions(ledger[index].path);
}

} // namespace

class Repository::Impl final {
  public:
    Impl(std::filesystem::path root, RepositoryInfo info, std::unique_ptr<Database> database,
         OpenMode mode)
        : root_(std::move(root)), info_(std::move(info)), database_(std::move(database)),
          mode_(mode) {}

    std::filesystem::path root_;
    RepositoryInfo info_;
    std::unique_ptr<Database> database_;
    OpenMode mode_;
    std::shared_ptr<FailureInjector> failure_injector_;
};

void Repository::create(const std::filesystem::path& requested_root,
                        const RepositoryCreateOptions& options) {
    const std::filesystem::path root = normalized_root(requested_root);
    if (options.chunk_size_bytes == 0 ||
        options.chunk_size_bytes >
            static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        throw LocalVaultError(ErrorCode::invalid_argument,
                              "repository chunk size is outside SQLite's supported range", root);
    }

    const bool root_existed = path_exists(root);
    std::filesystem::perms original_permissions = std::filesystem::perms::unknown;
    if (root_existed) {
        std::error_code error;
        const std::filesystem::file_status status = std::filesystem::symlink_status(root, error);
        if (error) {
            throw LocalVaultError(ErrorCode::filesystem_error,
                                  "failed to inspect repository root: " + error.message(), root);
        }
        if (std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)) {
            throw LocalVaultError(
                ErrorCode::invalid_argument,
                "repository root already exists and is not a non-symlink directory", root);
        }
        original_permissions = status.permissions();

        const bool empty = std::filesystem::is_empty(root, error);
        if (error) {
            throw LocalVaultError(ErrorCode::filesystem_error,
                                  "failed to inspect repository directory: " + error.message(),
                                  root);
        }
        if (!empty && !options.allow_existing_non_empty) {
            throw LocalVaultError(
                ErrorCode::invalid_argument,
                "repository root is an existing non-empty directory; explicit approval is required",
                root);
        }
    }

    const FilesystemClass classification = classify_destination_filesystem(root);
    const FilesystemPolicy policy =
        filesystem_policy(classification, options.allow_risky_filesystem);
    if (!policy.allowed) {
        throw LocalVaultError(
            ErrorCode::filesystem_error,
            "repository destination is on a network filesystem; set allow_risky_filesystem to "
            "accept reduced safety",
            root);
    }
    if (policy.warn) {
        std::fprintf(stderr,
                     "WARNING: LocalVault repository destination uses FAT/exFAT; filesystem "
                     "durability and permission guarantees are reduced: %s\n",
                     root.string().c_str());
    }

    const std::array reserved_paths{root / "objects",
                                    root / "temporary",
                                    root / "logs",
                                    root / "repository.lock",
                                    root / "repository.db",
                                    root / "repository.db-wal",
                                    root / "repository.db-shm",
                                    root / "repository.db-journal"};
    for (const std::filesystem::path& path : reserved_paths) {
        if (path_exists(path)) {
            throw LocalVaultError(ErrorCode::invalid_argument,
                                  "repository path collides with existing content", path);
        }
    }

    const std::filesystem::path database_path = root / "repository.db";
    const std::filesystem::path wal_path = root / "repository.db-wal";
    const std::filesystem::path shm_path = root / "repository.db-shm";
    const std::filesystem::path journal_path = root / "repository.db-journal";
    const std::vector<std::filesystem::path> missing_roots =
        root_existed ? std::vector<std::filesystem::path>{} : missing_root_paths(root);

    std::vector<CreationRecord> records;
    records.reserve(missing_roots.size() + 10U);
    for (const std::filesystem::path& path : missing_roots) {
        records.push_back({.path = path, .protected_while_locked = true});
    }
    const auto add_record = [&records](std::filesystem::path path,
                                       bool protected_while_locked = false) {
        const std::size_t index = records.size();
        records.push_back(
            {.path = std::move(path), .protected_while_locked = protected_while_locked});
        return index;
    };
    const std::size_t objects_index = add_record(root / "objects");
    const std::size_t temporary_index = add_record(root / "temporary");
    const std::size_t temporary_objects_index = add_record(root / "temporary" / "objects");
    const std::size_t temporary_restores_index = add_record(root / "temporary" / "restores");
    const std::size_t logs_index = add_record(root / "logs");
    const std::size_t lock_index = add_record(root / "repository.lock", true);
    const std::size_t database_index = add_record(database_path);
    const std::size_t wal_index = add_record(wal_path);
    const std::size_t shm_index = add_record(shm_path);
    const std::size_t journal_index = add_record(journal_path);
    CreationLedger ledger(std::move(records));

    std::optional<RepositoryLock> writer_lock;
    bool permissions_changed = false;
    bool published = false;
    const auto cleanup = [&] {
        ledger.cleanup(writer_lock.has_value());
        writer_lock.reset();
        ledger.cleanup(false);
        if (permissions_changed && original_permissions != std::filesystem::perms::unknown) {
            std::error_code ignored;
            std::filesystem::permissions(root, original_permissions,
                                         std::filesystem::perm_options::replace, ignored);
        }
    };
    try {
        for (std::size_t index = 0; index < missing_roots.size(); ++index) {
            create_directory(ledger, index);
        }
        validate_creation_root(root);
        apply_restrictive_repository_permissions(root);
        permissions_changed = root_existed;

        create_directory(ledger, objects_index);
        create_directory(ledger, temporary_index);
        create_directory(ledger, temporary_objects_index);
        create_directory(ledger, temporary_restores_index);
        create_directory(ledger, logs_index);
        create_empty_file(ledger, lock_index);

        {
            writer_lock.emplace(RepositoryLock::acquire_exclusive(root / "repository.lock"));
            create_empty_file(ledger, database_index);
            create_empty_file(ledger, wal_index);
            create_empty_file(ledger, shm_index);
            create_empty_file(ledger, journal_index);
            Database database(database_path);
            run_migrations(database);

            Transaction transaction(database, TransactionMode::exclusive);
            auto insert = database.statement(
                "INSERT INTO repository_info "
                "(singleton_id, repository_uuid, format_version, created_at_ns, "
                "application_version, chunk_size_bytes, zstd_level, hash_algorithm) "
                "VALUES (1, :uuid, :format_version, :created_at_ns, :application_version, "
                ":chunk_size_bytes, :zstd_level, 'blake3')");
            insert.bind(":uuid", make_uuid_v4());
            insert.bind(":format_version", static_cast<std::int64_t>(current_format_version));
            insert.bind(":created_at_ns", utc_now_ns());
            insert.bind(":application_version", kVersion);
            insert.bind(":chunk_size_bytes", static_cast<std::int64_t>(options.chunk_size_bytes));
            insert.bind(":zstd_level", static_cast<std::int64_t>(options.zstd_level));
            insert.execute();
            transaction.commit();

            auto checkpoint = database.statement("PRAGMA wal_checkpoint(TRUNCATE)");
            if (!checkpoint.step() || checkpoint.column_int64(0) != 0 || checkpoint.step()) {
                throw LocalVaultError(ErrorCode::database_error,
                                      "checkpointing the repository database failed",
                                      database_path);
            }
            const int flush_result = sqlite3_db_cacheflush(database.handle());
            if (flush_result != SQLITE_OK) {
                throw LocalVaultError(ErrorCode::database_error,
                                      "flushing repository database failed (SQLite result " +
                                          std::to_string(flush_result) + ")",
                                      database_path);
            }
            flush_containing_directory(database_path);
        }

        {
            Repository locked_validation = Repository::open(root, OpenMode::read_only);
            (void)locked_validation;
        }
        published = true;
        writer_lock.reset();

        Repository final_self_check = Repository::open(root, OpenMode::read_only);
        (void)final_self_check;
    } catch (const LocalVaultError& error) {
        if (!published) {
            cleanup();
        }
        if (!error.path().empty()) {
            throw;
        }
        throw LocalVaultError(error.code(), error.what(), database_path);
    } catch (const std::filesystem::filesystem_error& error) {
        if (!published) {
            cleanup();
        }
        const std::filesystem::path failing_path = error.path1().empty() ? root : error.path1();
        throw LocalVaultError(ErrorCode::filesystem_error, error.what(), failing_path);
    } catch (...) {
        if (!published) {
            cleanup();
        }
        throw;
    }
}

Repository Repository::open(const std::filesystem::path& requested_root, OpenMode mode) {
    const std::filesystem::path root = normalized_root(requested_root);
    std::error_code error;
    const std::filesystem::file_status root_status = std::filesystem::symlink_status(root, error);
    if (error == std::errc::no_such_file_or_directory ||
        root_status.type() == std::filesystem::file_type::not_found) {
        throw LocalVaultError(ErrorCode::repository_not_found, "repository root does not exist",
                              root);
    }
    if (error) {
        throw LocalVaultError(ErrorCode::filesystem_error,
                              "failed to inspect repository root: " + error.message(), root);
    }
    if (std::filesystem::is_symlink(root_status) || !std::filesystem::is_directory(root_status)) {
        throw LocalVaultError(ErrorCode::invalid_repository,
                              "repository root is not a non-symlink directory", root);
    }

    validate_layout(root);
    const std::filesystem::path database_path = root / "repository.db";
    const DatabaseAccess access = database_access_for_open(root, mode);
    auto database = std::make_unique<Database>(database_path, access);
    RepositoryInfo info = load_repository_info(*database, database_path);
    return Repository(std::make_unique<Impl>(root, std::move(info), std::move(database), mode));
}

Repository::Repository(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Repository::Repository(Repository&&) noexcept = default;
Repository& Repository::operator=(Repository&&) noexcept = default;
Repository::~Repository() = default;

const std::filesystem::path& Repository::root() const noexcept {
    return impl_->root_;
}

std::uint32_t Repository::format_version() const noexcept {
    return impl_->info_.format_version;
}

const RepositoryInfo& Repository::info() const noexcept {
    return impl_->info_;
}

Database& Repository::database() noexcept {
    return *impl_->database_;
}

OpenMode Repository::open_mode() const noexcept {
    return impl_->mode_;
}

void Repository::set_failure_injector(std::shared_ptr<FailureInjector> injector) {
    impl_->failure_injector_ = std::move(injector);
}

} // namespace localvault
