# LocalVault Build Plan — Part 09: Testing, Benchmarks, CI, and Packaging

> Sections 32–36 of the Technical Implementation Guide (Revision 2), split verbatim.
> Section numbers are unchanged, so cross-references like “Section 25.10”
> resolve via the lookup table in [00-INDEX.md](00-INDEX.md).

---

# 32. Testing strategy

Tests are required at three levels:

1. Unit tests for isolated algorithms and wrappers.
2. Integration tests using temporary repositories and real files.
3. End-to-end CLI/GUI smoke tests.

All tests must be deterministic and must not depend on a developer's home directory.

## 32.1 Test support utilities

Implement:

### `TemporaryDirectory`

- Creates a unique directory under the system temporary directory.
- Deletes it recursively in the destructor.
- Supports `release()` for failed-test debugging.
- Never copies.
- Moves safely.

### `DatasetBuilder`

Convenience methods:

```cpp
DatasetBuilder& directory(std::string_view relative_path);
DatasetBuilder& text_file(
    std::string_view relative_path,
    std::string_view contents);
DatasetBuilder& binary_file(
    std::string_view relative_path,
    std::span<const std::byte> contents);
DatasetBuilder& repeated_file(
    std::string_view relative_path,
    std::size_t size,
    std::byte value);
DatasetBuilder& symlink(
    std::string_view relative_path,
    std::string_view target);
```

### File assertions

- `expect_file_bytes_equal(a, b)`
- `expect_tree_equal(source, restored, metadata_policy)`
- `read_all_bytes(path)` only for small test files
- Streaming comparison for large test files
- `corrupt_byte(path, offset)`
- `truncate_file(path, size)`

## 32.2 Required unit tests

### Chunker

- Empty file.
- One-byte file.
- File smaller than 4 MiB.
- Exactly 4 MiB.
- 4 MiB plus one byte.
- Multiple full chunks.
- Read error in the middle of a file.
- Chunk offsets and lengths.
- Cancellation between chunks.

### BLAKE3 wrapper

- Known official test vectors stored in test data.
- Incremental updates produce the same digest as one-shot hashing.
- Empty input.
- Binary input containing zero bytes.
- Hex encoding is lowercase and exactly 64 characters.
- Move behavior if supported.

### zstd wrapper

- Empty input policy if called directly.
- Small text round trip.
- Random binary round trip.
- Incompressible data.
- Maximum allowed chunk size.
- Invalid compressed input.
- Truncated frame.
- Expected raw-size mismatch.
- Error message conversion.

### Object store

- Hash-to-path mapping.
- Shard directory creation.
- First object write.
- Reusing an existing object.
- Simulated duplicate worker write.
- Temporary-file cleanup after failure.
- Invalid hash rejection.
- Read/decompress/verify.
- Missing object.
- Corrupt object.

### Database wrappers

- Prepared statement binding.
- NULL handling.
- Transaction commit.
- Automatic rollback.
- Foreign keys enabled.
- Migration from empty database.
- Re-running migrations is idempotent.
- Unsupported schema version rejected.

### Ignore rules

- Comments and blank lines.
- Exact names.
- Wildcards.
- Directory pruning.
- Nested paths.
- Hidden files.
- Spaces.
- Case behavior.
- Invalid pattern error handling.

### Path safety

- Normal relative path accepted.
- Absolute POSIX path rejected.
- Empty path rejected where required.
- `..` rejected.
- Normalization that removes `.` accepted.
- Prefix-confusion case.
- Existing symlink ancestor rejected.
- Destination-root boundary.
- Unicode path.

### Diff engine

- Added.
- Removed.
- Type changed.
- Content modified.
- Metadata modified.
- Unchanged.
- Empty snapshots.
- Sorted streaming merge.
- Very long path.

### Statistics

- Empty repository.
- One snapshot.
- Duplicate chunks.
- Shared chunks across snapshots.
- Compression and total-savings formulas.
- Zero denominators.
- Integer-overflow checks.

## 32.3 Required integration tests

Each integration test creates isolated source, repository, and restore directories.

### Repository lifecycle

