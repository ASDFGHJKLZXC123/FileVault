# M7 basic commands

Own only new `src/cli/commands_basic.cpp`. Root's shared foundation is in commands.hpp,
output.cpp/main.cpp. Implement `register_basic_commands(CLI::App&, Context&, Dispatcher&)`.
One options struct and run function per command; shared_ptr option lifetimes captured by
CLI callbacks. Each subcommand uses fallthrough() for globals; callback only assigns the
dispatch closure and context.command, never runs storage while parsing. Command run uses
Context::repository_path(), utf8_path(), Context::progress()/stop and finish(result, code).
JSON envelope is {schema_version:1,command,result}; errors are emitted centrally.
Human results are formatted JSON data; no diagnostics on stdout. No direct DB access.

Part 08 §28 commands/flags:
- init [repository-path] [--chunk-size 4MiB] [--compression-level 3] [--allow-risky-filesystem].
  Positional repo or global --repo, reject contradictory duplicate paths. First release only
  supports 4MiB chunks as allowed by §28.2; accept 4MiB and numeric 4194304.
- snapshot source-path [--message] [--workers N] [--force-rehash] [--ignore-file]
  [--skip-hidden] [--one-file-system]. All map to existing SnapshotOptions.
- list [--limit N] [--offset N]; defaults follow core PageRequest 200/0.
- show id [--warnings]: QueryService get_snapshot + list_warnings; page through warnings
  internally so all requested warnings appear. New SnapshotWarning fields path/code/message.
- files id [--path directory] [--search text] [--limit] [--offset]. Reject simultaneous
  explicit --path and --search; use existing direct-child or literal-search APIs.

Result contracts: init {repository}; snapshot {snapshot_id, file_count, directory_count,
logical_bytes,new_stored_bytes,new_chunks,reused_chunks,warnings}; list {snapshots,total_count};
show {snapshot,warnings?}; files {entries,total_count}. Use shared serializers.
Per-file warnings -> exit6; normal operations exit0. Read-only commands open read_only.
Missing repo/usage/error mapping delegated to Context/main. UTF-8 paths end-to-end.
Root owns CMake, builds and end-to-end tests. No other file edits. Smallest complete code.
