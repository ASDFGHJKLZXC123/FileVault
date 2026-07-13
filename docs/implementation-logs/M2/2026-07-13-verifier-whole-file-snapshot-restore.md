# Milestone 2 Independent Verification

Date: 2026-07-13

Milestone: Minimal whole-file snapshot/restore

Scope: current uncommitted working tree based on `905f90e0571042a0d254fee19dece17149272673`

## Verdict

**CODE COMPLETE, conditional on exact-head tri-platform CI.** I found no local code or test
blocker. The checklist totals are **16 PASS, 0 FAIL, 1 PENDING**. The sole pending row is the
combined platform gate: the macOS local suite passed, but Linux/macOS/Windows CI has not run for
the exact committed M2 tree. This log does not certify Windows.

## Verification commands

- **PASS — warning-strict development configure/build:**
  `cmake --preset development -DLOCALVAULT_WARNINGS_AS_ERRORS=ON` followed serially by
  `cmake --build --preset development --parallel 1`. The cache records
  `LOCALVAULT_WARNINGS_AS_ERRORS:BOOL=ON`; Ninja completed successfully. CMake emitted only the
  pre-existing author deprecation warning for `SQLite::SQLite3`.
- **PASS — full development tests, serial:** `ctest --preset development --parallel 1` — 74/74
  passed, 0 failed, 1.92 seconds.
- **PASS — ASan/UBSan full binary:** configured and built the `sanitizers` preset serially with
  ASan and UBSan enabled, then ran
  `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./build/sanitizers/tests/localvault_tests`
  on Apple Clang — 74/74 passed, no sanitizer diagnostic. `detect_leaks=0` is the documented Apple
  host exception.
- **PASS — full clang-format dry run:**
  `git ls-files --cached --others --exclude-standard -z -- '*.cpp' '*.hpp' | xargs -0 clang-format --dry-run --Werror --style=file`.
  This included all 63 tracked and untracked C++ source/header files.
- **PASS — whitespace:** `git diff --check`.

## Completion checklist

### Implementation

- **PASS** — `FileScanner`: uses `symlink_status`, emits normalized `/`-separated relative paths,
  emits the root row as `relative_path = ''` / `parent_path = ''` / `name = ''` (§14.4), skips
  special files with warnings.

  Evidence: `src/core/filesystem/file_scanner.cpp` uses `generic_u8string`, calls
  `symlink_status` for the root and each candidate, constructs the empty root entry, recurses only
  into directory entries, and warns on unsupported types/encodings. Proving tests:
  `FileScannerTest.EmitsRootFilesDirectoriesHiddenAndExactUnicodePaths`,
  `CapturesBrokenAndDirectorySymlinksWithoutFollowingThem`, and
  `WarnsAndSkipsSpecialEntriesAndInvalidUtf8WhereSupported`.

- **PASS** — Entries inserted for regular files, directories, and symlinks with `logical_size`,
  `modified_time_ns` (nanoseconds), `posix_mode`; symlink target text stored. (`file_hash` may stay
  NULL until M3 introduces BLAKE3 — record this explicitly in the log so it isn't mistaken for a
  bug.)

  Evidence: `ScannedEntry`, the POSIX/Win32 metadata adapters, and
  `MetadataStore::insert_entry`; `StoreFixture.InsertsAllMetadataWithFileHashNull` and
  `PreservesSymlinkTargetAndDistinctUnicodeIdentities`. `file_hash` is deliberately bound NULL in
  M2 and is recorded as such in the implementation log. Windows attributes are also NULL at this
  milestone, making FR-104/FR-309 partial as recorded below.

- **PASS** — File content stored via the minimal object store (one object per file) using
  write-temp-then-rename.

  Evidence: `ObjectStore::store` writes a unique temporary `.tmp` file and renames it to a
  deterministic lowercase-BLAKE3 `.raw` path; `SnapshotEngine` uses the bounded `Chunker`/
  `ObjectStore` interfaces. Each non-empty M2 acceptance fixture file is below the configured
  bound and has one object; empty files have none. Proving tests:
  `ObjectStoreTest.StoresRealBlake3AtDeterministicRawPathAndReusesIt`,
  `ChunkerTest.EmitsBoundedOrderedChunksAndNoChunkForAnEmptyFile`, and
  `SnapshotEngineTest.StoresCompleteFixtureWithOrderedVerifiedRawChunks`.