- Initialize valid repository.
- Reject non-empty destination without approval.
- Open valid repository.
- Reject random directory.
- Reject unsupported format version.
- Reject second writer lock.

### Snapshot and restore

- Empty source directory.
- Nested directories.
- Empty file.
- Small text files.
- Large binary file spanning multiple chunks.
- File exactly one chunk.
- File with spaces.
- Unicode file names.
- Hidden files.
- Symlink saved and restored.
- Unsupported special file skipped where test environment permits.
- Basic mode and mtime restoration.
- Full snapshot restored byte-for-byte.

### Deduplication

- Two identical files in one snapshot use the same chunks.
- Unchanged second snapshot stores no new content objects.
- Copying a large file adds logical bytes but no new chunks.
- Modifying one fixed-size region of a large file stores only affected fixed chunks.
- Deleting a file from a later snapshot does not remove chunks needed by an earlier snapshot.

### Incremental reuse

- Unchanged metadata reuses previous mapping.
- Changed size forces re-read.
- Changed mtime forces re-read.
- `force_rehash` reads unchanged files.
- Metadata-only mode change is represented correctly.

### Unstable files

Use a controlled test hook or custom stream abstraction:

- File changes once and succeeds after retry.
- File changes twice and is skipped.
- File disappears.
- Permission denied.
- Snapshot completes with warnings.

### Restore conflicts

- `never` leaves existing destination unchanged.
- `always` replaces through temporary publication.
- Prompt resolution callback chooses skip/replace.
- File-versus-directory conflict.
- Cancel during restore leaves no published partial file.
- Corrupt object leaves previous destination unchanged.

### Verification

- Healthy quick verification.
- Healthy full verification.
- Missing referenced object.
- Truncated object.
- Modified compressed bytes.
- Wrong `raw_size`.
- Invalid object path in database.
- Foreign-key inconsistency in a deliberately malformed test database.

### Snapshot deletion and GC

- Delete one snapshot.
- Shared objects retained.
- Unreferenced objects selected.
- Dry run deletes nothing.
- GC removes unreferenced objects and rows.
- Orphan final object detected.
- Stale temporary object removed.
- Interrupted GC is recoverable.

### Cancellation and crash-state recovery

- Cancel during scan.
- Cancel during chunk processing.
- Cancel during verification.
- Pending snapshot is not listed as complete.
- Re-open cleans pending metadata.
- Interrupted batched deletion (`deleting` status) resumes on re-open.
- Previously complete snapshots remain restorable.
- Orphan objects created before failure are removable.

### Windows-specific behavior (run on Windows CI)

- Reserved names and invalid characters are rejected by path safety and skipped by restore planning.
- Drive-relative (`C:foo`), rooted (`\foo`), and UNC paths are rejected as stored relative paths.
- A directory junction in the source is recorded as a link entry and never traversed (no recursion loop).
- A file held open with an exclusive share mode is skipped with a sharing-violation warning.
- Symlink restore without privilege degrades to skip-plus-warning.
- Paths longer than 260 characters snapshot and restore correctly.

### Cross-platform portability

- A fixture repository created on Linux opens, verifies, and restores on macOS and Windows (CI artifact hand-off), and vice versa.
- Restoring entries whose names collide case-insensitively restores one entry and skips the rest with warnings.

## 32.4 Property and invariant tests

Where practical, generate random directory trees and verify:

```text
restore(snapshot(source)) == source
```

under the supported metadata policy.

Other invariants:

- Every complete snapshot chunk reference resolves.
- Every `entry_chunks` sequence starts at zero and is contiguous.
- Sum of `raw_length` equals file `logical_size`.
- Reconstructed full-file hash equals stored `file_hash`.
- GC never selects a referenced chunk.
- Snapshot deletion does not mutate another snapshot's entries.
- Object path is a pure function of hash.

Use deterministic random seeds and print the seed on failure.

## 32.5 Failure injection

Introduce test-only injection points behind an interface, not preprocessor logic scattered through production code:

```cpp
enum class FailurePoint {
    after_temp_object_write,
    after_object_fsync,
    after_object_rename,
    before_metadata_batch_commit,
    before_snapshot_publish,
    during_restore_write
};

class FailureInjector {
public:
    virtual ~FailureInjector() = default;
    virtual void hit(FailurePoint point) = 0;
};
```

