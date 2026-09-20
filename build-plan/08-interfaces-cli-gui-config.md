# LocalVault Build Plan — Part 08: Interfaces: CLI, GUI, Configuration, Logging

> Sections 28–31 of the Technical Implementation Guide (Revision 2), split verbatim.
> Section numbers are unchanged, so cross-references like “Section 25.10”
> resolve via the lookup table in [00-INDEX.md](00-INDEX.md).

---

# 28. Command-line interface

Executable:

```text
localvault
```

## 28.1 Global options

```text
--repo <path>        Repository path
--json               Machine-readable output where supported
--verbose            Detailed diagnostics
--quiet              Suppress non-error progress
--no-color           Disable terminal colors
--help
--version
```

For commands that create the repository, `--repo` may be replaced by a positional path.

## 28.2 Commands

### Initialize

```bash
localvault init <repository-path> \
  [--chunk-size 4MiB] \
  [--compression-level 3] \
  [--allow-risky-filesystem]
```

The first release may accept only the default chunk size to preserve a simple format.

### Create snapshot

```bash
localvault snapshot <source-path> \
  --repo <repository-path> \
  [--message "text"] \
  [--workers N] \
  [--force-rehash] \
  [--ignore-file <path>] \
  [--skip-hidden] \
  [--one-file-system]
```

### List snapshots

```bash
localvault list \
  --repo <repository-path> \
  [--limit N] \
  [--offset N]
```

### Show one snapshot

```bash
localvault show <snapshot-id> \
  --repo <repository-path> \
  [--warnings]
```

### Browse snapshot entries

```bash
localvault files <snapshot-id> \
  --repo <repository-path> \
  [--path <relative-directory>] \
  [--search <text>] \
  [--limit N] \
  [--offset N]
```

### Diff

```bash
localvault diff <older-id> <newer-id> \
  --repo <repository-path> \
  [--include-unchanged] \
  [--content-only]
```

### Restore

```bash
localvault restore <snapshot-id> \
  [<relative-path> ...] \
  --repo <repository-path> \
  --output <destination-root> \
  [--overwrite never|prompt|always] \
  [--no-final-hash]
```

No relative path means restore the complete snapshot only when `--all` is supplied. Require explicit `--all` to avoid accidental large restores:

```bash
localvault restore 12 --all --repo ./vault --output ./restored
```

### Verify

```bash
localvault verify \
  --repo <repository-path> \
  [--quick | --full] \
  [--files]
```

### Statistics

```bash
localvault stats \
  --repo <repository-path> \
  [--snapshot <id>]
```

### Delete snapshot

```bash
localvault delete <snapshot-id> \
  --repo <repository-path> \
  [--yes] \
  [--gc]
```

### Garbage collect

```bash
localvault gc \
  --repo <repository-path> \
  [--dry-run]
```

## 28.3 Exit codes

| Code | Meaning |
|---:|---|
| `0` | Success |
| `2` | CLI usage or invalid argument |
| `3` | Repository missing, invalid, or unsupported |
| `4` | Filesystem or database operation failed |
| `5` | Repository corruption or verification failure |
| `6` | Partial success with skipped entries or metadata warnings |
| `7` | Repository busy |
| `130` | Cancelled/interrupted |

Catch `SIGINT` on POSIX and register `SetConsoleCtrlHandler` on Windows; request cooperative stop, wait for safe cleanup, then return `130` on every platform for consistency. A second interrupt while cleanup is running forces immediate exit with `130` and no further cleanup; recovery handles the leftovers on the next open.

## 28.4 Output rules

Human-readable mode:

- Progress on stderr.
- Result tables/data on stdout.
- Errors on stderr.

JSON mode:

- Emit one valid JSON document to stdout.
- Keep progress disabled or emit structured progress to stderr.
- Do not mix color codes with JSON.
- Include stable field names and a schema version.

## 28.5 CLI implementation

Create one function per command:

```cpp
int run_init_command(const InitCommandOptions&);
int run_snapshot_command(const SnapshotCommandOptions&, std::stop_token);
int run_list_command(const ListCommandOptions&);
```

`main` responsibilities:

1. Configure CLI11.
2. Parse arguments.
3. Install signal handling.
4. Dispatch one command.
5. Map exceptions to exit codes.
6. Never contain storage logic.

---

# 29. Qt desktop application

Use Qt 6 Widgets.

## 29.1 Target structure

```text
MainWindow
├── Navigation
├── DashboardPage
├── SnapshotsPage
├── RestorePage
├── VerifyPage
└── SettingsPage
```

On first launch, or whenever no repository is configured, show an onboarding view with **Create Repository** and **Open Repository** actions before the pages above become active. Repository creation runs `Repository::create` on a worker thread with the same filesystem classification and warnings as the CLI (Section 15).

## 29.2 Pages

### Dashboard

Display:

- Current repository.
- Protected/source folder used by latest snapshot.
- Latest complete snapshot.
- Repository health.
- Complete snapshot count.
- File count.
- Logical bytes.
- Stored bytes.
- Deduplication, compression, and total savings.
- `Create Snapshot` action.
- Maintenance actions: garbage collection with a mandatory dry-run preview shown before the destructive step (Section 29.6), and stale-state cleanup.

### Snapshots

Use `QTableView` with `SnapshotTableModel`.

Columns:

- ID.
- Created time.
- Message.
- Status.
- Files.
- Logical size.
- Newly stored size.
- New/reused chunks.
- Duration.

Actions:

- Browse.
- Compare.
- Restore.
- Delete.
- View warnings.

### Restore

- Snapshot selector.
- Lazy tree model.
- Path search.
- Multi-selection.
- Destination chooser.
- Overwrite policy.
- Restore button.
- Progress and result summary.

