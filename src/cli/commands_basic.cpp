#include "commands.hpp"

#include <memory>
#include <optional>
#include <utility>

#include "localvault/query_service.hpp"
#include "localvault/repository.hpp"
#include "localvault/snapshot_engine.hpp"

namespace localvault::cli {

namespace {

struct InitCommandOptions {
    std::string path;
    std::string chunk_size{"4MiB"};
    RepositoryCreateOptions repository;
};

struct SnapshotCommandOptions {
    std::string source;
    std::optional<std::string> ignore_file;
    bool skip_hidden{};
    SnapshotOptions snapshot;
};

struct ListCommandOptions {
    PageRequest page;
};

struct ShowCommandOptions {
    SnapshotId id{};
    bool warnings{};
};

struct FilesCommandOptions {
    SnapshotId id{};
    std::string path{"."};
    std::optional<std::string> search;
    PageRequest page;
};

int run_init_command(const InitCommandOptions& options, Context& context) {
    if (options.chunk_size != "4MiB" && options.chunk_size != "4194304") {
        throw LocalVaultError(ErrorCode::invalid_argument,
                              "only the 4MiB (4194304-byte) chunk size is supported");
    }
    const auto root = std::filesystem::absolute(options.path.empty() ? context.repository_path()
                                                                     : utf8_path(options.path))
                          .lexically_normal();
    if (!options.path.empty() && !context.repository.empty() &&
        root != std::filesystem::absolute(context.repository_path()).lexically_normal()) {
        throw LocalVaultError(ErrorCode::invalid_argument,
                              "positional repository path conflicts with --repo");
    }
    Repository::create(root, options.repository);
    return context.finish({{"repository", path_text(root)}});
}

int run_snapshot_command(const SnapshotCommandOptions& options, Context& context) {
    auto repository = Repository::open(context.repository_path());
    auto settings = options.snapshot;
    settings.include_hidden = !options.skip_hidden;
    if (options.ignore_file) {
        settings.ignore_file = utf8_path(*options.ignore_file);
    }
    const auto result =
        SnapshotEngine(repository)
            .create_snapshot(utf8_path(options.source), settings, context.stop, context.progress());
    return context.finish({{"snapshot_id", result.snapshot_id},
                           {"file_count", result.file_count},
                           {"directory_count", result.directory_count},
                           {"logical_bytes", result.logical_bytes},
                           {"new_stored_bytes", result.new_stored_bytes},
                           {"new_chunks", result.new_chunks},
                           {"reused_chunks", result.reused_chunks},
                           {"warnings", warnings_json(result.skipped_entries)}},
                          result.skipped_entries.empty() ? 0 : 6);
}

int run_list_command(const ListCommandOptions& options, Context& context) {
    auto repository = Repository::open_for_query(context.repository_path());
    const auto page = QueryService(repository).list_snapshots(options.page);
    Json snapshots = Json::array();
    for (const auto& snapshot : page.items) {
        snapshots.push_back(snapshot_json(snapshot));
    }
    return context.finish({{"snapshots", std::move(snapshots)}, {"total_count", page.total_count}});
}

int run_show_command(const ShowCommandOptions& options, Context& context) {
    auto repository = Repository::open_for_query(context.repository_path());
    const QueryService query(repository);
    Json result{{"snapshot", snapshot_json(query.get_snapshot(options.id))}};
    if (options.warnings) {
        result["warnings"] = Json::array();
        PageRequest request{0, 10'000};
        for (;;) {
            if (context.stop.stop_requested()) {
                throw LocalVaultError(ErrorCode::cancelled, "warning query cancelled");
            }
            const auto page = query.list_warnings(options.id, request);
            for (const auto& warning : page.items) {
                result["warnings"].push_back({{"path", path_text(warning.relative_path)},
                                              {"code", warning.code},
                                              {"message", warning.message}});
            }
            request.offset += page.items.size();
            if (page.items.empty() || request.offset >= page.total_count) {
                break;
            }
        }
    }
    return context.finish(std::move(result));
}

int run_files_command(const FilesCommandOptions& options, Context& context) {
    auto repository = Repository::open_for_query(context.repository_path());
    const QueryService query(repository);
    const auto page = options.search
                          ? query.search_paths(options.id, *options.search, options.page)
                          : query.list_children(options.id, utf8_path(options.path), options.page);
    Json entries = Json::array();
    for (const auto& entry : page.items) {
        entries.push_back(entry_json(entry));
    }
    return context.finish({{"entries", std::move(entries)}, {"total_count", page.total_count}});
}

} // namespace

void register_basic_commands(CLI::App& app, Context& context, Dispatcher& dispatch) {
    auto init_options = std::make_shared<InitCommandOptions>();
    auto* init = app.add_subcommand("init", "Create a repository")->fallthrough();
    init->add_option("repository-path", init_options->path, "Repository path");
    init->add_option("--chunk-size", init_options->chunk_size, "Chunk size (4MiB)")
        ->capture_default_str();
    init->add_option("--compression-level", init_options->repository.zstd_level, "Zstandard level")
        ->capture_default_str();
    init->add_flag("--allow-risky-filesystem", init_options->repository.allow_risky_filesystem,
                   "Allow a filesystem with reduced safety guarantees");
    init->callback([init_options, &context, &dispatch] {
        context.command = "init";
        dispatch = [init_options, &context] { return run_init_command(*init_options, context); };
    });

    auto snapshot_options = std::make_shared<SnapshotCommandOptions>();
    auto* snapshot = app.add_subcommand("snapshot", "Create a snapshot")->fallthrough();
    snapshot->add_option("source-path", snapshot_options->source, "Source directory")->required();
    snapshot->add_option("--message", snapshot_options->snapshot.message, "Snapshot message");
    snapshot->add_option("--workers", snapshot_options->snapshot.worker_count, "Worker count")
        ->check(CLI::NonNegativeNumber);
    snapshot->add_flag("--force-rehash", snapshot_options->snapshot.force_rehash,
                       "Rehash all file contents");
    snapshot->add_option("--ignore-file", snapshot_options->ignore_file, "Ignore rules file");
    snapshot->add_flag("--skip-hidden", snapshot_options->skip_hidden, "Skip hidden entries");
    snapshot->add_flag("--one-file-system", snapshot_options->snapshot.one_file_system,
                       "Stay on the source filesystem");
    snapshot->callback([snapshot_options, &context, &dispatch] {
        context.command = "snapshot";
        dispatch = [snapshot_options, &context] {
            return run_snapshot_command(*snapshot_options, context);
        };
    });

    auto list_options = std::make_shared<ListCommandOptions>();
    auto* list = app.add_subcommand("list", "List complete snapshots")->fallthrough();
    list->add_option("--limit", list_options->page.limit, "Page size")
        ->check(CLI::Range(1, 10'000))
        ->capture_default_str();
    list->add_option("--offset", list_options->page.offset, "Page offset")
        ->check(CLI::NonNegativeNumber)
        ->capture_default_str();
    list->callback([list_options, &context, &dispatch] {
        context.command = "list";
        dispatch = [list_options, &context] { return run_list_command(*list_options, context); };
    });

    auto show_options = std::make_shared<ShowCommandOptions>();
    auto* show = app.add_subcommand("show", "Show snapshot metadata")->fallthrough();
    show->add_option("snapshot-id", show_options->id, "Snapshot identifier")->required();
    show->add_flag("--warnings", show_options->warnings, "Include stored snapshot warnings");
    show->callback([show_options, &context, &dispatch] {
        context.command = "show";
        dispatch = [show_options, &context] { return run_show_command(*show_options, context); };
    });

    auto files_options = std::make_shared<FilesCommandOptions>();
    auto* files = app.add_subcommand("files", "Browse or search snapshot entries")->fallthrough();
    files->add_option("snapshot-id", files_options->id, "Snapshot identifier")->required();
    auto* path = files->add_option("--path", files_options->path, "Relative directory");
    files->add_option("--search", files_options->search, "Literal path substring")->excludes(path);
    files->add_option("--limit", files_options->page.limit, "Page size")
        ->check(CLI::Range(1, 10'000))
        ->capture_default_str();
    files->add_option("--offset", files_options->page.offset, "Page offset")
        ->check(CLI::NonNegativeNumber)
        ->capture_default_str();
    files->callback([files_options, &context, &dispatch] {
        context.command = "files";
        dispatch = [files_options, &context] { return run_files_command(*files_options, context); };
    });
}

} // namespace localvault::cli
