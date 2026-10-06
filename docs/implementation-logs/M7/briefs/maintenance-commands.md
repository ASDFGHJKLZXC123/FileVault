# M7 maintenance commands

Own only new `src/cli/commands_maintenance.cpp`. Implement
register_maintenance_commands(CLI::App&, Context&, Dispatcher&) with commands.hpp helpers.
Each subcommand fallthrough() for globals; callbacks only assign dispatch + context.command.
Options live in shared_ptr structs, one run function per command, no direct DB/storage logic.
Root owns CMake/main/builds/tests. Console API: terminal(FILE*), read_answer(stop), InterruptHandler.

Part08 §28 flags:
- diff older-id newer-id [--include-unchanged] [--content-only]. compare_metadata=false
  for content-only; unchanged excluded by default. Use streaming core callback; result
  {changes:[{path,kind,before,after}]}. Required classifications from DiffKind.
- restore id [relative-path ...] --output destination [--overwrite never|prompt|always]
  [--no-final-hash] [--all]. Require explicit --all for no paths, reject --all with paths.
  ConflictResolver supports skip/replace/cancel plus apply-to-all skip/replace. User decision:
  accept terminal and scripted input; print prompts to stderr, read_answer(stop), clear EOF
  error. Document answers s/r/sa/ra/c (accept long forms if cheap). Never stdout prompts.
  Result {restored_files,restored_directories,restored_symlinks,restored_bytes,warnings}, warning6.
- verify [--quick|--full] [--files]. Default quick; --files implies full unless explicit quick
  (usage2). New core signature verify(mode,token,progress,boolfiles). Result {ok,checked_snapshots,
  checked_entries,checked_objects,checked_stored_bytes,checked_files,checked_file_bytes,issues};
  issues path/kind/severity/detail. Core okfalse ->exit5, maintenance notes ->exit0.
- stats [--snapshot id]. Use repository_stats or new snapshot_stats. Result shared stats JSON.
- delete id [--yes] [--gc]. Ask confirmation otherwise (terminal/script input accepted),
  non-TTY EOF invalidargument. Declining retains snapshot and returns cancelled130; --gc
  explicitly executes GC after deletion. Result {deleted_snapshot_id,collection?}.
- gc [--dry-run]. CLI default executes cleanup, --dry-run previews. Explicitly set core's
  dry_run option; core default preview stays unchanged. Result includes all GC result counters.

Long operations receive context.stop/context.progress. Diff opens read_only, restore/
verification/delete/gc use read_write. Verification must not trigger recovery before reporting
stale state; existing core open behavior handles this. Root resolves integration seams.
Use UTF-8 path helpers, bounded core loops, clear errors, minimal complete implementation.