- **PASS** — Snapshot row is written and marked `complete` only after all entries are committed;
  `list` shows complete snapshots only.

  Evidence: `SnapshotEngine::create_snapshot` creates `pending`, performs every entry/object/
  mapping write, and only then calls `mark_snapshot_complete`; both complete lookup queries filter
  `status = 'complete'`. Proving tests: `StoreFixture.PublishesOnlyCompleteSnapshotsWithFinalCounters`,
  `SnapshotEngineTest.FatalProgressFailureMarksSnapshotFailedAndHidesItFromListings`, and the
  acceptance test's complete-list assertions.

- **PASS** — Restore order per §19.2: directories shallow→deep, then files (temp file + rename),
  then symlinks, then directory mtime/mode deepest-first.

  Evidence: `RestoreEngine::restore` builds a depth-ascending plan, executes separate directory,
  regular-file, and symlink passes, then depth-descending directory metadata. POSIX and Win32
  `TemporaryOutputFile` implementations retain the exclusively-created descriptor/handle through
  write, metadata, sync, and native publication. Proving tests:
  `RestoreEngineTest.RestoresBytesEmptyEntriesAndMetadata`,
  `NativeNoReplacePublicationPreservesDestinationAndTemporaryContent`,
  `CorruptObjectCannotReplaceExistingDestination`, and the acceptance test.

- **PASS** — Restore works to an alternate destination root (FR-303 basic form).

  Evidence: `SnapshotRestoreAcceptanceTest.RestoresDeletedFixtureIntoNonemptyAlternateDestination`
  restores to an absolute alternate root after moving the source away.

- **PASS** — Test support built and reusable: `TemporaryDirectory`, `DatasetBuilder`,
  `expect_tree_equal` with a metadata policy (§32.1).

  Evidence: `tests/support/test_filesystem.{hpp,cpp}` also supplies streaming byte comparison,
  `read_all_bytes`, `corrupt_byte`, and `truncate_file`. Proving tests are the
  `TemporaryDirectoryTest`, `DatasetBuilderTest`, `FileMutationTest`, and `TreeComparisonTest`
  suites.

### Tests (all green)

- **PASS** — Acceptance loop: `source → snapshot → delete source copy → restore →
  expect_tree_equal` passes byte-for-byte plus mtime/mode policy.

  Evidence: `SnapshotRestoreAcceptanceTest.RestoresDeletedFixtureIntoNonemptyAlternateDestination`;
  it moves the source tree out of place before restore and invokes `expect_tree_equal` with exact
  mtime and POSIX-mode comparison on this macOS run.

- **PASS** — Fixture tree includes every §32.3 shape: nested dirs, an empty dir, an empty file,
  small text files, a file with spaces, a Unicode name (one NFC and one NFD), a hidden file, a
  symlink.

  Evidence: the acceptance fixture constructs all listed shapes, including distinct on-disk NFC/
  NFD names and a broken symlink. The local run created and round-tripped the symlink (no skip).

- **PASS** — Empty file round-trips: entry row exists, `logical_size = 0`, no content object,
  restored as an empty file.

  Evidence: explicit database and restored-size assertions in the acceptance test plus
  `ChunkerTest.EmitsBoundedOrderedChunksAndNoChunkForAnEmptyFile` and
  `RestoreEngineTest.RestoresBytesEmptyEntriesAndMetadata`.

- **PASS** — Root-row rule: listing children of `''` returns top-level entries and never the root
  itself.

  Evidence: `MetadataStore::list_children` includes `relative_path <> ''`;
  `StoreFixture.ListsRootChildrenWithoutReturningTheRootRow` proves root and nested listings.

- **PASS** — Symlink round-trips as a link (target text identical), including a link whose target
  does not exist; target contents never read.

  Evidence: scanner recurses only when the no-follow status classified an entry as a directory;
  restore recreates stored link text. Proving tests:
  `FileScannerTest.CapturesBrokenAndDirectorySymlinksWithoutFollowingThem`,
  `RestoreEngineTest.RestoresBrokenSymlinkTextWithoutFollowingTarget`,
  `TreeComparisonTest.ComparesBrokenSymlinkTextWithoutFollowingTarget`, and the acceptance test.

- **PASS** — Directory mtimes survive restore (proves deepest-first metadata ordering).

  Evidence: the restore and acceptance tests assign different root/nested/deep directory mtimes
  before snapshot and compare exact restored times; the final metadata pass sorts by descending
  path depth.

- **PASS** — Restore into a non-empty destination with pre-existing unrelated files leaves those
  files untouched.

  Evidence: the acceptance test verifies the unrelated file's bytes and mtime, and
  `RestoreEngineTest.NeverPreservesExistingConflictsAndUnrelatedFiles` separately verifies both
  conflict preservation and unrelated-file preservation.

### Platform & CI

