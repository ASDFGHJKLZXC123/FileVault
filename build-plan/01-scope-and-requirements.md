# LocalVault Build Plan — Part 01: Scope and Requirements

> Sections 1–4 of the Technical Implementation Guide (Revision 2), split verbatim.
> Section numbers are unchanged, so cross-references like “Section 25.10”
> resolve via the lookup table in [00-INDEX.md](00-INDEX.md).

---

# 1. Project definition

**Name:** LocalVault

**Type:** Cross-platform desktop and command-line snapshot backup application.

**Primary supported platforms:**

- Linux
- macOS
- Windows 10/11 (x64)

All three platforms are first-class from Milestone 0: every milestone must build, test, and pass CI on Linux, macOS, and Windows before it is complete. Platform-specific behavior is governed by the platform policy matrix in Section 25.10.

**Core behavior:**

- Initialize a local backup repository.
- Snapshot a selected directory.
- Store file content in fixed-size, content-addressed chunks.
- Reuse chunks already present in the repository.
- Compress newly stored chunks.
- List and inspect snapshots.
- Compare snapshots.
- Restore files, directories, or complete snapshots.
- Verify repository integrity.
- Delete old snapshots.
- Garbage-collect unreferenced objects.
- Expose the same core functionality through a CLI and a Qt desktop interface.

The core storage engine must not depend on Qt. The CLI and GUI are adapters around the same `localvault_core` library.

---

# 2. Functional requirements

Use these identifiers in issues, tests, and release notes.

## Repository management

- **FR-001:** Create a repository in an empty or explicitly approved directory.
- **FR-002:** Detect whether a directory is a valid LocalVault repository.
- **FR-003:** Reject repositories with unsupported format versions.
- **FR-004:** Prevent two writer processes from modifying one repository concurrently.
- **FR-005:** Recover or clean up incomplete operations after an interrupted run.

## Snapshot creation

- **FR-100:** Recursively scan a source directory.
- **FR-101:** Save regular-file contents.
- **FR-102:** Save empty directories.
- **FR-103:** Save symbolic links as links without following them by default. On Windows, treat directory junctions as link entries and never traverse them.
- **FR-104:** Save platform file metadata and modification time per the platform policy matrix (POSIX mode on Linux/macOS; basic file attributes on Windows).
- **FR-105:** Split regular files into 4 MiB chunks.
- **FR-106:** Hash every raw chunk with BLAKE3.
- **FR-107:** Compress every newly stored chunk with zstd.
- **FR-108:** Reuse chunks whose hashes already exist.
- **FR-109:** Save an ordered chunk list for each regular file.
- **FR-110:** Save a full-file BLAKE3 hash while streaming the file.
- **FR-111:** Mark a snapshot restorable only after all required metadata and objects are valid.
- **FR-112:** Detect files modified while they are being read.
- **FR-113:** Support cancellation without damaging completed snapshots.
- **FR-114:** Apply `.localvaultignore` rules.
- **FR-115:** Report progress through a core callback API.
- **FR-116:** Record files that cannot be opened due to Windows sharing violations as warnings and continue.
- **FR-117:** Never traverse Windows volume mount points; record them as skipped entries.
- **FR-118:** Detect cloud placeholder files (for example OneDrive Files On-Demand) and skip them with a warning instead of triggering hydration.
- **FR-119:** Support an option to stay within the source filesystem or volume during scanning.

## Snapshot inspection

- **FR-200:** List complete snapshots.
- **FR-201:** Show snapshot metadata and statistics.
- **FR-202:** Browse entries within a snapshot.
- **FR-203:** Search entries by relative path.
- **FR-204:** Compare two snapshots and classify added, removed, content-modified, metadata-modified, and unchanged entries.

## Restore

