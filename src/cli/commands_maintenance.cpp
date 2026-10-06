#include "commands.hpp"
#include "console.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "localvault/diff_engine.hpp"
#include "localvault/garbage_collector.hpp"
#include "localvault/integrity_verifier.hpp"
#include "localvault/query_service.hpp"
#include "localvault/repository.hpp"
#include "localvault/restore_engine.hpp"

namespace localvault::cli {
namespace {

struct DiffCommandOptions {
    SnapshotId older{};
    SnapshotId newer{};
    bool include_unchanged{};
    bool content_only{};
};

struct RestoreCommandOptions {
    SnapshotId snapshot{};
    std::vector<std::string> paths;
    std::string output;
    std::string overwrite{"never"};
    bool no_final_hash{};
    bool all{};
};

struct VerifyCommandOptions {
    bool quick{};
    bool full{};
    bool files{};
};

struct StatsCommandOptions {
    SnapshotId snapshot{};
};

struct DeleteCommandOptions {
    SnapshotId snapshot{};
    bool yes{};
    bool gc{};
};

struct GcCommandOptions {
    bool dry_run{};
};

std::string answer(const Context& context) {
    auto text = read_answer(context.stop);
    const auto first = text.find_first_not_of(" \t\r\n");
    text = first == std::string::npos
               ? std::string{}
               : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return text;
}

ConflictResolver restore_resolver(const Context& context) {
    return [&context, all = std::optional<ConflictDecision>{}](const std::filesystem::path& path,
                                                               EntryType) mutable {
        if (all) {
            return *all;
        }
        while (true) {
            std::cerr << "Destination exists: " << path_text(path)
                      << " [s=skip, r=replace, sa=skip all, ra=replace all, c=cancel]: "
                      << std::flush;
            const auto choice = answer(context);
            if (choice == "s" || choice == "skip") {
                return ConflictDecision::skip;
            }
            if (choice == "r" || choice == "replace") {
                return ConflictDecision::replace;
            }
            if (choice == "sa" || choice == "skip all") {
                all = ConflictDecision::skip;
                return *all;
            }
            if (choice == "ra" || choice == "replace all") {
                all = ConflictDecision::replace;
                return *all;
            }
            if (choice == "c" || choice == "cancel") {
                return ConflictDecision::cancel;
            }
            std::cerr << "Enter s, r, sa, ra, or c.\n";
        }
    };
}

Json collection_json(const GarbageCollectionResult& result, bool dry_run) {
    return {{"dry_run", dry_run},
            {"unreferenced_chunks", result.unreferenced_chunks},
            {"orphan_objects", result.orphan_objects},
            {"stale_temporary_files", result.stale_temporary_files},
            {"stale_snapshots", result.stale_snapshots},
            {"ignored_paths", result.ignored_paths},
            {"reclaimable_bytes", result.reclaimable_bytes},
            {"removed_objects", result.removed_objects},
            {"reclaimed_bytes", result.reclaimed_bytes}};
}

int run_diff_command(const DiffCommandOptions& options, const Context& context) {
    auto repository = Repository::open_for_query(context.repository_path());
    DiffOptions request;
    request.include_unchanged = options.include_unchanged;
    request.compare_metadata = !options.content_only;
    constexpr std::array kinds{
        "added", "removed", "type_changed", "content_modified", "metadata_modified", "unchanged"};
    return context.finish_array("changes", [&](const JsonConsumer& emit) {
        DiffEngine(repository)
            .diff(
                options.older, options.newer, request,
                [&](const DiffEntry& entry) {
                    emit({{"path", path_text(entry.relative_path)},
                          {"kind", kinds.at(static_cast<std::size_t>(entry.kind))},
                          {"before", entry.before ? entry_json(*entry.before) : Json()},
                          {"after", entry.after ? entry_json(*entry.after) : Json()}});
                },
                context.stop);
    });
}

int run_restore_command(const RestoreCommandOptions& options, const Context& context) {
    if ((options.paths.empty() && !options.all) || (!options.paths.empty() && options.all)) {
        throw LocalVaultError(ErrorCode::invalid_argument,
                              "restore requires relative paths or --all, supplied separately");
    }
    RestoreRequest request;
    request.snapshot_id = options.snapshot;
    request.destination_root = utf8_path(options.output);
    for (const auto& path : options.paths) {
        request.relative_paths.push_back(utf8_path(path));
    }
    request.overwrite_policy = options.overwrite == "always"   ? OverwritePolicy::always
                               : options.overwrite == "prompt" ? OverwritePolicy::prompt
                                                               : OverwritePolicy::never;
    if (request.overwrite_policy == OverwritePolicy::prompt) {
        request.conflict_resolver = restore_resolver(context);
    }
    request.verify_final_file_hash = !options.no_final_hash;
    auto repository = Repository::open(context.repository_path());
    const auto result =
        RestoreEngine(repository).restore(request, context.stop, context.progress());
    return context.finish({{"restored_files", result.restored_files},
                           {"restored_directories", result.restored_directories},
                           {"restored_symlinks", result.restored_symlinks},
                           {"restored_bytes", result.restored_bytes},
                           {"warnings", warnings_json(result.skipped_entries)}},
                          result.skipped_entries.empty() ? 0 : 6);
}

int run_verify_command(const VerifyCommandOptions& options, const Context& context) {
    if (options.quick && options.files) {
        throw LocalVaultError(ErrorCode::invalid_argument,
                              "--files cannot be combined with --quick");
    }
    auto repository = Repository::open(context.repository_path(), OpenMode::maintenance_read_only);
    const auto mode = options.full || options.files ? VerifyMode::full : VerifyMode::quick;
    const auto result =
        IntegrityVerifier(repository).verify(mode, context.stop, context.progress(), options.files);
    constexpr std::array kinds{"missing_object",         "corrupt_object",
                               "invalid_chunk_size",     "invalid_entry_relationship",
                               "invalid_snapshot_state", "stale_temporary_file",
                               "invalid_database",       "invalid_repository_info",
                               "stale_chunk_metadata",   "orphan_object",
                               "file_hash_mismatch"};
    Json issues = Json::array();
    for (const auto& issue : result.issues) {
        const auto severity =
            issue.severity == VerificationIssue::Severity::error ? "error" : "note";
        issues.push_back({{"path", path_text(issue.path)},
                          {"kind", kinds.at(static_cast<std::size_t>(issue.kind))},
                          {"severity", severity},
                          {"detail", issue.detail}});
        std::cerr << severity << ": " << issue.detail;
        if (!issue.path.empty()) {
            std::cerr << ": " << path_text(issue.path);
        }
        std::cerr << '\n';
    }
    return context.finish({{"ok", result.ok()},
                           {"checked_snapshots", result.checked_snapshots},
                           {"checked_entries", result.checked_entries},
                           {"checked_objects", result.checked_objects},
                           {"checked_stored_bytes", result.checked_stored_bytes},
                           {"checked_files", result.checked_files},
                           {"checked_file_bytes", result.checked_file_bytes},
                           {"issues", std::move(issues)}},
                          result.ok() ? 0 : 5);
}

int run_stats_command(const StatsCommandOptions& options, const Context& context) {
    auto repository = Repository::open_for_query(context.repository_path());
    QueryService query(repository);
    return context.finish(stats_json(options.snapshot != 0 ? query.snapshot_stats(options.snapshot)
                                                           : query.repository_stats()));
}

int run_delete_command(const DeleteCommandOptions& options, const Context& context) {
    auto repository = Repository::open(context.repository_path());
    if (!options.yes) {
        (void)QueryService(repository).get_snapshot(options.snapshot);
        while (true) {
            std::cerr << "Delete snapshot " << options.snapshot
                      << "? [y=yes, n=no]: " << std::flush;
            const auto choice = answer(context);
            if (choice == "y" || choice == "yes") {
                break;
            }
            if (choice == "n" || choice == "no" || choice == "c" || choice == "cancel") {
                throw LocalVaultError(ErrorCode::cancelled, "snapshot deletion cancelled");
            }
            std::cerr << "Enter y or n.\n";
        }
    }
    GarbageCollector collector(repository);
    collector.delete_snapshot(options.snapshot, context.stop);
    Json result{{"deleted_snapshot_id", options.snapshot}};
    if (options.gc) {
        result["collection"] = collection_json(
            collector.collect({.dry_run = false}, context.stop, context.progress()), false);
    }
    return context.finish(std::move(result));
}

int run_gc_command(const GcCommandOptions& options, const Context& context) {
    auto repository =
        Repository::open(context.repository_path(),
                         options.dry_run ? OpenMode::maintenance_read_only : OpenMode::read_write);
    const auto result =
        GarbageCollector(repository)
            .collect({.dry_run = options.dry_run}, context.stop, context.progress());
    return context.finish(collection_json(result, options.dry_run));
}

} // namespace

void register_maintenance_commands(CLI::App& app, Context& context, Dispatcher& dispatch) {
    {
        auto options = std::make_shared<DiffCommandOptions>();
        auto command = app.add_subcommand("diff", "Compare snapshots");
        command->fallthrough();
        command->add_option("older-id", options->older, "Older snapshot ID")
            ->required()
            ->check(CLI::PositiveNumber);
        command->add_option("newer-id", options->newer, "Newer snapshot ID")
            ->required()
            ->check(CLI::PositiveNumber);
        command->add_flag("--include-unchanged", options->include_unchanged,
                          "Include unchanged entries");
        command->add_flag("--content-only", options->content_only, "Ignore metadata differences");
        command->callback([options, &context, &dispatch] {
            context.command = "diff";
            dispatch = [options, &context] { return run_diff_command(*options, context); };
        });
    }
    {
        auto options = std::make_shared<RestoreCommandOptions>();
        auto command = app.add_subcommand("restore", "Restore snapshot files");
        command->fallthrough();
        command->add_option("snapshot-id", options->snapshot, "Snapshot ID")
            ->required()
            ->check(CLI::PositiveNumber);
        command->add_option("relative-path", options->paths, "Paths within the snapshot");
        command->add_option("--output", options->output, "Destination directory")->required();
        command->add_option("--overwrite", options->overwrite, "never, prompt, or always")
            ->check(CLI::IsMember({"never", "prompt", "always"}))
            ->default_val("never");
        command->add_flag("--no-final-hash", options->no_final_hash,
                          "Disable optional final hash check");
        command->add_flag("--all", options->all, "Restore the complete snapshot");
        command->callback([options, &context, &dispatch] {
            context.command = "restore";
            dispatch = [options, &context] { return run_restore_command(*options, context); };
        });
    }
    {
        auto options = std::make_shared<VerifyCommandOptions>();
        auto command = app.add_subcommand("verify", "Check repository integrity");
        command->fallthrough();
        auto quick =
            command->add_flag("--quick", options->quick, "Check metadata and object sizes");
        auto full = command->add_flag("--full", options->full, "Verify chunk content hashes");
        quick->excludes(full);
        command->add_flag("--files", options->files, "Verify whole-file hashes; implies --full");
        command->callback([options, &context, &dispatch] {
            context.command = "verify";
            dispatch = [options, &context] { return run_verify_command(*options, context); };
        });
    }
    {
        auto options = std::make_shared<StatsCommandOptions>();
        auto command = app.add_subcommand("stats", "Show repository or snapshot statistics");
        command->fallthrough();
        command->add_option("--snapshot", options->snapshot, "Scope to one snapshot")
            ->check(CLI::PositiveNumber);
        command->callback([options, &context, &dispatch] {
            context.command = "stats";
            dispatch = [options, &context] { return run_stats_command(*options, context); };
        });
    }
    {
        auto options = std::make_shared<DeleteCommandOptions>();
        auto command = app.add_subcommand("delete", "Delete snapshot metadata");
        command->fallthrough();
        command->add_option("snapshot-id", options->snapshot, "Snapshot ID")
            ->required()
            ->check(CLI::PositiveNumber);
        command->add_flag("--yes", options->yes, "Confirm deletion without prompting");
        command->add_flag("--gc", options->gc, "Reclaim unreferenced objects after deletion");
        command->callback([options, &context, &dispatch] {
            context.command = "delete";
            dispatch = [options, &context] { return run_delete_command(*options, context); };
        });
    }
    {
        auto options = std::make_shared<GcCommandOptions>();
        auto command = app.add_subcommand("gc", "Reclaim unreferenced repository data");
        command->fallthrough();
        command->add_flag("--dry-run", options->dry_run, "Preview cleanup without deleting data");
        command->callback([options, &context, &dispatch] {
            context.command = "gc";
            dispatch = [options, &context] { return run_gc_command(*options, context); };
        });
    }
}

} // namespace localvault::cli