The default production injector does nothing. Tests install an injector through `Repository::set_failure_injector` (Section 12.4), throw at selected points, reopen the repository, and validate recovery.

## 32.6 Test naming

Use behavior-oriented names:

```cpp
TEST(SnapshotRestore, RestoresLargeFileByteForByte)
TEST(GarbageCollector, RetainsChunksReferencedByOlderSnapshot)
TEST(PathSafety, RejectsParentTraversalAfterNormalization)
```

Avoid numbered test names.

## 32.7 CLI end-to-end tests

`tests/cli/cli_e2e_test.py` drives the built `localvault` binary as a black box; it is registered with CTest in `tests/CMakeLists.txt` (Section 10.10) and runs on all three CI platforms.

Required coverage:

- `init → snapshot → list → show → files → diff → restore → verify → delete → gc` happy path in a temporary directory, with restored bytes compared to the source.
- Every documented exit code, including `2` (usage), `3` (missing repository), `6` (partial success), and `7` (repository busy — the test holds the lock itself).
- `--json` output parses as valid JSON and contains the schema version.
- Cancellation: send the platform interrupt signal mid-snapshot and assert exit code `130` plus a recoverable repository.

---

# 33. Benchmarking

Benchmark only optimized Release builds.

## 33.1 Metrics

Record:

- Files discovered.
- Regular files processed.
- Logical bytes.
- Unique raw bytes.
- Stored compressed bytes.
- New and reused chunk counts.
- Wall-clock snapshot duration.
- Snapshot throughput:
  ```text
  logical bytes processed / elapsed seconds
  ```
- Restore duration and throughput.
- Full-verification duration and throughput.
- Peak resident memory.
- CPU utilization when available.
- Database size.
- Object count.

## 33.2 Dataset profiles

### A. Many small files

- 50,000 to 100,000 files.
- Sizes from 0 to 16 KiB.
- Text, JSON, source, and random binary content.
- Deep and wide directories.

Purpose: scanner, path, SQLite, and metadata overhead. Expect this profile to be dominated by per-object fsync cost (two syncs per new object, Section 18.6); record objects/second alongside byte throughput so the fsync bound stays visible.

### B. Large files

- Several files from 256 MiB to multiple GiB.
- Deterministically generated content.

Purpose: streaming throughput, chunking, hashing, compression, and restore.

### C. Duplicate-heavy

- Multiple copies of the same directory.
- Repeated large files.
- Some unique small files.

Purpose: deduplication effectiveness.

### D. Incremental changes

1. Create snapshot A.
2. Modify a small region of selected large files.
3. Add and remove a small percentage of files.
4. Create snapshot B.

Purpose: incremental reuse and fixed-chunk behavior.

### E. Incompressible

- Deterministic pseudorandom bytes.

Purpose: compression overhead and memory behavior.

## 33.3 Deterministic dataset script

`benchmarks/generate_dataset.py` should accept:

```bash
python3 benchmarks/generate_dataset.py \
  --output ./benchmark-data \
  --profile duplicate-heavy \
  --seed 12345 \
  --size-gib 10
```

Write a manifest containing the generator version, seed, profile, file count, and logical bytes.

## 33.4 Benchmark output

Support JSON:

```json
{
  "schema_version": 1,
  "localvault_version": "0.1.0",
  "profile": "duplicate-heavy",
  "seed": 12345,
  "file_count": 50000,
  "logical_bytes": 10737418240,
  "unique_raw_bytes": 0,
  "stored_bytes": 0,
  "new_chunks": 0,
  "reused_chunks": 0,
  "snapshot_seconds": 0.0,
  "restore_seconds": 0.0,
  "peak_rss_bytes": 0
}
```

Populate only measured values. Keep raw benchmark outputs under `benchmarks/results/` or release artifacts, not as invented README values.

## 33.5 Measurement rules

- Run on an otherwise idle system.
- Record hardware, OS, filesystem, compiler, build type, and dependency baseline.
- Warm-cache and cold-cache results must be labeled separately.
- Run multiple iterations when practical.
- Report median and range.
- Do not compare results across different machines without labeling them.
- Do not benchmark Debug or sanitizer builds as performance results.

