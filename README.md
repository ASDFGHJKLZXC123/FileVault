LocalVault
==========

LocalVault is an offline-first, content-addressed snapshot backup application.

The core library supports:

- Chunked snapshots with BLAKE3, zstd, deduplication, cancellation, and crash recovery
- Safe restoration with byte verification
- Snapshot browsing, path search, streaming diff, and storage statistics
- Quick/full integrity verification, resumable snapshot deletion, and GC preview/execution

CMake + vcpkg presets build and test Linux, macOS, and Windows. GitHub Actions also runs
ASan/UBSan and TSan. The CLI and Qt targets remain scaffolds; M7 and M8 add their complete interfaces.

## Build plan

The [build plan index](build-plan/00-INDEX.md) links to the technical design, milestone specifications, and orchestration guides. Start with the [M0–M9 milestone overview](build-plan/10-milestones-and-definition-of-done.md) for the implementation sequence and completion criteria.