- **FR-300:** Restore one regular file.
- **FR-301:** Restore one directory recursively.
- **FR-302:** Restore an entire snapshot.
- **FR-303:** Restore to an alternate destination.
- **FR-304:** Verify each chunk hash during restore.
- **FR-305:** Verify the final file hash after reconstruction.
- **FR-306:** Write restored files through temporary files and atomically publish them.
- **FR-307:** Support overwrite policies: `never`, `prompt`, and `always`.
- **FR-308:** Reject unsafe paths and path traversal.
- **FR-309:** Restore saved modification times and platform metadata per the platform policy matrix.
- **FR-310:** Report saved paths that cannot be represented on the destination platform (reserved names, invalid characters, case or normalization collisions) as skipped entries instead of failing the whole restore.

## Verification and maintenance

- **FR-400:** Perform quick repository verification.
- **FR-401:** Perform full repository verification by decompressing and hashing every referenced object.
- **FR-402:** Detect missing objects.
- **FR-403:** Detect corrupt objects.
- **FR-404:** Detect invalid database relationships.
- **FR-405:** Delete snapshots transactionally.
- **FR-406:** Preview garbage collection.
- **FR-407:** Remove objects not referenced by any retained snapshot.
- **FR-408:** Remove stale temporary files and stale incomplete snapshots.

## User interfaces

- **FR-500:** Provide all required operations through `localvault`.
- **FR-501:** Provide snapshot, browse, restore, verify, statistics, and settings workflows through `localvault_desktop`.
- **FR-502:** Keep the Qt GUI responsive while storage operations run.
- **FR-503:** Display structured, actionable errors in both interfaces.

---

# 3. Non-functional requirements

- **NFR-001 — Language:** All application code uses C++20.
- **NFR-002 — Memory:** Files are streamed; the application must not load complete large files into memory.
- **NFR-003 — Bounded memory:** Worker queues are bounded.
- **NFR-004 — Durability:** Completed snapshots remain usable after process termination or system restart.
- **NFR-005 — Reproducibility:** Third-party dependencies are pinned through a vcpkg baseline.
- **NFR-006 — Portability:** Platform-specific code is isolated behind small platform abstractions with one POSIX and one Win32 implementation per interface.
- **NFR-007 — Testability:** Storage logic is independent of CLI and GUI code.
- **NFR-008 — Observability:** Operations expose progress and produce diagnostic logs.
- **NFR-009 — Security:** Restore operations cannot write outside the chosen destination.
- **NFR-010 — Build quality:** Project code builds without compiler warnings in CI.
- **NFR-011 — Correctness:** Snapshot/restore integration tests compare restored bytes with source bytes.
- **NFR-012 — Repository compatibility:** The repository format is versioned.
- **NFR-013 — Maintainability:** Dependencies flow in one direction and business logic does not live in Qt widgets.
- **NFR-014 — Performance:** Chunking, hashing, and compression can run in parallel, while SQLite writes remain coordinated.
- **NFR-015 — Determinism:** A given raw chunk always maps to the same object identifier and object path.

---

# 4. Scope and non-goals

## Required first release

- Linux, macOS, and Windows 10/11 (x64).
- Local repositories on a filesystem.
- One source root per snapshot.
- Fixed-size 4 MiB chunking.
- BLAKE3 content hashes.
- zstd compression.
- SQLite metadata.
- Qt 6 Widgets desktop application.
- CLI11 command-line interface.
- GoogleTest unit and integration tests.
- CMake and vcpkg.
- GitHub Actions CI.
- AddressSanitizer and UndefinedBehaviorSanitizer on Linux.

## Not required for the first release

- Cloud storage.
- Remote synchronization.
- User accounts.
- Network servers.
- Kernel extensions or filesystem drivers.
- Real-time filesystem monitoring.
- Automatic scheduling.
- Windows ACLs, alternate data streams (ADS), and reparse-point data beyond link targets.
- Volume Shadow Copy Service (VSS) snapshots; files locked by other Windows processes are skipped with warnings.
- Full POSIX ACL or extended attribute preservation.
- Retention policies (automatic pruning such as keep-last-N); snapshot deletion is manual in the first release.
- Sparse-file hole preservation.
- Hard-link relationship preservation.
- Content-defined chunking.
- Repository encryption.
- Custom cryptography.
- Distributed storage.

These may be added only after the required implementation is complete and tested.

---