---

# 34. Static analysis and sanitizers

## 34.1 Formatting

Check formatting without modifying files in CI:

```bash
find include src tests benchmarks \
  \( -name '*.cpp' -o -name '*.hpp' \) -print0 |
  xargs -0 clang-format --dry-run --Werror
```

Use a pinned toolchain in CI so formatting does not change unexpectedly.

## 34.2 clang-tidy

Run from `compile_commands.json`:

```bash
run-clang-tidy \
  -p build/development \
  '^(include|src)/'
```

Third-party directories must be excluded.

Start with a manageable set of checks and resolve warnings instead of globally suppressing categories.

## 34.3 AddressSanitizer

Linux command:

```bash
cmake --preset sanitizers
cmake --build --preset sanitizers
ctest --preset sanitizers
```

Required environment:

```bash
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
```

## 34.4 UndefinedBehaviorSanitizer

Required environment:

```bash
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
```

## 34.5 ThreadSanitizer

Add a separate optional preset later. Do not combine ThreadSanitizer with AddressSanitizer in one binary. ThreadSanitizer is unavailable with MSVC; concurrency tests under TSan run on the Linux job. MSVC's `/fsanitize=address` may be added later as a separate optional Windows preset.

Run concurrency-focused tests under TSan:

- Bounded queue.
- Duplicate object storage.
- Progress aggregation.
- Cancellation.
- Error propagation.

## 34.6 Valgrind

Optional on Linux for targeted tests. Do not make it the only memory-checking approach; it is much slower and may not cover the same issues as sanitizers.

---

# 35. Continuous integration

CI must build from a clean checkout with no preinstalled project dependencies assumed beyond the runner's base toolchain.

## 35.1 Jobs

### Linux build and test

- Explicit supported Ubuntu runner image.
- Pin a supported compiler.
- Bootstrap a pinned vcpkg commit.
- Restore dependency cache keyed by:
  - OS.
  - compiler.
  - vcpkg baseline.
  - `vcpkg.json`.
- Configure Development.
- Build.
- Run tests.
- Configure Release.
- Build.
- Run tests.

### macOS build and test

- Explicit supported macOS runner image.
- Apple Clang.
- Same pinned vcpkg baseline.
- Build CLI and GUI.
- Run core tests.
- Run any Qt tests with offscreen platform.

### Windows build and test

- Explicit supported Windows runner image.
- MSVC 2022 via the Visual Studio generator presets (no developer-prompt setup step required).
- Same pinned vcpkg baseline.
- Build CLI and GUI.
- Run all tests, including the Windows-specific behavior suite (Section 32.3).

### Linux sanitizer

- GUI disabled.
- ASan and UBSan.
- All unit and integration tests.

### Static analysis

- Pinned clang-format and clang-tidy.
- Format check.
- clang-tidy on project code.

### Release

On a version tag:

- Build Release.
- Run tests.
- Package CLI and desktop app.
- Generate checksums.
- Attach artifacts to the release.

## 35.2 Action pinning policy

GitHub Action major versions and runner images change over time. When creating the workflow:

- Select currently supported action versions.
- Pin security-sensitive actions to full commit SHAs where practical.
- Enable Dependabot for GitHub Actions.
- Use explicit runner labels instead of a moving `*-latest` label when reproducibility is more important.
- Review runner deprecation notices and update intentionally.
- Never copy an old workflow without checking its Node runtime and action support status.

## 35.3 Workflow template

Replace every `PINNED_*` value with a currently supported version or commit before committing:

