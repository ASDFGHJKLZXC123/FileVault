# Milestone 2 Implementation Log

Date: 2026-07-13
Milestone: Minimal whole-file snapshot/restore

## Decisions

- Keep the engine single-threaded. `FileScanner` emits deterministic normalized entries, including
  the `''` root row, without following links; unsupported entries become snapshot warnings.
- Keep the M2 storage format deliberately minimal: each small non-empty fixture file is one raw
  `.raw` object addressed by its real lowercase BLAKE3 content ID. Empty files have zero chunk
  mappings. `file_hash` remains NULL, compression is not used, and M3 owns full-file hashes,
  content-defined chunking, compression, and the permanent object format. The bounded `Chunker`
  and `ObjectStore` interfaces already have their required architectural shape.
- Publish snapshots only by transitioning `pending` to `complete` after all entries, mappings,
  counters, and warnings are stored. Failed or cancelled snapshots remain hidden from complete-only
  listings.
- Restore under the repository writer lock, validate all stored paths and chunk relationships
  before mutation, create directories shallow-first, publish files and links without replacement
  races, and apply directory metadata deepest-last. File temporary output retains its exclusive
  descriptor/handle from cryptographically random same-directory creation through write, metadata,
  sync, and native publication.
- Compare resolved containment components exactly on POSIX and case-insensitively on Windows.
  Restore rejects POSIX symlinks and every Windows reparse point in existing ancestor chains.
- Treat metadata application errors as warnings after verified content succeeds; content remains
  restored and later entries continue. Content, object verification, path safety, and publication
  errors remain fatal.

## Plan amendment and deliberate boundaries

- M2 originally asked its log to map all of FR-103 even though M5 explicitly owns Windows
  junction/mount-point scanner classification and `one_file_system`. The M2 milestone and
  orchestrator now mark FR-103 partial until M5. M2 still rejects all Windows reparse points in
  restore ancestors as a current containment requirement.
- FR-104 and FR-309 remain partial as planned: nanosecond mtimes and POSIX modes are exercised
  locally; full Windows attribute policy is not claimed. Crash recovery and durable operation
  journals remain M4 work.
- M2 does not claim FR-105/106/108/109 or FR-304. Integration and acceptance fixtures use the
  default size so every small non-empty file has one object; only the unit-level bounded chunker
  interface is exercised with multiple callbacks.

## Functional requirements and proving tests

- **FR-100/101/102** — `FileScannerTest.EmitsRootFilesDirectoriesHiddenAndExactUnicodePaths`,
  `SnapshotEngineTest.StoresCompleteFixtureWithOrderedVerifiedRawChunks`, and
  `SnapshotRestoreAcceptanceTest.RestoresDeletedFixtureIntoNonemptyAlternateDestination`.
- **FR-103 (partial through M5)** —
  `FileScannerTest.CapturesBrokenAndDirectorySymlinksWithoutFollowingThem`,
  `RestoreEngineTest.RestoresBrokenSymlinkTextWithoutFollowingTarget`, and the acceptance test.
- **FR-104 (partial)** — `RestoreEngineTest.RestoresBytesEmptyEntriesAndMetadata` and
  `MetadataErrorsWarnWithoutSuppressingRestoredContent`.
- **FR-200** — `StoreFixture.PublishesOnlyCompleteSnapshotsWithFinalCounters` and
  `SnapshotEngineTest.FatalProgressFailureMarksSnapshotFailedAndHidesItFromListings`.
- **FR-300/301** — `RestoreEngineTest.RestoresOneFileOrOneDirectoryRecursively`.
- **FR-302/303** — the full alternate-destination acceptance test.
- **FR-309 (partial)** — `RestoreEngineTest.RestoresBytesEmptyEntriesAndMetadata`, the acceptance
  tree comparison, and `MetadataErrorsWarnWithoutSuppressingRestoredContent`.

## Gotchas for later milestones

- No-replace publication needs native primitives. On macOS, hard-link publication follows a broken
  symlink's missing target, so restore uses `renamex_np(RENAME_EXCL)`; Linux uses
  `renameat2(RENAME_NOREPLACE)`, and Windows uses `MoveFileExW` without replacement.
- Do not reopen an exclusively created restore temp pathname: retain and write the exact native
  descriptor/handle. Random naming reduces discovery; full descriptor-relative ancestor TOCTOU
  hardening remains outside M2's minimum and must be revisited with later recovery work.
- Serialize CMake builds in the shared multi-agent worktree. Concurrent test links once produced a
  partial generated GoogleTest discovery file even though the code was sound.
- The first exact-head CI exposed standard-library/compiler differences absent on the development
  Mac: Apple libc++ keeps C++20 stop tokens behind `-fexperimental-library`, GCC diagnoses omitted
  aggregate fields under `-Werror`, and MSVC lacks `file_clock::to_sys`/`from_sys`. Keep progress
  aggregates explicit and use the platform metadata adapter as the sole nanosecond-time source.
- The sanitizer preset's `detect_leaks=1` is unsupported by Apple AddressSanitizer on this host;
  run the complete binary with leak detection disabled while retaining ASan and UBSan.

## Local evidence

- Warning-strict development configure/build: passed (only the pre-existing SQLite target author
  deprecation warning).
- Development CTest: 74/74 passed.
- ASan/UBSan direct test binary with `detect_leaks=0`: 74/74 passed.
- Full-tree clang-format dry run and `git diff --check`: passed.
- Fresh repair invariant re-review: 0 blockers; independent verification found 0 code/test
  blockers with exact-head tri-platform CI as the sole pending gate.