### Verify

- Quick/full selection.
- Last operation summary.
- Start verification.
- Streaming issue table.
- Checked objects/bytes and elapsed time.

### Settings

Repository-independent UI settings:

- Last opened repository.
- Window geometry.
- Theme preference if implemented.

Repository settings:

- Default source root.
- zstd level for future new objects.
- Worker count.
- Ignore-file location or editor.
- Overwrite default.
- Log level.

Do not allow changing repository chunk size after creation.

## 29.3 Model/view rules

- Use `QAbstractTableModel` for snapshots.
- Use a lazy `QAbstractItemModel` for snapshot entries.
- Fetch children through `QueryService::list_children`.
- Do not create one widget per file.
- Do not load all entries into memory.
- Keep model data immutable while a fetch is in progress.
- Use explicit refresh after a mutating operation completes.

## 29.4 Background operations

The GUI thread must not call long-running core methods directly.

Recommended pattern:

1. `OperationController` owns a `QThread`.
2. A `CoreOperationWorker` object moves to that thread.
3. The worker calls the core engine.
4. Core progress callbacks emit Qt signals using queued connections.
5. Cancel button requests the worker's `std::stop_source`.
6. Completion and failure signals return to the GUI thread.
7. The controller joins/destroys the worker safely.

Do not update widgets from core worker threads.

## 29.5 GUI error mapping

Map `ErrorCode` to clear dialog titles and details:

- Repository busy.
- Source permission denied.
- Destination exists.
- Unsafe restore path.
- Object missing/corrupt.
- Operation cancelled.
- Partial success.

Show technical details in an expandable area and write the full details to logs.

## 29.6 GUI state rules

- Disable conflicting actions during a mutating operation.
- Keep cancel enabled while cancellation is possible.
- Do not show a `pending` snapshot in normal history.
- Refresh statistics only after operation completion.
- Persist only UI preferences with `QSettings`; repository configuration remains in repository metadata.
- Confirm destructive delete and GC operations.
- Display dry-run GC results before allowing deletion.

## 29.7 Headless GUI tests

Set:

```bash
QT_QPA_PLATFORM=offscreen
```

for tests that instantiate Qt widgets in CI. Core tests should not require Qt.

---

# 30. Configuration

## 30.1 Repository configuration

Authoritative repository settings live in `repository_info` (immutable identity and format fields, plus `zstd_level`) and the `repository_settings` table defined in Section 14.9 (all mutable defaults).

Immutable after creation:

- Format version.
- Hash algorithm.
- Chunk size.

Mutable:

- Default zstd level for future new chunks.
- Default worker count.
- Default source root.
- Default ignore-file path.

Changing zstd level does not rewrite existing objects. Each object is a valid independent zstd frame, so decompression does not require the original level.

## 30.2 Application configuration

The CLI uses command-line flags and optional environment defaults.

The GUI uses `QSettings` for:

- Last repository path.
- Window size and position.
- Recent destinations.
- UI-only preferences.

Do not store secrets; the first release has no encryption or account credentials.

## 30.3 Application version

Generate `include/localvault/version.hpp` through CMake:

```cmake
configure_file(
    "${PROJECT_SOURCE_DIR}/include/localvault/version.hpp.in"
    "${PROJECT_BINARY_DIR}/generated/localvault/version.hpp"
    @ONLY
)
```

Template:

```cpp
#pragma once

#define LOCALVAULT_VERSION_MAJOR @PROJECT_VERSION_MAJOR@
#define LOCALVAULT_VERSION_MINOR @PROJECT_VERSION_MINOR@
#define LOCALVAULT_VERSION_PATCH @PROJECT_VERSION_PATCH@
#define LOCALVAULT_VERSION_STRING "@PROJECT_VERSION@"
```

Add the generated include directory to targets.

---

# 31. Logging and error handling

## 31.1 Logging interface

Keep logging independent of Qt:

```cpp
enum class LogLevel {
    debug,
    info,
    warning,
    error
};

struct LogRecord {
    LogLevel level{};
    std::chrono::system_clock::time_point timestamp;
    std::string component;
    std::string message;
    std::optional<std::filesystem::path> path;
};

using LogSink = std::function<void(const LogRecord&)>;
```

A simple thread-safe logger can fan out to:

- Rotating or size-limited file log.
- CLI stderr.
- GUI diagnostic model.

Do not log file content or unbounded binary data.

Logging is best-effort: when `logs/` is unwritable (for example a repository on read-only media opened with `OpenMode::read_only`), operations proceed without file logging. Read-only verification must work on read-only repositories.

## 31.2 Error context

Every error should include enough context to act on it:

Bad:

```text
open failed
```

Good:

```text
Could not open source file for reading: /data/project/report.bin
Permission denied
```

Keep the top-level message concise and store nested details through `std::nested_exception` or explicit cause fields.

## 31.3 SQLite errors

Include:

- Operation being attempted.
- SQLite primary/extended result code.
- SQLite error message.
- SQL statement name, but not user data interpolated into SQL.

Always use prepared statements and bound parameters.

## 31.4 Partial success

Per-file warnings do not have to abort the whole snapshot or restore. Return a result containing warnings and map it to exit code `6`.

Fatal errors include:

- Repository cannot be opened.
- Database transaction cannot commit.
- Required object is missing during restore.
- Object hash mismatch.
- Unsafe restore path.
- Repository format unsupported.

## 31.5 Cleanup errors

Destructors must not throw. Log best-effort cleanup failures. Explicit `close`, `commit`, or `finalize` methods may throw before control reaches a destructor.

---
