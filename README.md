LocalVault
==========

LocalVault is an offline-first, content-addressed snapshot backup application.

The core library supports:

- Chunked snapshots with BLAKE3, zstd, deduplication, cancellation, and crash recovery
- Safe restoration with byte verification
- Snapshot browsing, path search, streaming diff, and storage statistics
- Quick/full and whole-file integrity verification, resumable snapshot deletion, and GC preview/execution

CMake + vcpkg presets build and test Linux, macOS, and Windows. GitHub Actions also runs
ASan/UBSan and TSan. The CLI exposes the core operations; the Qt interface remains an M8 scaffold.

## Command-line use

```text
localvault init /path/to/vault
localvault --repo /path/to/vault snapshot /path/to/source --message "First backup"
localvault --repo /path/to/vault list --json
localvault --repo /path/to/vault restore 1 --all --output /path/to/restored
localvault --repo /path/to/vault verify --files
```

Use `localvault <command> --help` for flags. Restore conflicts default to skipping existing files;
`--overwrite prompt` accepts terminal or scripted answers, including apply-to-all choices.
JSON results use schema version 1 on stdout; diagnostics and progress use stderr.
First Ctrl+C requests cancellation; a second interrupt forces exit 130.

M7 acceptance evidence and any remaining gates are recorded in
[the verification log](docs/implementation-logs/M7/verification.md).

## Build plan

The [build plan index](build-plan/00-INDEX.md) links to the technical design, milestone specifications, and orchestration guides. Start with the [M0–M9 milestone overview](build-plan/10-milestones-and-definition-of-done.md) for the implementation sequence and completion criteria.