```yaml
name: build-and-test

on:
  push:
  pull_request:

permissions:
  contents: read

env:
  VCPKG_COMMIT: PINNED_VCPKG_COMMIT
  # vcpkg binary caching through the GitHub Actions cache. Without this,
  # every run rebuilds Qt from source (~1 hour per platform).
  VCPKG_BINARY_SOURCES: "clear;x-gha,readwrite"

jobs:
  linux:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@PINNED_CHECKOUT_VERSION_OR_SHA

      - name: Export GitHub Actions cache variables for vcpkg
        uses: actions/github-script@PINNED_GITHUB_SCRIPT_VERSION_OR_SHA
        with:
          script: |
            core.exportVariable('ACTIONS_CACHE_URL', process.env.ACTIONS_CACHE_URL || '');
            core.exportVariable('ACTIONS_RUNTIME_TOKEN', process.env.ACTIONS_RUNTIME_TOKEN || '');

      - name: Install build tools and Qt system prerequisites
        run: |
          sudo apt-get update
          sudo apt-get install -y ninja-build pkg-config autoconf automake libtool \
            libx11-dev libx11-xcb-dev libxext-dev libxrender-dev libxi-dev \
            libxkbcommon-dev libxkbcommon-x11-dev libgl1-mesa-dev libglu1-mesa-dev \
            libxrandr-dev libxcursor-dev libxdamage-dev libxinerama-dev \
            '^libxcb.*-dev'
          # qtbase built through vcpkg requires the X11/xcb/OpenGL development
          # packages above; check the vcpkg qtbase port docs for the current list.

      - name: Bootstrap pinned vcpkg
        run: |
          git clone https://github.com/microsoft/vcpkg.git "$HOME/vcpkg"
          git -C "$HOME/vcpkg" checkout "$VCPKG_COMMIT"
          "$HOME/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
          echo "VCPKG_ROOT=$HOME/vcpkg" >> "$GITHUB_ENV"
          echo "$HOME/vcpkg" >> "$GITHUB_PATH"

      - name: Configure
        run: cmake --preset development -DLOCALVAULT_WARNINGS_AS_ERRORS=ON

      - name: Build
        run: cmake --build --preset development --parallel

      - name: Test
        run: ctest --preset development

  macos:
    runs-on: macos-15
    steps:
      - uses: actions/checkout@PINNED_CHECKOUT_VERSION_OR_SHA

      - name: Export GitHub Actions cache variables for vcpkg
        uses: actions/github-script@PINNED_GITHUB_SCRIPT_VERSION_OR_SHA
        with:
          script: |
            core.exportVariable('ACTIONS_CACHE_URL', process.env.ACTIONS_CACHE_URL || '');
            core.exportVariable('ACTIONS_RUNTIME_TOKEN', process.env.ACTIONS_RUNTIME_TOKEN || '');

      - name: Install Ninja
        run: brew install ninja

      - name: Bootstrap pinned vcpkg
        run: |
          git clone https://github.com/microsoft/vcpkg.git "$HOME/vcpkg"
          git -C "$HOME/vcpkg" checkout "$VCPKG_COMMIT"
          "$HOME/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
          echo "VCPKG_ROOT=$HOME/vcpkg" >> "$GITHUB_ENV"
          echo "$HOME/vcpkg" >> "$GITHUB_PATH"

      - name: Configure
        run: cmake --preset development -DLOCALVAULT_WARNINGS_AS_ERRORS=ON

      - name: Build
        run: cmake --build --preset development --parallel

      - name: Test
        env:
          QT_QPA_PLATFORM: offscreen
        run: ctest --preset development

  windows:
    runs-on: windows-2022
    steps:
      - uses: actions/checkout@PINNED_CHECKOUT_VERSION_OR_SHA

      - name: Export GitHub Actions cache variables for vcpkg
        uses: actions/github-script@PINNED_GITHUB_SCRIPT_VERSION_OR_SHA
        with:
          script: |
            core.exportVariable('ACTIONS_CACHE_URL', process.env.ACTIONS_CACHE_URL || '');
            core.exportVariable('ACTIONS_RUNTIME_TOKEN', process.env.ACTIONS_RUNTIME_TOKEN || '');

      - name: Bootstrap pinned vcpkg
        shell: pwsh
        run: |
          git clone https://github.com/microsoft/vcpkg.git "$env:USERPROFILE\vcpkg"
          git -C "$env:USERPROFILE\vcpkg" checkout "$env:VCPKG_COMMIT"
          & "$env:USERPROFILE\vcpkg\bootstrap-vcpkg.bat" -disableMetrics
          Add-Content $env:GITHUB_ENV "VCPKG_ROOT=$env:USERPROFILE\vcpkg"
          Add-Content $env:GITHUB_PATH "$env:USERPROFILE\vcpkg"

      - name: Configure
        run: cmake --preset windows-development -DLOCALVAULT_WARNINGS_AS_ERRORS=ON

      - name: Build
        run: cmake --build --preset windows-development-debug --parallel

      - name: Test
        run: ctest --preset windows-development-debug
```