- **PENDING** — Mac local suite green; all three CI jobs green. No VM session required for this
  milestone.

  Local evidence is PASS: warning-strict macOS build, serial 74/74 CTest, and full 74/74 Apple
  ASan/UBSan binary. Exact-head GitHub Actions evidence is absent because the M2 tree is still
  uncommitted; Linux, macOS CI, and Windows remain pending. Static inspection found the intended
  Win32 source selection and implementations for no-follow reparse-point checks, case-insensitive
  component containment, `CREATE_NEW` temporary files, `SetFileTime`, `FlushFileBuffers`,
  `MoveFileExW` no-replace/replace publication, and symlink privilege degradation. Static reading
  is not Windows compilation or runtime evidence. Windows junction/mount-point source-scanner
  classification and Windows attribute policy remain the declared later-milestone partials, not
  claims made by this verification.

### Process

- **PASS** — Implementation + verification logs under `docs/implementation-logs/M2/`, including
  this checklist's state.

  Evidence: `2026-07-13-codex-whole-file-snapshot-restore.md` and this independent verifier log.

- **PASS** — Log records FR-100–FR-104 (FR-103 partial: symbolic-link handling here, with Windows
  junction/mount-point scanner rules completed in M5; FR-104 partial: POSIX metadata only until
  Windows attributes are exercised), FR-200, FR-300–FR-303 (basic forms), FR-309 (partial) — with
  proving test names.

  Evidence: the following requirement map records every requested FR and the plan's partials.

## Functional-requirement map

| Requirement | M2 state | Proving evidence |
|---|---|---|
| FR-100 — recursively scan | PASS | `FileScannerTest.EmitsRootFilesDirectoriesHiddenAndExactUnicodePaths`; acceptance test |
| FR-101 — save regular-file contents | PASS | `SnapshotEngineTest.StoresCompleteFixtureWithOrderedVerifiedRawChunks`; `ObjectStoreTest.StoresRealBlake3AtDeterministicRawPathAndReusesIt`; acceptance test |
| FR-102 — save empty directories | PASS | `FileScannerTest.EmitsRootFilesDirectoriesHiddenAndExactUnicodePaths`; `RestoreEngineTest.RestoresBytesEmptyEntriesAndMetadata`; acceptance test |
| FR-103 — save links without following | **PARTIAL through M5** | `FileScannerTest.CapturesBrokenAndDirectorySymlinksWithoutFollowingThem`; `RestoreEngineTest.RestoresBrokenSymlinkTextWithoutFollowingTarget`; acceptance test. Symbolic links are covered here; Windows junction/mount-point scanner rules remain M5. |
| FR-104 — save platform metadata/mtime | **PARTIAL** | `FileScannerTest.EmitsRootFilesDirectoriesHiddenAndExactUnicodePaths`; `StoreFixture.InsertsAllMetadataWithFileHashNull`; `RestoreEngineTest.RestoresBytesEmptyEntriesAndMetadata`. Nanosecond mtimes and POSIX modes are locally proven; Windows attribute capture is not implemented/exercised yet. |
| FR-200 — list complete snapshots | PASS (basic) | `StoreFixture.PublishesOnlyCompleteSnapshotsWithFinalCounters`; `SnapshotEngineTest.FatalProgressFailureMarksSnapshotFailedAndHidesItFromListings`; acceptance list assertions |
| FR-300 — restore one regular file | PASS (basic) | `RestoreEngineTest.RestoresOneFileOrOneDirectoryRecursively` |
| FR-301 — restore one directory recursively | PASS (basic) | `RestoreEngineTest.RestoresOneFileOrOneDirectoryRecursively` |
| FR-302 — restore entire snapshot | PASS (basic) | `SnapshotRestoreAcceptanceTest.RestoresDeletedFixtureIntoNonemptyAlternateDestination` |
| FR-303 — alternate destination | PASS (basic) | `SnapshotRestoreAcceptanceTest.RestoresDeletedFixtureIntoNonemptyAlternateDestination` |
| FR-309 — restore metadata/mtime | **PARTIAL** | `RestoreEngineTest.RestoresBytesEmptyEntriesAndMetadata`; `MetadataErrorsWarnWithoutSuppressingRestoredContent`; acceptance tree comparison. Nanosecond mtimes and POSIX modes are locally proven; Windows attributes remain later work. |

## Blockers and follow-up gate

- Code/test blockers found: **0**.
- Required closing gate: commit the reviewed M2 tree and this log, push that exact commit, and
  require all Linux, macOS, and Windows GitHub Actions jobs to pass. A Windows green job is required
  before the milestone can be called complete; the static Win32 inspection above is not a
  substitute.