This is a structural template, not a source of permanent version numbers.

## 35.4 vcpkg caching

Enable vcpkg binary caching from the very first CI commit — it is not an optimization to defer. Building Qt from source takes on the order of an hour per platform, so an uncached workflow makes every push pay roughly three machine-hours. The template above uses the GitHub Actions cache backend (`VCPKG_BINARY_SOURCES=clear;x-gha,readwrite` plus the exported `ACTIONS_CACHE_URL`/`ACTIONS_RUNTIME_TOKEN` variables).

Cache keys must include the vcpkg commit/baseline and triplet. Never reuse arbitrary binary caches across incompatible compilers or operating systems.

## 35.5 CI failure policy

A pull request cannot merge when:

- Build fails.
- Any warning is emitted from project code — CI configures every platform with `-DLOCALVAULT_WARNINGS_AS_ERRORS=ON`; this is the enforcement mechanism for NFR-010.
- Tests fail.
- Format check fails.
- Sanitizer detects an issue.
- Required static-analysis checks fail.
- Generated schema or version files are stale.

---

# 36. Packaging and release

## 36.1 Install rules

Add CMake install rules:

```cmake
include(GNUInstallDirs)

install(
    TARGETS localvault localvault_core
    EXPORT LocalVaultTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)

if(LOCALVAULT_BUILD_GUI)
    install(
        TARGETS localvault_desktop
        BUNDLE DESTINATION .
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    )
endif()

install(
    DIRECTORY "${PROJECT_SOURCE_DIR}/include/localvault"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
```

If `localvault_core` is intended only as an internal library, do not install public development headers in the first release. Install only the executables and runtime dependencies.

## 36.2 CPack

Initial package formats:

- Linux: `.tar.gz` archive.
- macOS: `.zip` or CPack-generated bundle archive.
- Windows: `.zip` archive; an NSIS or MSIX installer is deferred until after the first release.

```cmake
set(CPACK_PACKAGE_NAME "LocalVault")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "LocalVault")
include(CPack)
```

Build package:

```bash
cmake --preset release
cmake --build --preset release
cmake --install build/release --prefix build/stage
cpack --config build/release/CPackConfig.cmake
```

## 36.3 Qt deployment

The macOS app bundle must include required Qt frameworks/plugins using the Qt deployment support appropriate to the installed Qt version (`macdeployqt`). On Windows, run `windeployqt` against `localvault_desktop.exe` so the archive contains the Qt DLLs and platform plugins. Validate every packaged app on a machine without the development Qt installation.

On Linux, start with a documented archive or AppImage only after the core release works. Do not delay core correctness for installer polish.

Code signing status must be decided and documented before public distribution:

- **macOS:** unsigned, un-notarized apps are blocked by Gatekeeper on default settings; users must right-click → Open. Normal double-click launch requires a Developer ID certificate plus notarization.
- **Windows:** unsigned executables trigger SmartScreen warnings until reputation accrues; Authenticode signing removes them.

The first release may ship unsigned with these workarounds documented in the release notes, but the decision must be explicit, not accidental.

## 36.4 Release checklist

- Version updated.
- `CHANGELOG.md` updated.
- Repository format compatibility documented.
- vcpkg baseline committed.
- Clean Linux, macOS, and Windows builds pass.
- Tests and sanitizers pass.
- Full verification passes on a test repository.
- Package tested on a clean environment for each platform (including a Windows machine without Visual Studio and a macOS machine without Qt).
- Code signing / notarization status decided and documented in the release notes.
- Checksums generated.
- Tag follows semantic versioning, for example `v0.1.0`.
- Release artifacts contain license and documentation.
- No benchmark numbers are published without environment details.

---
