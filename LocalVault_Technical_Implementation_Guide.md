# LocalVault — Technical Implementation Guide

This document is the complete engineering specification for building **LocalVault** from an empty repository. It intentionally contains only implementation, architecture, build, testing, packaging, and maintenance information.

**Revision 2.** This revision adds Windows 10/11 as a first-release platform, resolves contradictions found in planning review (locking rules, deletion batching, settings storage, bootstrap ordering), and adds platform-policy, portability, and CI-hardening material. Top-level section numbering is unchanged from Revision 1.

---

## Table of contents

1. [Project definition](#1-project-definition)
2. [Functional requirements](#2-functional-requirements)
3. [Non-functional requirements](#3-non-functional-requirements)
4. [Scope and non-goals](#4-scope-and-non-goals)
5. [Technology stack](#5-technology-stack)
6. [Supported environments and prerequisites](#6-supported-environments-and-prerequisites)
7. [Repository layout](#7-repository-layout)
8. [Bootstrap the repository](#8-bootstrap-the-repository)
9. [Dependency management](#9-dependency-management)
10. [CMake configuration](#10-cmake-configuration)
11. [Architecture and dependency rules](#11-architecture-and-dependency-rules)
12. [Core public API](#12-core-public-api)
13. [Repository-on-disk format](#13-repository-on-disk-format)
14. [SQLite schema](#14-sqlite-schema)
15. [Repository initialization](#15-repository-initialization)
16. [Snapshot algorithm](#16-snapshot-algorithm)
17. [Incremental snapshot optimization](#17-incremental-snapshot-optimization)
18. [Chunking, hashing, compression, and deduplication](#18-chunking-hashing-compression-and-deduplication)
19. [Restore algorithm](#19-restore-algorithm)
20. [Snapshot diff](#20-snapshot-diff)
21. [Integrity verification](#21-integrity-verification)
22. [Snapshot deletion and garbage collection](#22-snapshot-deletion-and-garbage-collection)
23. [Crash consistency and recovery](#23-crash-consistency-and-recovery)
24. [Concurrency and cancellation](#24-concurrency-and-cancellation)
25. [Filesystem behavior](#25-filesystem-behavior)
26. [Restore security](#26-restore-security)
27. [Ignore rules](#27-ignore-rules)
28. [Command-line interface](#28-command-line-interface)
29. [Qt desktop application](#29-qt-desktop-application)
30. [Configuration](#30-configuration)
31. [Logging and error handling](#31-logging-and-error-handling)
32. [Testing strategy](#32-testing-strategy)
33. [Benchmarking](#33-benchmarking)
34. [Static analysis and sanitizers](#34-static-analysis-and-sanitizers)
35. [Continuous integration](#35-continuous-integration)
36. [Packaging and release](#36-packaging-and-release)
37. [Implementation milestones](#37-implementation-milestones)
38. [Definition of done](#38-definition-of-done)
39. [Known limitations](#39-known-limitations)
40. [Optional advanced work](#40-optional-advanced-work)
41. [Build and dependency maintenance](#41-build-and-dependency-maintenance)
42. [Appendix: implementation skeletons](#42-appendix-implementation-skeletons)

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

# 5. Technology stack

| Area | Selection |
|---|---|
| Language | C++20 |
| GUI | Qt 6 Widgets |
| Build system | CMake 3.24 or newer |
| Build generator | Ninja |
| Dependency management | vcpkg manifest mode with a pinned baseline |
| Metadata database | SQLite |
| Compression | zstd |
| Content hash | BLAKE3 |
| CLI parsing | CLI11 |
| JSON serialization for CLI output | nlohmann/json |
| Unit/integration tests | GoogleTest |
| Filesystem abstraction | `std::filesystem` plus a small POSIX layer |
| Formatting | clang-format |
| Static analysis | clang-tidy |
| Runtime checking | AddressSanitizer and UndefinedBehaviorSanitizer |
| CI | GitHub Actions |
| Packaging | CPack initially; native installers later |
| License file | MIT unless another license is deliberately selected |

Compilers: GCC or Clang on Linux, Apple Clang on macOS, MSVC 2022 (v143) on Windows. All dependencies above have supported vcpkg ports on all three platforms.

Do not use deprecated low-level OpenSSL digest APIs. LocalVault uses BLAKE3 directly, so OpenSSL is not required.

---

# 6. Supported environments and prerequisites

## Required tools

Install:

- Git
- A C++20-capable compiler (GCC/Clang on Linux, Apple Clang on macOS, Visual Studio 2022 with the C++ workload on Windows)
- CMake 3.24 or newer
- Ninja (Linux and macOS; Windows uses the Visual Studio generator presets)
- Python 3 for utility scripts and benchmark dataset generation
- vcpkg
- A debugger such as GDB, LLDB, or the Visual Studio debugger

## Compiler policy

Do not hard-code compiler-specific language extensions. Set:

```cmake
CMAKE_CXX_EXTENSIONS=OFF
```

The build must validate actual compiler support instead of relying only on compiler version strings:

```cmake
target_compile_features(localvault_core PUBLIC cxx_std_20)
```

## Environment variables

Set `VCPKG_ROOT` to the vcpkg checkout:

```bash
export VCPKG_ROOT="$HOME/tools/vcpkg"
export PATH="$VCPKG_ROOT:$PATH"
```

Persist these in the shell configuration only after confirming the location.

## Recommended local build matrix

At minimum, test:

- Clang on macOS.
- GCC or Clang on Linux.
- MSVC on Windows.
- Debug build.
- Release build.
- Linux sanitizer build.

---

# 7. Repository layout

Create this structure:

```text
LocalVault/
├── CMakeLists.txt
├── CMakePresets.json
├── vcpkg.json
├── .clang-format
├── .clang-tidy
├── .gitignore
├── LICENSE
├── README.md
├── CHANGELOG.md
│
├── cmake/
│   ├── CompilerWarnings.cmake
│   ├── Dependencies.cmake
│   ├── Sanitizers.cmake
│   └── StaticAnalysis.cmake
│
├── include/
│   └── localvault/
│       ├── error.hpp
│       ├── progress.hpp
│       ├── repository.hpp
│       ├── snapshot_engine.hpp
│       ├── restore_engine.hpp
│       ├── diff_engine.hpp
│       ├── integrity_verifier.hpp
│       ├── garbage_collector.hpp
│       ├── query_service.hpp
│       ├── types.hpp
│       └── version.hpp.in
│
├── src/
│   ├── core/
│   │   ├── CMakeLists.txt
│   │   ├── database/
│   │   │   ├── database.cpp
│   │   │   ├── migrations.cpp
│   │   │   ├── statement.cpp
│   │   │   └── transaction.cpp
│   │   ├── filesystem/
│   │   │   ├── file_scanner.cpp
│   │   │   ├── ignore_rules.cpp
│   │   │   ├── path_safety.cpp
│   │   │   └── platform/
│   │   │       ├── platform_lock.hpp
│   │   │       ├── platform_metadata.hpp
│   │   │       ├── posix_lock.cpp
│   │   │       ├── posix_metadata.cpp
│   │   │       ├── win32_lock.cpp
│   │   │       └── win32_metadata.cpp
│   │   ├── storage/
│   │   │   ├── chunker.cpp
│   │   │   ├── blake3_hasher.cpp
│   │   │   ├── zstd_codec.cpp
│   │   │   └── object_store.cpp
│   │   ├── repository.cpp
│   │   ├── snapshot_engine.cpp
│   │   ├── restore_engine.cpp
│   │   ├── diff_engine.cpp
│   │   ├── integrity_verifier.cpp
│   │   ├── garbage_collector.cpp
│   │   ├── query_service.cpp
│   │   ├── logging.cpp
│   │   └── error.cpp
│   │
│   ├── cli/
│   │   ├── CMakeLists.txt
│   │   ├── main.cpp
│   │   ├── cli_app.cpp
│   │   ├── output.cpp
│   │   ├── signal_handler.cpp
│   │   └── commands/
│   │       ├── init_command.cpp
│   │       ├── snapshot_command.cpp
│   │       ├── list_command.cpp
│   │       ├── show_command.cpp
│   │       ├── files_command.cpp
│   │       ├── diff_command.cpp
│   │       ├── restore_command.cpp
│   │       ├── verify_command.cpp
│   │       ├── stats_command.cpp
│   │       ├── delete_command.cpp
│   │       └── gc_command.cpp
│   │
│   └── desktop/
│       ├── CMakeLists.txt
│       ├── main.cpp
│       ├── main_window.cpp
│       ├── main_window.hpp
│       ├── controllers/
│       │   ├── repository_controller.cpp
│       │   └── operation_controller.cpp
│       ├── models/
│       │   ├── snapshot_table_model.cpp
│       │   └── snapshot_tree_model.cpp
│       ├── pages/
│       │   ├── dashboard_page.cpp
│       │   ├── snapshots_page.cpp
│       │   ├── restore_page.cpp
│       │   ├── verify_page.cpp
│       │   └── settings_page.cpp
│       └── workers/
│           └── core_operation_worker.cpp
│
├── tests/
│   ├── CMakeLists.txt
│   ├── unit/
│   │   ├── chunker_test.cpp
│   │   ├── hasher_test.cpp
│   │   ├── codec_test.cpp
│   │   ├── ignore_rules_test.cpp
│   │   ├── path_safety_test.cpp
│   │   └── diff_test.cpp
│   ├── integration/
│   │   ├── repository_test.cpp
│   │   ├── snapshot_restore_test.cpp
│   │   ├── deduplication_test.cpp
│   │   ├── corruption_test.cpp
│   │   ├── cancellation_test.cpp
│   │   └── garbage_collection_test.cpp
│   ├── cli/
│   │   └── cli_e2e_test.py
│   └── support/
│       ├── temporary_directory.cpp
│       ├── dataset_builder.cpp
│       └── file_assertions.cpp
│
├── benchmarks/
│   ├── CMakeLists.txt
│   ├── benchmark_main.cpp
│   └── generate_dataset.py
│
├── docs/
│   ├── architecture.md
│   ├── repository-format.md
│   ├── crash-consistency.md
│   ├── database-schema.md
│   ├── user-guide.md
│   ├── implementation-logs/
│   └── screenshots/
│
├── scripts/
│   ├── format.sh
│   ├── check-format.sh
│   ├── run-clang-tidy.sh
│   ├── run-sanitizers.sh
│   └── package.sh
│
└── .github/
    ├── dependabot.yml
    └── workflows/
        ├── build-test.yml
        ├── static-analysis.yml
        └── release.yml
```

Target ownership:

- `localvault_core`: everything under `src/core`.
- `localvault`: everything under `src/cli`.
- `localvault_desktop`: everything under `src/desktop`.
- `localvault_tests`: all tests.
- `localvault_benchmarks`: benchmark driver.

---

# 8. Bootstrap the repository

## 8.1 Create the directory and initialize Git

```bash
mkdir LocalVault
cd LocalVault
git init
mkdir -p cmake include/localvault src/core src/cli src/desktop tests benchmarks docs scripts
touch CMakeLists.txt CMakePresets.json vcpkg.json
touch README.md CHANGELOG.md LICENSE .clang-format .clang-tidy .gitignore
```

## 8.2 Install vcpkg

Keep vcpkg outside the project repository:

```bash
mkdir -p "$HOME/tools"
cd "$HOME/tools"
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
./bootstrap-vcpkg.sh
export VCPKG_ROOT="$PWD"
```

On Windows, run `bootstrap-vcpkg.bat` from a Developer PowerShell and set `VCPKG_ROOT` as a user environment variable.

## 8.3 Select and record a vcpkg baseline

First write the manifest content from Section 9.1 into `vcpkg.json` — the baseline tool fails on the empty file created in Section 8.1. Then, from the project root:

```bash
"$VCPKG_ROOT/vcpkg" x-update-baseline --add-initial-baseline
```

Commit the resulting `builtin-baseline` in `vcpkg.json`. Do not leave a placeholder baseline in the committed project.

## 8.4 Initial build

After the files in Sections 9 and 10 are present:

```bash
cmake --preset development
cmake --build --preset development
ctest --preset development
```

---

# 9. Dependency management

Use vcpkg manifest mode. Do not install project dependencies globally and do not rely on unversioned packages from a developer machine.

## 9.1 `vcpkg.json`

Use this as the starting manifest:

```json
{
  "name": "localvault",
  "version-semver": "0.1.0",
  "builtin-baseline": "REPLACE_WITH_GENERATED_VCPKG_COMMIT",
  "dependencies": [
    "blake3",
    "cli11",
    "gtest",
    "nlohmann-json",
    "qtbase",
    "sqlite3",
    "zstd"
  ]
}
```

Replace the baseline before the first commit.

## 9.2 Dependency update policy

- Update the baseline in a dedicated pull request.
- Build and run all tests on Linux, macOS, and Windows before merging.
- Record meaningful dependency changes in `CHANGELOG.md`.
- Never update dependencies and change repository format in the same pull request unless required.
- Keep a known-good baseline available in Git history.
- Do not use floating Git branches for vendored dependencies.
- Do not copy arbitrary single-header files from tutorials.

## 9.3 Dependency abstraction

Wrap external C APIs behind LocalVault classes:

- `Blake3Hasher`
- `ZstdCodec`
- `Database`
- `Statement`
- `Transaction`

Only these wrappers should include third-party C headers. Most of `localvault_core` should include LocalVault headers only.

---

# 10. CMake configuration

## 10.1 Root `CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.24)

project(
    LocalVault
    VERSION 0.1.0
    DESCRIPTION "Snapshot backup and deduplication application"
    LANGUAGES C CXX
)

include(CTest)

option(LOCALVAULT_BUILD_GUI "Build the Qt desktop application" ON)
option(LOCALVAULT_BUILD_BENCHMARKS "Build benchmark tools" ON)
option(LOCALVAULT_WARNINGS_AS_ERRORS "Treat LocalVault warnings as errors" OFF)
option(LOCALVAULT_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(LOCALVAULT_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(LOCALVAULT_ENABLE_CLANG_TIDY "Run clang-tidy during compilation" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/generated/localvault")
configure_file(
    "${PROJECT_SOURCE_DIR}/include/localvault/version.hpp.in"
    "${PROJECT_BINARY_DIR}/generated/localvault/version.hpp"
    @ONLY
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(Dependencies)
include(CompilerWarnings)
include(Sanitizers)
include(StaticAnalysis)

add_subdirectory(src/core)
add_subdirectory(src/cli)

if(LOCALVAULT_BUILD_GUI)
    add_subdirectory(src/desktop)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

if(LOCALVAULT_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
```

Do not apply compiler options globally. Apply them only to LocalVault targets.

## 10.2 `CMakePresets.json`

```json
{
  "version": 5,
  "cmakeMinimumRequired": {
    "major": 3,
    "minor": 24,
    "patch": 0
  },
  "configurePresets": [
    {
      "name": "base",
      "hidden": true,
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/${presetName}",
      "toolchainFile": "$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake",
      "cacheVariables": {
        "CMAKE_EXPORT_COMPILE_COMMANDS": "ON",
        "CMAKE_CXX_EXTENSIONS": "OFF"
      }
    },
    {
      "name": "development",
      "inherits": "base",
      "displayName": "Development",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug",
        "BUILD_TESTING": "ON",
        "LOCALVAULT_BUILD_GUI": "ON",
        "LOCALVAULT_BUILD_BENCHMARKS": "ON"
      }
    },
    {
      "name": "release",
      "inherits": "base",
      "displayName": "Release",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Release",
        "BUILD_TESTING": "ON",
        "LOCALVAULT_BUILD_GUI": "ON",
        "LOCALVAULT_BUILD_BENCHMARKS": "ON"
      }
    },
    {
      "name": "sanitizers",
      "inherits": "base",
      "displayName": "ASan and UBSan",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug",
        "BUILD_TESTING": "ON",
        "LOCALVAULT_BUILD_GUI": "OFF",
        "LOCALVAULT_BUILD_BENCHMARKS": "OFF",
        "LOCALVAULT_ENABLE_ASAN": "ON",
        "LOCALVAULT_ENABLE_UBSAN": "ON"
      }
    },
    {
      "name": "windows-base",
      "hidden": true,
      "generator": "Visual Studio 17 2022",
      "architecture": "x64",
      "binaryDir": "${sourceDir}/build/${presetName}",
      "toolchainFile": "$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake",
      "cacheVariables": {
        "CMAKE_CXX_EXTENSIONS": "OFF"
      },
      "condition": {
        "type": "equals",
        "lhs": "${hostSystemName}",
        "rhs": "Windows"
      }
    },
    {
      "name": "windows-development",
      "inherits": "windows-base",
      "displayName": "Windows Development",
      "cacheVariables": {
        "BUILD_TESTING": "ON",
        "LOCALVAULT_BUILD_GUI": "ON",
        "LOCALVAULT_BUILD_BENCHMARKS": "ON"
      }
    }
  ],
  "buildPresets": [
    {
      "name": "development",
      "configurePreset": "development"
    },
    {
      "name": "release",
      "configurePreset": "release"
    },
    {
      "name": "sanitizers",
      "configurePreset": "sanitizers"
    },
    {
      "name": "windows-development-debug",
      "configurePreset": "windows-development",
      "configuration": "Debug"
    },
    {
      "name": "windows-development-release",
      "configurePreset": "windows-development",
      "configuration": "Release"
    }
  ],
  "testPresets": [
    {
      "name": "development",
      "configurePreset": "development",
      "output": {
        "outputOnFailure": true
      }
    },
    {
      "name": "release",
      "configurePreset": "release",
      "output": {
        "outputOnFailure": true
      }
    },
    {
      "name": "sanitizers",
      "configurePreset": "sanitizers",
      "output": {
        "outputOnFailure": true
      },
      "environment": {
        "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
        "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"
      }
    },
    {
      "name": "windows-development-debug",
      "configurePreset": "windows-development",
      "configuration": "Debug",
      "output": {
        "outputOnFailure": true
      }
    },
    {
      "name": "windows-development-release",
      "configurePreset": "windows-development",
      "configuration": "Release",
      "output": {
        "outputOnFailure": true
      }
    }
  ]
}
```

On Windows, use the `windows-development` preset (Visual Studio generator, multi-config); no developer command prompt is required. The Windows presets omit `CMAKE_BUILD_TYPE` because the generator is multi-config — the build and test presets select the configuration. CI enforces NFR-010 by configuring every platform with `-DLOCALVAULT_WARNINGS_AS_ERRORS=ON` (Section 35.5).

## 10.3 `cmake/Dependencies.cmake`

Use CMake package targets when stable and create project-local imported targets for plain C libraries whose package target names may differ.

```cmake
find_package(CLI11 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(SQLite3 REQUIRED)

if(LOCALVAULT_BUILD_GUI)
    find_package(Qt6 CONFIG REQUIRED COMPONENTS Core Widgets)
endif()

if(BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
endif()

find_path(BLAKE3_INCLUDE_DIR NAMES blake3.h REQUIRED)
find_library(BLAKE3_LIBRARY NAMES blake3 REQUIRED)

add_library(LocalVault_blake3 UNKNOWN IMPORTED)
set_target_properties(
    LocalVault_blake3
    PROPERTIES
        IMPORTED_LOCATION "${BLAKE3_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${BLAKE3_INCLUDE_DIR}"
)
add_library(LocalVault::blake3 ALIAS LocalVault_blake3)

find_path(ZSTD_INCLUDE_DIR NAMES zstd.h REQUIRED)
find_library(ZSTD_LIBRARY NAMES zstd libzstd REQUIRED)

add_library(LocalVault_zstd UNKNOWN IMPORTED)
set_target_properties(
    LocalVault_zstd
    PROPERTIES
        IMPORTED_LOCATION "${ZSTD_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${ZSTD_INCLUDE_DIR}"
)
add_library(LocalVault::zstd ALIAS LocalVault_zstd)
```

## 10.4 `cmake/CompilerWarnings.cmake`

```cmake
function(localvault_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        if(LOCALVAULT_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(
            ${target}
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Wconversion
                -Wsign-conversion
                -Wshadow
                -Wnon-virtual-dtor
                -Wold-style-cast
                -Woverloaded-virtual
        )
        if(LOCALVAULT_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
```

If a warning produces excessive false positives, remove it only after documenting the reason. Do not disable warnings for the entire project because one third-party header is noisy. Third-party includes should be exposed as `SYSTEM` includes where necessary.

## 10.5 `cmake/Sanitizers.cmake`

```cmake
function(localvault_enable_sanitizers target)
    if(MSVC)
        return()
    endif()

    if(LOCALVAULT_ENABLE_ASAN)
        target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address)
    endif()

    if(LOCALVAULT_ENABLE_UBSAN)
        target_compile_options(${target} PRIVATE -fsanitize=undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=undefined)
    endif()
endfunction()
```

## 10.6 `cmake/StaticAnalysis.cmake`

```cmake
function(localvault_enable_clang_tidy target)
    if(NOT LOCALVAULT_ENABLE_CLANG_TIDY)
        return()
    endif()

    find_program(CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
    set_target_properties(
        ${target}
        PROPERTIES
            CXX_CLANG_TIDY "${CLANG_TIDY_EXE}"
    )
endfunction()
```

## 10.7 `src/core/CMakeLists.txt`

```cmake
add_library(
    localvault_core
    database/database.cpp
    database/migrations.cpp
    database/statement.cpp
    database/transaction.cpp
    filesystem/file_scanner.cpp
    filesystem/ignore_rules.cpp
    filesystem/path_safety.cpp
    storage/chunker.cpp
    storage/blake3_hasher.cpp
    storage/zstd_codec.cpp
    storage/object_store.cpp
    repository.cpp
    snapshot_engine.cpp
    restore_engine.cpp
    diff_engine.cpp
    integrity_verifier.cpp
    garbage_collector.cpp
    query_service.cpp
    logging.cpp
    error.cpp
)

if(WIN32)
    target_sources(
        localvault_core
        PRIVATE
            filesystem/platform/win32_lock.cpp
            filesystem/platform/win32_metadata.cpp
    )
else()
    target_sources(
        localvault_core
        PRIVATE
            filesystem/platform/posix_lock.cpp
            filesystem/platform/posix_metadata.cpp
    )
endif()

add_library(LocalVault::core ALIAS localvault_core)

target_compile_features(localvault_core PUBLIC cxx_std_20)

target_include_directories(
    localvault_core
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>"
        "$<INSTALL_INTERFACE:include>"
)

target_link_libraries(
    localvault_core
    PRIVATE
        SQLite::SQLite3
        LocalVault::blake3
        LocalVault::zstd
)

localvault_set_warnings(localvault_core)
localvault_enable_sanitizers(localvault_core)
localvault_enable_clang_tidy(localvault_core)
```

## 10.8 `src/cli/CMakeLists.txt`

```cmake
add_executable(
    localvault
    main.cpp
    cli_app.cpp
    output.cpp
    signal_handler.cpp
    commands/init_command.cpp
    commands/snapshot_command.cpp
    commands/list_command.cpp
    commands/show_command.cpp
    commands/files_command.cpp
    commands/diff_command.cpp
    commands/restore_command.cpp
    commands/verify_command.cpp
    commands/stats_command.cpp
    commands/delete_command.cpp
    commands/gc_command.cpp
)

target_link_libraries(
    localvault
    PRIVATE
        LocalVault::core
        CLI11::CLI11
        nlohmann_json::nlohmann_json
)

localvault_set_warnings(localvault)
localvault_enable_sanitizers(localvault)
localvault_enable_clang_tidy(localvault)
```

## 10.9 `src/desktop/CMakeLists.txt`

```cmake
qt_standard_project_setup()

add_executable(
    localvault_desktop
    main.cpp
    main_window.cpp
    controllers/repository_controller.cpp
    controllers/operation_controller.cpp
    models/snapshot_table_model.cpp
    models/snapshot_tree_model.cpp
    pages/dashboard_page.cpp
    pages/snapshots_page.cpp
    pages/restore_page.cpp
    pages/verify_page.cpp
    pages/settings_page.cpp
    workers/core_operation_worker.cpp
)

target_link_libraries(
    localvault_desktop
    PRIVATE
        LocalVault::core
        Qt6::Core
        Qt6::Widgets
)

set_target_properties(
    localvault_desktop
    PROPERTIES
        WIN32_EXECUTABLE TRUE
        MACOSX_BUNDLE TRUE
)

localvault_set_warnings(localvault_desktop)
localvault_enable_sanitizers(localvault_desktop)
localvault_enable_clang_tidy(localvault_desktop)
```

## 10.10 `tests/CMakeLists.txt`

```cmake
add_executable(
    localvault_tests
    unit/chunker_test.cpp
    unit/hasher_test.cpp
    unit/codec_test.cpp
    unit/ignore_rules_test.cpp
    unit/path_safety_test.cpp
    unit/diff_test.cpp
    integration/repository_test.cpp
    integration/snapshot_restore_test.cpp
    integration/deduplication_test.cpp
    integration/corruption_test.cpp
    integration/cancellation_test.cpp
    integration/garbage_collection_test.cpp
    support/temporary_directory.cpp
    support/dataset_builder.cpp
    support/file_assertions.cpp
)

target_link_libraries(
    localvault_tests
    PRIVATE
        LocalVault::core
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(localvault_tests)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(
    NAME localvault_cli_e2e
    COMMAND Python3::Interpreter
        "${CMAKE_CURRENT_SOURCE_DIR}/cli/cli_e2e_test.py"
        --binary "$<TARGET_FILE:localvault>"
)

localvault_set_warnings(localvault_tests)
localvault_enable_sanitizers(localvault_tests)
localvault_enable_clang_tidy(localvault_tests)
```

## 10.11 `benchmarks/CMakeLists.txt`

```cmake
add_executable(
    localvault_benchmarks
    benchmark_main.cpp
)

target_link_libraries(
    localvault_benchmarks
    PRIVATE
        LocalVault::core
        nlohmann_json::nlohmann_json
)

localvault_set_warnings(localvault_benchmarks)
localvault_enable_sanitizers(localvault_benchmarks)
localvault_enable_clang_tidy(localvault_benchmarks)
```

## 10.12 `.gitignore`

```gitignore
/build/
/out/
/.vscode/
/.idea/
.DS_Store
*.user
*.swp
*.tmp
compile_commands.json
Testing/
```

## 10.13 `.clang-format`

```yaml
BasedOnStyle: LLVM
IndentWidth: 4
ContinuationIndentWidth: 4
ColumnLimit: 100
DerivePointerAlignment: false
PointerAlignment: Left
SortIncludes: CaseSensitive
AllowShortFunctionsOnASingleLine: Empty
```

## 10.14 `.clang-tidy`

```yaml
Checks: >
  -*,
  bugprone-*,
  clang-analyzer-*,
  concurrency-*,
  modernize-*,
  performance-*,
  portability-*,
  readability-*
WarningsAsErrors: ''
HeaderFilterRegex: '^(.*[/\\])?(include|src)[/\\].*'
FormatStyle: file
```

Start with `WarningsAsErrors` empty. Promote selected checks to errors only after the codebase is clean.

---

# 11. Architecture and dependency rules

## 11.1 Layered architecture

```text
┌─────────────────────────────────────────────────────────────┐
│ Interfaces                                                  │
│                                                             │
│ localvault CLI                 localvault_desktop (Qt)       │
└──────────────────────┬───────────────────────┬──────────────┘
                       │                       │
                       └──────────┬────────────┘
                                  ▼
┌─────────────────────────────────────────────────────────────┐
│ Application services                                        │
│                                                             │
│ SnapshotEngine     RestoreEngine       DiffEngine            │
│ IntegrityVerifier GarbageCollector     QueryService          │
└───────────────────────────────┬─────────────────────────────┘
                                ▼
┌─────────────────────────────────────────────────────────────┐
│ Domain and infrastructure components                        │
│                                                             │
│ Repository       FileScanner       IgnoreRules              │
│ Chunker          Blake3Hasher      ZstdCodec                 │
│ ObjectStore      MetadataStore     RepositoryLock            │
│ PathSafety       POSIX metadata    Progress reporting        │
└─────────────────────┬─────────────────────────┬─────────────┘
                      ▼                         ▼
               SQLite database          Repository filesystem
```

## 11.2 Dependency rules

1. `localvault_core` must not include Qt or CLI11 headers.
2. CLI code may depend on `localvault_core` and CLI11.
3. GUI code may depend on `localvault_core` and Qt.
4. Database classes may use SQLite directly; higher-level services must use database wrappers.
5. Storage wrappers may use BLAKE3 and zstd directly; higher-level services must not.
6. Platform-specific file locking and metadata code stays under `src/core/filesystem/platform`, as POSIX and Win32 implementations of one shared header. No `#ifdef` platform blocks outside that directory.
7. UI code must not execute the CLI as a subprocess.
8. Qt widgets must not contain snapshot, restore, verification, or garbage-collection algorithms.
9. Public headers under `include/localvault` expose stable application types, not SQLite handles or zstd contexts.
10. Every mutating repository operation requires an exclusive `RepositoryLock`.

## 11.3 Core components

### `Repository`

- Validates repository paths.
- Opens and configures SQLite.
- Checks repository format compatibility.
- Acquires the writer lock for mutating operations.
- Runs recovery of stale incomplete operations.
- Exposes repository paths and metadata services.

### `FileScanner`

- Recursively enumerates a source root.
- Uses `symlink_status` so it does not follow symlinks accidentally.
- Never traverses Windows directory junctions or volume mount points (Section 25.4).
- Optionally stays within one filesystem/volume (`one_file_system`).
- Applies ignore rules.
- Produces normalized relative paths.
- Emits regular-file, directory, and symbolic-link records.
- Skips unsupported special files and cloud placeholders with warnings.

### `Chunker`

- Reads regular files as a stream.
- Produces chunks up to 4 MiB.
- Handles empty files without producing a content chunk.
- Supplies each chunk to the full-file hasher and chunk processor.

### `Blake3Hasher`

- Wraps the BLAKE3 C API.
- Supports incremental update.
- Returns a fixed 32-byte digest.
- Converts digests to lowercase 64-character hexadecimal identifiers.

### `ZstdCodec`

- Compresses one raw chunk.
- Decompresses one stored object.
- Enforces expected raw-size limits during decompression.
- Converts zstd error codes into `LocalVaultError`.

### `ObjectStore`

- Maps a hash to an object path.
- Writes new objects through unique temporary files.
- Publishes objects atomically.
- Handles duplicate hashes safely.
- Reads and validates object data.
- Deletes only unreferenced objects selected by garbage collection.

### `MetadataStore`

This is implemented by the database wrapper and query methods. It:

- Runs migrations.
- Creates and updates snapshots.
- Inserts entries and chunk relationships.
- Lists snapshots and entries.
- Computes statistics.
- Finds unreferenced chunks.
- Deletes snapshot metadata transactionally.

### `SnapshotEngine`

Coordinates scanning, workers, object storage, metadata insertion, progress, retry policy, and snapshot state.

### `RestoreEngine`

Validates requested paths, retrieves ordered chunks, reconstructs files, verifies hashes, applies overwrite policy, and restores metadata.

### `DiffEngine`

Compares two snapshots by normalized relative path and entry metadata.

### `IntegrityVerifier`

Supports quick and full verification modes.

### `GarbageCollector`

Deletes stale incomplete snapshot metadata, stale temporary files, and unreferenced objects under an exclusive repository lock.

### `QueryService`

Provides read-only queries used by both interfaces:

- Snapshot list.
- Snapshot details.
- Directory children.
- Path search.
- Repository statistics.
- Verification history if stored.

---

# 12. Core public API

The exact implementation may evolve, but preserve the separation below.

## 12.1 Common types

`include/localvault/types.hpp`:

```cpp
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace localvault {

using SnapshotId = std::int64_t;
using ByteCount = std::uint64_t;
using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;

enum class EntryType {
    regular_file,
    directory,
    symbolic_link
};

enum class SnapshotStatus {
    pending,
    complete,
    failed,
    cancelled,
    deleting
};

enum class OverwritePolicy {
    never,
    prompt,
    always
};

enum class VerifyMode {
    quick,
    full
};

struct SnapshotSummary {
    SnapshotId id{};
    TimePoint created_at{};
    std::filesystem::path source_root;
    std::string message;
    SnapshotStatus status{SnapshotStatus::pending};
    std::uint64_t file_count{};
    std::uint64_t directory_count{};
    ByteCount logical_size{};
    ByteCount new_stored_size{};
    std::chrono::milliseconds duration{};
};

struct EntryInfo {
    std::int64_t id{};
    SnapshotId snapshot_id{};
    std::filesystem::path relative_path;
    EntryType type{EntryType::regular_file};
    ByteCount logical_size{};
    std::int64_t modified_time_ns{};
    std::uint32_t posix_mode{};
    std::optional<std::uint32_t> windows_attributes;
    std::optional<std::string> file_hash_hex;
    std::optional<std::filesystem::path> symlink_target;
};

struct RepositoryStats {
    std::uint64_t complete_snapshot_count{};
    std::uint64_t unique_chunk_count{};
    ByteCount logical_bytes{};
    ByteCount unique_raw_bytes{};
    ByteCount stored_bytes{};
    double deduplication_savings{};
    double compression_savings{};
    double total_savings{};
};

struct SkippedEntry {
    std::filesystem::path relative_path;
    std::string reason;
};

}  // namespace localvault
```

## 12.2 Error model

`include/localvault/error.hpp`:

```cpp
#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

namespace localvault {

enum class ErrorCode {
    invalid_argument,
    repository_not_found,
    invalid_repository,
    unsupported_repository_version,
    repository_busy,
    filesystem_error,
    database_error,
    compression_error,
    hashing_error,
    object_missing,
    object_corrupt,
    unsafe_restore_path,
    destination_exists,
    source_changed,
    cancelled,
    partial_success,
    internal_error
};

class LocalVaultError final : public std::runtime_error {
public:
    LocalVaultError(
        ErrorCode code,
        std::string message,
        std::filesystem::path path = {})
        : std::runtime_error(std::move(message)),
          code_(code),
          path_(std::move(path)) {}

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    ErrorCode code_;
    std::filesystem::path path_;
};

}  // namespace localvault
```

Use exceptions for fatal operation errors crossing service boundaries. Use value types for expected per-file outcomes such as skipped or unstable files. Never throw through a C callback.

`ErrorCode::partial_success` is reserved for interface-boundary exit-code mapping. Core operations never throw it; they report partial success through warning lists in their result types (Section 31.4).

## 12.3 Progress API

`include/localvault/progress.hpp`:

```cpp
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace localvault {

enum class OperationPhase {
    preparing,
    scanning,
    reading,
    hashing,
    compressing,
    writing_objects,
    writing_metadata,
    restoring,
    verifying,
    garbage_collecting,
    finalizing,
    complete
};

struct ProgressEvent {
    OperationPhase phase{OperationPhase::preparing};
    std::filesystem::path current_path;
    std::uint64_t discovered_entries{};
    std::uint64_t processed_entries{};
    std::uint64_t processed_bytes{};
    std::optional<std::uint64_t> total_entries;
    std::optional<std::uint64_t> total_bytes;
    std::uint64_t new_chunks{};
    std::uint64_t reused_chunks{};
    std::string message;
};

using ProgressCallback = std::function<void(const ProgressEvent&)>;

}  // namespace localvault
```

The callback may be invoked from worker threads. Interface adapters must marshal events to their own threads.

`total_entries` and `total_bytes` are empty until the scan phase completes, because the pipeline overlaps scanning with processing. Interfaces must show indeterminate progress until totals arrive, then switch to a percentage.

## 12.4 Repository API

`include/localvault/repository.hpp`:

```cpp
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

namespace localvault {

class FailureInjector;

struct RepositoryCreateOptions {
    std::uint64_t chunk_size_bytes{4ULL * 1024ULL * 1024ULL};
    int zstd_level{3};
    bool allow_risky_filesystem{false};
};

enum class OpenMode {
    read_only,
    read_write
};

class Repository final {
public:
    static void create(
        const std::filesystem::path& root,
        const RepositoryCreateOptions& options = {});

    static Repository open(
        const std::filesystem::path& root,
        OpenMode mode = OpenMode::read_write);

    Repository(Repository&&) noexcept;
    Repository& operator=(Repository&&) noexcept;
    ~Repository();

    Repository(const Repository&) = delete;
    Repository& operator=(const Repository&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const noexcept;
    [[nodiscard]] std::uint32_t format_version() const noexcept;

    // Test-only seam (Section 32.5). Production callers never call this;
    // the default injector is a no-op.
    void set_failure_injector(std::shared_ptr<FailureInjector> injector);

private:
    class Impl;
    explicit Repository(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend class SnapshotEngine;
    friend class RestoreEngine;
    friend class DiffEngine;
    friend class IntegrityVerifier;
    friend class GarbageCollector;
    friend class QueryService;
};

}  // namespace localvault
```

Use a PImpl to prevent SQLite and platform details from leaking into public headers.

Open-mode semantics:

- `read_write` (default): mutating engines may run. Each mutating or object-reading operation acquires the exclusive repository lock per operation (Section 23.6). Stale-operation recovery runs at the start of the first mutating operation, under the lock.
- `read_only`: never creates files, never acquires the writer lock, never runs recovery, and works on read-only media. Only `QueryService` and `DiffEngine` accept a read-only repository; other engines throw `invalid_argument`.

## 12.5 Snapshot API

`include/localvault/snapshot_engine.hpp`:

```cpp
#pragma once

#include "localvault/progress.hpp"
#include "localvault/types.hpp"

#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

namespace localvault {

class Repository;

struct SnapshotOptions {
    std::string message;
    std::size_t worker_count{};
    bool retry_unstable_files{true};
    bool force_rehash{false};
    bool include_hidden{true};
    bool one_file_system{false};
};

struct SnapshotResult {
    SnapshotId snapshot_id{};
    std::uint64_t file_count{};
    std::uint64_t directory_count{};
    ByteCount logical_bytes{};
    ByteCount new_stored_bytes{};
    std::uint64_t new_chunks{};
    std::uint64_t reused_chunks{};
    std::vector<SkippedEntry> skipped_entries;
};

class SnapshotEngine final {
public:
    explicit SnapshotEngine(Repository& repository);

    SnapshotResult create_snapshot(
        const std::filesystem::path& source_root,
        const SnapshotOptions& options,
        std::stop_token stop_token = {},
        ProgressCallback progress = {});

private:
    Repository& repository_;
};

}  // namespace localvault
```

If `worker_count == 0`, use the default worker count defined in Section 24.4 — that section is the single normative definition. Do not allow an unbounded worker count from user input.

## 12.6 Restore API

`include/localvault/restore_engine.hpp`:

```cpp
#pragma once

#include "localvault/progress.hpp"
#include "localvault/types.hpp"

#include <filesystem>
#include <functional>
#include <stop_token>
#include <vector>

namespace localvault {

class Repository;

enum class ConflictDecision {
    skip,
    replace,
    cancel
};

using ConflictResolver = std::function<ConflictDecision(
    const std::filesystem::path& destination,
    EntryType incoming_type)>;

struct RestoreRequest {
    SnapshotId snapshot_id{};
    std::vector<std::filesystem::path> relative_paths;
    std::filesystem::path destination_root;
    OverwritePolicy overwrite_policy{OverwritePolicy::never};
    ConflictResolver conflict_resolver;
    bool verify_final_file_hash{true};
};

struct RestoreResult {
    std::uint64_t restored_files{};
    std::uint64_t restored_directories{};
    std::uint64_t restored_symlinks{};
    ByteCount restored_bytes{};
    std::vector<SkippedEntry> skipped_entries;
};

class RestoreEngine final {
public:
    explicit RestoreEngine(Repository& repository);

    RestoreResult restore(
        const RestoreRequest& request,
        std::stop_token stop_token = {},
        ProgressCallback progress = {});

private:
    Repository& repository_;
};

}  // namespace localvault
```

The core library must not implement interactive prompts. `OverwritePolicy::prompt` requires a non-empty `conflict_resolver`; the core invokes it synchronously per conflict, and interface adapters marshal the question to their own threads (usage rules in Section 42.11).

## 12.7 Verification API

```cpp
struct VerificationIssue {
    enum class Kind {
        missing_object,
        corrupt_object,
        invalid_chunk_size,
        invalid_entry_relationship,
        invalid_snapshot_state,
        stale_temporary_file
    };

    Kind kind{};
    std::filesystem::path path;
    std::string detail;
};

struct VerificationResult {
    std::uint64_t checked_snapshots{};
    std::uint64_t checked_entries{};
    std::uint64_t checked_objects{};
    ByteCount checked_stored_bytes{};
    std::vector<VerificationIssue> issues;

    [[nodiscard]] bool ok() const noexcept { return issues.empty(); }
};

class IntegrityVerifier final {
public:
    explicit IntegrityVerifier(Repository& repository);

    VerificationResult verify(
        VerifyMode mode,
        std::stop_token stop_token = {},
        ProgressCallback progress = {});
};
```

## 12.8 Query API

Use page-based queries so the GUI never loads an entire large snapshot tree at once:

```cpp
struct PageRequest {
    std::uint64_t offset{};
    std::uint32_t limit{200};
};

template <typename T>
struct Page {
    std::vector<T> items;
    std::uint64_t total_count{};
};

class QueryService final {
public:
    explicit QueryService(Repository& repository);

    Page<SnapshotSummary> list_snapshots(PageRequest page);
    SnapshotSummary get_snapshot(SnapshotId id);
    Page<EntryInfo> list_children(
        SnapshotId id,
        const std::filesystem::path& parent,
        PageRequest page);
    Page<EntryInfo> search_paths(
        SnapshotId id,
        std::string_view query,
        PageRequest page);
    RepositoryStats repository_stats();
};
```

---

# 13. Repository-on-disk format

A repository is a normal directory:

```text
MyVault/
├── repository.db
├── objects/
│   ├── 00/
│   ├── 01/
│   └── ...
├── temporary/
│   ├── objects/
│   └── restores/
├── logs/
└── repository.lock
```

## 13.1 Required paths

| Path | Purpose |
|---|---|
| `repository.db` | SQLite metadata |
| `objects/` | Immutable compressed chunks |
| `temporary/objects/` | Incomplete object writes |
| `temporary/restores/` | Optional internal restore staging |
| `logs/` | Diagnostic logs |
| `repository.lock` | Cross-process writer lock |

## 13.2 Object identifier

For a raw chunk:

```text
object_id = lowercase_hex(BLAKE3(raw_chunk))
```

BLAKE3 produces 32 bytes, rendered as 64 lowercase hexadecimal characters.

## 13.3 Object path

Use the first two hash characters as a shard:

```text
objects/<first-two-hex>/<full-hash>.zst
```

Example:

```text
objects/7a/7a2f...e91c.zst
```

The object path is derived from the hash. Store the relative path in SQLite for diagnostics, but verify that it matches the derived path when opening old metadata.

## 13.4 Object payload

The first release stores one valid zstd frame containing exactly one raw chunk. The database records:

- Expected raw size.
- Compressed file size.
- Hash.
- Relative object path.

No custom binary header is required in the first release. Repository format versioning allows a header to be introduced later.

## 13.5 Invariants

- Objects are immutable.
- A hash identifies uncompressed bytes, not compressed bytes.
- Two identical raw chunks share one object.
- An object is published before metadata references it.
- A complete snapshot never references a missing object under normal operation.
- Extra unreferenced objects are harmless and removable by garbage collection.
- Temporary files are never treated as valid objects.

---

# 14. SQLite schema

Use UTF-8 text, integer byte counts, and integer UTC epoch nanoseconds.

Enable these pragmas on every connection as appropriate:

```sql
PRAGMA foreign_keys = ON;
PRAGMA journal_mode = WAL;
PRAGMA synchronous = FULL;
PRAGMA busy_timeout = 5000;
```

`journal_mode` is persistent, but set and verify it when initializing the repository. Use one write connection and separate read connections only after correctness is established.

## 14.1 Migration table

```sql
CREATE TABLE IF NOT EXISTS schema_migrations (
    version      INTEGER PRIMARY KEY,
    applied_at_ns INTEGER NOT NULL,
    description  TEXT NOT NULL
);
```

## 14.2 Repository information

```sql
CREATE TABLE repository_info (
    singleton_id              INTEGER PRIMARY KEY CHECK (singleton_id = 1),
    repository_uuid           TEXT NOT NULL UNIQUE,
    format_version            INTEGER NOT NULL,
    created_at_ns             INTEGER NOT NULL,
    application_version       TEXT NOT NULL,
    chunk_size_bytes          INTEGER NOT NULL CHECK (chunk_size_bytes > 0),
    zstd_level                INTEGER NOT NULL,
    hash_algorithm            TEXT NOT NULL CHECK (hash_algorithm = 'blake3'),
    path_encoding             TEXT NOT NULL DEFAULT 'utf-8'
);
```

Exactly one row exists with `singleton_id = 1`.

## 14.3 Snapshots

```sql
CREATE TABLE snapshots (
    id                 INTEGER PRIMARY KEY AUTOINCREMENT,
    created_at_ns      INTEGER NOT NULL,
    completed_at_ns    INTEGER,
    source_root         TEXT NOT NULL,
    message             TEXT NOT NULL DEFAULT '',
    status              TEXT NOT NULL
                        CHECK (status IN ('pending', 'complete', 'failed', 'cancelled', 'deleting')),
    file_count          INTEGER NOT NULL DEFAULT 0,
    directory_count     INTEGER NOT NULL DEFAULT 0,
    symlink_count       INTEGER NOT NULL DEFAULT 0,
    logical_size        INTEGER NOT NULL DEFAULT 0,
    new_stored_size     INTEGER NOT NULL DEFAULT 0,
    new_chunk_count     INTEGER NOT NULL DEFAULT 0,
    reused_chunk_count  INTEGER NOT NULL DEFAULT 0,
    duration_ms         INTEGER NOT NULL DEFAULT 0,
    failure_message     TEXT
);

CREATE INDEX idx_snapshots_status_created
    ON snapshots(status, created_at_ns DESC);
```

Only `status = 'complete'` is visible to normal restore and browse operations.

## 14.4 Entries

Use one table for files, directories, and symbolic links:

```sql
CREATE TABLE entries (
    id                  INTEGER PRIMARY KEY AUTOINCREMENT,
    snapshot_id         INTEGER NOT NULL
                        REFERENCES snapshots(id) ON DELETE CASCADE,
    relative_path       TEXT NOT NULL,
    parent_path         TEXT NOT NULL,
    name                TEXT NOT NULL,
    entry_type          TEXT NOT NULL
                        CHECK (entry_type IN ('file', 'directory', 'symlink')),
    logical_size        INTEGER NOT NULL DEFAULT 0,
    modified_time_ns    INTEGER NOT NULL DEFAULT 0,
    change_time_ns      INTEGER,
    posix_mode          INTEGER NOT NULL DEFAULT 0,
    windows_attributes  INTEGER,
    file_hash           TEXT,
    symlink_target      TEXT,
    source_device       INTEGER,
    source_inode        INTEGER,
    UNIQUE(snapshot_id, relative_path)
);

CREATE INDEX idx_entries_snapshot_parent_name
    ON entries(snapshot_id, parent_path, name);

CREATE INDEX idx_entries_snapshot_path
    ON entries(snapshot_id, relative_path);
```

`source_device` and `source_inode` hold the POSIX device/inode pair on Linux/macOS and the volume serial number/file ID on Windows. Together with `change_time_ns` (POSIX ctime; NTFS change time) they feed the incremental reuse rule (Section 17.1) and can later support hard-link preservation, but the first release does not recreate hard links.

`posix_mode` is `0` for entries captured on Windows. `windows_attributes` stores the captured `FILE_ATTRIBUTE_*` bits on Windows and is `NULL` for entries captured on POSIX platforms. See the platform policy matrix (Section 25.10).

The source root itself is represented with `relative_path = ''`, `parent_path = ''`, and `name = ''`. Child listing queries must exclude the root row when listing the root level:

```sql
SELECT ... FROM entries
WHERE snapshot_id = ? AND parent_path = '' AND relative_path <> '';
```

Test this rule explicitly; the root is the only row whose `parent_path` equals its own `relative_path`.

## 14.5 Chunks

```sql
CREATE TABLE chunks (
    hash                TEXT PRIMARY KEY,
    raw_size            INTEGER NOT NULL CHECK (raw_size > 0),
    compressed_size     INTEGER NOT NULL CHECK (compressed_size > 0),
    object_path         TEXT NOT NULL UNIQUE,
    created_at_ns       INTEGER NOT NULL
);
```

Do not store an authoritative mutable reference count. Derive references from `entry_chunks`; this avoids reference-count drift after crashes or bugs.

## 14.6 Entry-to-chunk mapping

```sql
CREATE TABLE entry_chunks (
    entry_id            INTEGER NOT NULL
                        REFERENCES entries(id) ON DELETE CASCADE,
    sequence_number     INTEGER NOT NULL CHECK (sequence_number >= 0),
    chunk_hash          TEXT NOT NULL
                        REFERENCES chunks(hash),
    raw_offset          INTEGER NOT NULL CHECK (raw_offset >= 0),
    raw_length          INTEGER NOT NULL CHECK (raw_length > 0),
    PRIMARY KEY(entry_id, sequence_number)
);

CREATE INDEX idx_entry_chunks_hash
    ON entry_chunks(chunk_hash);
```

For an empty file, insert an `entries` row with `logical_size = 0` and no `entry_chunks` rows.

## 14.7 Skipped entries

```sql
CREATE TABLE snapshot_warnings (
    id                  INTEGER PRIMARY KEY AUTOINCREMENT,
    snapshot_id         INTEGER NOT NULL
                        REFERENCES snapshots(id) ON DELETE CASCADE,
    relative_path       TEXT NOT NULL,
    warning_code        TEXT NOT NULL,
    message             TEXT NOT NULL
);

CREATE INDEX idx_snapshot_warnings_snapshot
    ON snapshot_warnings(snapshot_id);
```

## 14.8 Optional operation history

```sql
CREATE TABLE operation_history (
    id                  INTEGER PRIMARY KEY AUTOINCREMENT,
    operation_type      TEXT NOT NULL,
    started_at_ns       INTEGER NOT NULL,
    completed_at_ns     INTEGER,
    status              TEXT NOT NULL,
    summary_json        TEXT,
    failure_message     TEXT
);
```

This table is optional for the first CLI milestone, but useful for the GUI.

## 14.9 Repository settings

Mutable repository-scoped settings (Section 30.1) are stored as key/value rows and created by the initial migration:

```sql
CREATE TABLE repository_settings (
    key            TEXT PRIMARY KEY,
    value          TEXT NOT NULL,
    updated_at_ns  INTEGER NOT NULL
);
```

Defined keys in the first release: `default_source_root`, `default_worker_count`, `default_ignore_file`, `default_overwrite_policy`, `log_level`. Unknown keys are preserved but ignored. Immutable configuration (format version, hash algorithm, chunk size) stays in `repository_info` and is never stored here.

## 14.10 Schema migrations

Store SQL migrations as numbered embedded resources or C++ string constants:

```text
001_initial_schema.sql
002_add_operation_history.sql
```

Migration rules:

1. Begin an exclusive transaction.
2. Read the current maximum migration version.
3. Apply missing migrations in order.
4. Insert one `schema_migrations` row per migration.
5. Update `repository_info.format_version` only when repository compatibility changes.
6. Roll back everything if any migration fails.
7. Back up `repository.db` before destructive migrations in future versions.
8. Never modify a migration after it has been released; add a new migration.

---

# 15. Repository initialization

`Repository::create` performs these steps:

1. Convert the requested root to an absolute normalized path.
2. Reject an existing non-empty directory unless the caller explicitly approved it.
3. Classify the destination filesystem:
   - Network filesystems (NFS, SMB/CIFS, and other remote mounts; `DRIVE_REMOTE` on Windows) are rejected unless `allow_risky_filesystem` is set, because SQLite WAL and file locking are unreliable there.
   - FAT/exFAT produce a prominent warning: no permission bits and weaker rename/flush durability.
   - Detection uses `statfs`/`f_fstypename` on macOS, `statfs` `f_type` on Linux, and `GetDriveTypeW`/`GetVolumeInformationW` on Windows.
4. Create the repository root with restrictive access: mode `0700` on POSIX platforms. On Windows, rely on inherited user-profile ACLs; repositories created outside the user profile are not additionally restricted in the first release (documented limitation).
5. Create `objects`, `temporary/objects`, `temporary/restores`, and `logs`.
6. Create `repository.lock`.
7. Acquire the exclusive writer lock.
8. Create `repository.db`.
9. Enable SQLite foreign keys, WAL, and full synchronous mode.
10. Run migration `001_initial_schema`.
11. Insert the `repository_info` row:
    - New random UUID (version 4; generate from `getentropy`/`arc4random_buf` on POSIX and `BCryptGenRandom` on Windows — the standard library has no UUID facility).
    - Repository format version `1`.
    - Current application version.
    - Chunk size `4 * 1024 * 1024`.
    - zstd level `3`.
    - Hash algorithm `blake3`.
12. Commit the database transaction.
13. Flush the database and containing directory when practical.
14. Release the writer lock.
15. Re-open the repository using the normal validation path as a final self-check.

Failure handling:

- If creation fails before the repository becomes valid, remove only paths created by this operation.
- Never recursively delete a pre-existing user directory.
- Return a detailed error with the failing path.

---

# 16. Snapshot algorithm

## 16.1 High-level sequence

```text
Validate source and repository
        │
Acquire exclusive repository lock
        │
Recover stale incomplete operations
        │
Insert pending snapshot row
        │
Scan source and enqueue jobs
        │
Workers read/hash/compress/store chunks
        │
Single metadata writer inserts entries and chunk mappings
        │
Validate counters and referenced objects
        │
Mark snapshot complete
        │
Release repository lock
```

## 16.2 Detailed steps

1. Validate that the source exists and is a directory.
2. Reject a source located inside the repository.
3. Reject a repository located inside the source unless the repository path is automatically ignored. Prefer rejecting this configuration to avoid recursive backup.
4. Acquire `RepositoryLock` exclusively.
5. Run stale-operation recovery.
6. Insert a `pending` snapshot row and commit it so a crash is detectable.
7. Load repository configuration and ignore rules.
8. Start:
   - One scanner producer.
   - A bounded file-job queue.
   - A fixed worker pool.
   - One result/metadata writer.
9. Insert directory and symlink metadata.
10. For each regular file:
    - Capture pre-read metadata: size, modification time, change time, mode or attributes, and file identifier (device/inode on POSIX; volume serial/file ID on Windows).
    - Decide whether an earlier entry can be reused.
    - Otherwise stream the file in 4 MiB chunks.
    - Feed every byte to the full-file BLAKE3 hasher.
    - For each chunk:
      - Compute chunk hash.
      - Check the in-memory recent-object cache.
      - Check the database/object store when not cached.
      - Compress and atomically store only if absent.
      - Produce an ordered `ChunkReference`.
    - Capture post-read size and modification time.
    - If changed, discard the produced entry result and retry once when enabled.
    - If still unstable, record a warning and skip it.
11. The metadata writer:
    - Inserts or verifies each `chunks` row.
    - Inserts the `entries` row.
    - Inserts all `entry_chunks` rows.
    - Commits in bounded batches, for example every 500 entries or 64 MiB of metadata input.
12. If cancellation is requested:
    - Stop accepting new file jobs.
    - Allow in-flight object writes to finish safely.
    - Drain or discard uncommitted results.
    - Mark the snapshot `cancelled`.
    - Delete its entries and warnings or retain them only for diagnostics. Recommended: delete them transactionally.
    - Leave orphan objects for later garbage collection.
13. If a fatal error occurs:
    - Mark the snapshot `failed` with a concise failure message.
    - Remove its entry metadata transactionally.
    - Leave completed immutable objects unreferenced.
14. On success:
    - Verify all entry chunk references exist.
    - Calculate final counters.
    - Update the snapshot row to `complete` inside one transaction.
15. Flush final database state.
16. Emit a final progress event.
17. Release the repository lock.

## 16.3 Batch metadata transaction

Do not keep one enormous transaction open for a multi-hour snapshot. Use batches while the snapshot remains `pending`.

Visibility rule:

```sql
SELECT ... FROM snapshots WHERE status = 'complete';
```

Recovery can delete all entries belonging to stale non-complete snapshots.

## 16.4 Stable file check

For each regular file:

```text
before = {size, mtime_ns, ctime_ns, file_id}
read and process file
after  = {size, mtime_ns, ctime_ns, file_id}
```

`file_id` is device/inode on POSIX and volume serial/file ID on Windows; `ctime_ns` is POSIX ctime or the NTFS change time.

Treat the file as unstable when:

- Size changed.
- Modification time changed.
- Change time changed.
- File identifier changed.
- The read ended earlier than expected.
- The file disappeared.

Retry once by default. Do not silently store a mixture of versions.

## 16.5 Snapshot warnings

Warnings are non-fatal per-entry outcomes:

- Permission denied.
- File locked by another process (Windows sharing violation).
- Cloud placeholder file skipped without hydration.
- Directory junction recorded as a link; volume mount point skipped.
- File disappeared.
- File changed repeatedly.
- Unsupported file type.
- Path not convertible to valid UTF-8 (including unpaired UTF-16 surrogates on Windows).
- Symlink target could not be read.

The CLI and GUI must show the count and allow viewing details. A snapshot with warnings may still be `complete`, but the operation result must be marked partial.

---

# 17. Incremental snapshot optimization

Correctness must work without this optimization. Add it after basic snapshot/restore tests pass.

## 17.1 Fast reuse rule

Find the newest complete snapshot for the same source root. For an entry at the same relative path, reuse its existing chunk list when all of these match:

- Entry type is regular file.
- Logical size.
- Modification time at nanosecond precision when available.
- Change time (POSIX ctime; NTFS change time) when available — this catches mtime-preserving modifications such as `touch -r` or checkout tools.
- File identifier (device/inode or volume serial/file ID) where meaningful.
- Platform metadata (POSIX mode or Windows attributes) if metadata-only changes should be detected.

When reused:

- Insert a new `entries` row for the new snapshot.
- Copy the old `entry_chunks` rows.
- Reuse the previous full-file hash.
- Do not read or hash file contents.

## 17.2 Safety modes

- Default fast mode: metadata-based reuse.
- `--force-rehash`: read and hash every regular file.
- Future optional paranoid mode: reuse only after a secondary lightweight fingerprint.

Document that metadata-based reuse assumes the filesystem maintains modification and change times correctly, and that the false-negative window is exactly: a file modified without changing size, mtime, ctime, or file identifier.

## 17.3 SQL copy pattern

After inserting the new entry:

```sql
INSERT INTO entry_chunks (
    entry_id,
    sequence_number,
    chunk_hash,
    raw_offset,
    raw_length
)
SELECT
    :new_entry_id,
    sequence_number,
    chunk_hash,
    raw_offset,
    raw_length
FROM entry_chunks
WHERE entry_id = :previous_entry_id
ORDER BY sequence_number;
```

---

# 18. Chunking, hashing, compression, and deduplication

## 18.1 Fixed-size chunking

Default raw chunk size:

```text
4 MiB = 4 * 1024 * 1024 bytes
```

Rules:

- Read up to 4 MiB at a time.
- The last chunk may be shorter.
- Empty files have zero chunks.
- Chunk size is stored in repository metadata and cannot be changed for an existing repository without a format migration.
- Use binary file I/O.

## 18.2 Hashing

Hash raw, uncompressed bytes:

```cpp
chunk_hash = BLAKE3(raw_chunk)
```

Hash the full file concurrently as the stream is read:

```cpp
full_file_hasher.update(raw_chunk);
```

Do not calculate a file hash by concatenating textual chunk hashes; hash the actual file bytes.

## 18.3 Compression

Use zstd level `3` by default.

Per chunk:

1. Calculate `ZSTD_compressBound(raw_size)`.
2. Allocate a destination buffer no larger than that bound.
3. Compress.
4. Check `ZSTD_isError`.
5. Resize the compressed buffer to the returned byte count.
6. Write the compressed bytes to a temporary file.
7. Record raw and compressed sizes.

Decompression:

1. Retrieve expected raw size from SQLite.
2. Reject sizes greater than configured safety limits.
3. Allocate exactly the expected chunk size.
4. Decompress.
5. Require the returned size to equal expected raw size.
6. Recompute BLAKE3 and compare with the object identifier.

## 18.4 Deduplication lookup

Use a two-level lookup:

1. A thread-safe in-memory cache of known hashes for the current process.
2. SQLite/object existence check on cache miss.

The database is authoritative for known objects, but verification also checks the file exists.

## 18.5 Duplicate-write race

Two worker threads may discover the same new hash simultaneously. Prevent duplicate publication with a striped mutex table:

```cpp
std::array<std::mutex, 256> object_mutexes;
auto& mutex = object_mutexes[first_hash_byte];
```

Inside the selected mutex:

1. Recheck whether the object already exists.
2. If absent, write and publish it.
3. Insert or verify the database `chunks` row through the metadata writer.

The repository-level writer lock prevents a separate process from writing concurrently. The striped mutex handles worker threads inside one process.

## 18.6 Object write algorithm

```text
derive final object path
lock stripe for hash
recheck final object and database record
create unique temp file under temporary/objects
write compressed bytes
flush userspace stream
fsync temp file
atomically rename temp file to final object path
fsync final object's parent directory
unlock stripe
```

The temporary directory must be on the same filesystem as `objects/` so rename remains atomic. On POSIX platforms publication uses `rename(2)`; on Windows it uses `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING`, which is atomic for same-volume moves on NTFS. Windows cannot fsync a directory; see Section 23.2 for the per-platform durability statement.

If the final object appears before publication because another worker stored it, delete the temporary file and reuse the existing object.

## 18.7 Hash collision policy

A BLAKE3 collision is not expected, but code must not blindly accept conflicting metadata.

When a hash already exists:

- Require the stored `raw_size` to match.
- During full verification, require decompressed bytes to hash to the same digest.
- If an existing row has a conflicting raw size, report repository corruption and stop the operation.

## 18.8 Storage-savings formulas

For a selected snapshot or repository scope:

```text
logical_bytes     = sum of logical file sizes
unique_raw_bytes  = sum raw sizes of distinct referenced chunks
stored_bytes      = sum compressed sizes of distinct referenced chunks

deduplication_savings = 1 - unique_raw_bytes / logical_bytes
compression_savings   = 1 - stored_bytes / unique_raw_bytes
total_savings         = 1 - stored_bytes / logical_bytes
```

When a denominator is zero, return `0.0`.

---

# 19. Restore algorithm

## 19.1 Request validation

Before writing anything:

1. Acquire the exclusive repository lock for the duration of the restore (the v1 locking rule, Section 23.6): restores read object files and must not race garbage collection.
2. Require the requested snapshot to exist and have status `complete`.
3. Normalize every requested relative path.
4. Reject:
   - Absolute paths.
   - Paths containing a `..` component after lexical normalization.
   - Empty selections unless the request explicitly means the complete snapshot.
   - Paths not present in the snapshot.
5. Resolve the destination root to an absolute path.
6. Reject a destination root that equals the repository root, lies inside the repository, or contains the repository — restoring into `objects/` or over `repository.db` must be impossible.
7. Apply overwrite policy.
8. Calculate a restore plan before writing. On case-insensitive or normalization-insensitive destinations, detect entry-name collisions in the plan and mark losers as skipped (Section 25.12).

## 19.2 Restore order

Restore in this order:

1. Create directories from shallowest to deepest.
2. Restore regular files.
3. Recreate symbolic links.
4. Apply directory timestamps and modes from deepest to shallowest.

Applying directory metadata last prevents child creation from changing final directory timestamps.

## 19.3 Regular-file reconstruction

For each regular file:

1. Validate the output path with `PathSafety`.
2. Create parent directories.
3. Create a unique temporary file in the same destination directory:
   ```text
   .<filename>.localvault-<random>.tmp
   ```
4. Initialize a full-file BLAKE3 hasher.
5. Query ordered `entry_chunks`.
6. For each chunk:
   - Check cancellation.
   - Open the object file.
   - Read compressed bytes with an upper size bound.
   - Decompress to the expected raw size.
   - Hash raw bytes and compare with `chunk_hash`.
   - Append raw bytes to the temporary output.
   - Feed raw bytes to the full-file hasher.
7. Flush and fsync the temporary output.
8. Require written bytes to equal the entry's logical size.
9. Compare the final file hash with `entries.file_hash`.
10. Apply file mode and modification time to the temporary file where possible.
11. Atomically publish it according to overwrite policy. For `never`, use an atomic no-replace rename — `renameat2(..., RENAME_NOREPLACE)` on Linux, `renamex_np(..., RENAME_EXCL)` on macOS, `MoveFileExW` without `MOVEFILE_REPLACE_EXISTING` on Windows — never a check-then-rename sequence, which races concurrent writers. For `always`, use a replacing rename (`rename(2)` / `MOVEFILE_REPLACE_EXISTING`).
12. Fsync the destination parent directory.
13. On any failure, remove the temporary output and leave the previous destination unchanged.

## 19.4 Symbolic-link restore

- Store and recreate the link text, not the target contents.
- Never follow the saved target during restore.
- Apply overwrite rules before creating the link.
- Reject a symbolic-link destination path if an ancestor is an existing symbolic link unless a hardened path-safe implementation proves containment.
- Display a warning when a saved absolute symlink target is restored; the link may point outside the restore tree by design, but LocalVault itself must not follow it.
- On Windows, creating symbolic links requires Developer Mode or `SeCreateSymbolicLinkPrivilege`. When creation fails for that reason, skip the link with a warning and continue (partial success), and say so in the result summary. Use `SYMBOLIC_LINK_FLAG_DIRECTORY` for links whose stored target was a directory, and `SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE` when available.

## 19.5 Conflict policies

### `never`

- Skip or fail every path that already exists.
- The CLI should return partial-success status when some selected paths were skipped.

### `prompt`

- The core emits a conflict request through an interface callback.
- The CLI asks on the terminal.
- The GUI displays a dialog.
- Allow "apply to all" decisions.

### `always`

- Replace regular files and links through safe temporary publication.
- Replacing a directory with a non-directory, or the reverse, requires explicit handling and should fail by default in the first release.

## 19.6 Restore result verification

A successful file restore requires:

- Every object existed.
- Every object decompressed to its expected raw size.
- Every raw chunk matched its BLAKE3 hash.
- Total reconstructed bytes matched `logical_size`.
- Full reconstructed file matched `file_hash`.
- Atomic publication succeeded.

---

# 20. Snapshot diff

## 20.1 Comparison key

Compare entries by normalized `relative_path`.

## 20.2 Classification

For each path in the union of both snapshots:

- Present only in newer snapshot: `added`.
- Present only in older snapshot: `removed`.
- Different entry type: `type_changed`.
- Both regular files and `file_hash` differs, or both symbolic links and `symlink_target` differs: `content_modified`.
- Same content (file hash or link target) but mode, attributes, or mtime differs: `metadata_modified`.
- Equivalent content and selected metadata: `unchanged`.

For directories, compare mode and modification time only when metadata comparison is enabled. Directory modification times often change because children changed; provide an option to suppress noisy directory metadata changes.

## 20.3 Efficient implementation

Use two sorted SQL cursors by `relative_path` and perform a merge comparison. Do not load millions of entries into memory.

Conceptually:

```sql
SELECT relative_path, entry_type, logical_size, modified_time_ns,
       posix_mode, file_hash, symlink_target
FROM entries
WHERE snapshot_id = ?
ORDER BY relative_path;
```

## 20.4 Diff output type

```cpp
enum class DiffKind {
    added,
    removed,
    type_changed,
    content_modified,
    metadata_modified,
    unchanged
};

struct DiffEntry {
    std::filesystem::path relative_path;
    DiffKind kind{};
    std::optional<EntryInfo> before;
    std::optional<EntryInfo> after;
};
```

Support streaming results through a callback and paged results for the GUI.

---

# 21. Integrity verification

## 21.1 Quick verification

Quick verification checks metadata and object presence without reading every object body:

1. `PRAGMA integrity_check`.
2. Foreign-key check:
   ```sql
   PRAGMA foreign_key_check;
   ```
3. Validate one `repository_info` row exists.
4. Validate repository format and algorithms.
5. Ensure complete snapshots have valid counters.
6. Ensure every `entry_chunks.chunk_hash` has a `chunks` row.
7. Ensure every referenced chunk object file exists.
8. Ensure object file sizes match `compressed_size`.
9. Ensure derived object paths match stored object paths.
10. Detect stale pending, failed, and unfinished `deleting` snapshots.
11. Detect stale temporary files.

## 21.2 Full verification

Full verification includes quick verification and, for every distinct referenced chunk:

1. Read the object.
2. Decompress to exactly `raw_size`.
3. Compute BLAKE3.
4. Compare against `chunks.hash`.
5. Report corruption without stopping at the first issue unless continuing is unsafe.

Optionally verify full file hashes by reconstructing streams from chunks without writing destination files. This is expensive and may be exposed as:

```bash
localvault verify --full --files
```

## 21.3 Verification issue handling

Verification does not automatically delete corrupt data.

- Missing/corrupt referenced objects are fatal repository health problems.
- Missing unreferenced objects are stale metadata and can be removed by GC.
- Extra object files not represented in SQLite are orphan candidates, not corruption of complete snapshots.
- The command returns a nonzero exit code when any required object or relationship is invalid.

## 21.4 Repair policy

The first release should not claim automatic repair. Safe repair actions may include:

- Remove stale temporary files.
- Delete metadata for stale incomplete snapshots.
- Register or delete verified orphan objects only through a deliberate maintenance command.

Never fabricate missing backup content.

---

# 22. Snapshot deletion and garbage collection

## 22.1 Delete snapshot

Deletion requires an exclusive repository lock. Do not delete millions of entry rows in one transaction — the same principle as Section 16.3.

1. Validate the snapshot exists.
2. Prevent deletion of a `pending` snapshot being processed by the current operation.
3. Set the snapshot `status` to `deleting` and commit; the snapshot is now invisible to browsing and restore, and recovery treats it as a resumable deletion.
4. Delete its `entries` rows in bounded batches (for example 10,000 entries per transaction); `ON DELETE CASCADE` removes the matching `entry_chunks` rows and warnings with each batch.
5. Delete the snapshot row in a final transaction.
6. Do not delete objects in the same command unless `--gc` is explicitly requested.

A crash mid-deletion leaves a `deleting` snapshot; startup recovery (Section 23.4) finishes the batched deletion. This keeps transactions bounded and object cleanup independently recoverable.

## 22.2 Identify unreferenced chunks

```sql
SELECT c.hash, c.object_path, c.compressed_size
FROM chunks AS c
LEFT JOIN entry_chunks AS ec ON ec.chunk_hash = c.hash
WHERE ec.chunk_hash IS NULL
ORDER BY c.hash;
```

Because normal browsing and restore only use complete snapshots, recovery must remove entry metadata for stale incomplete snapshots before GC.

## 22.3 Dry run

`gc --dry-run` reports:

- Number of unreferenced chunk rows.
- Number of orphan object files.
- Bytes reclaimable.
- Stale temporary files.
- Stale incomplete snapshot metadata.

It performs no deletions.

## 22.4 Deletion order

For each unreferenced chunk:

1. Confirm it is still unreferenced under the exclusive lock.
2. Delete the object file.
3. Delete the `chunks` row.
4. Commit in bounded batches.

A crash after object deletion but before row deletion leaves an unreferenced row pointing to a missing object. The next GC can remove it safely because no snapshot references it.

## 22.5 Orphan object files

Walk `objects/` and find files not represented in `chunks`.

Before deleting one:

- Verify its filename has the expected hash format.
- Ensure it is not a temporary file.
- Optionally decompress and hash it for diagnostics.
- Delete only under the exclusive repository lock.

---

# 23. Crash consistency and recovery

## 23.1 Consistency model

LocalVault guarantees:

- Previously complete snapshots are never modified.
- New objects are immutable.
- New snapshots are invisible until marked `complete`.
- A crash may leave temporary files, unreferenced objects, or a non-complete snapshot row.
- Recovery can remove those leftovers without changing complete snapshots.

## 23.2 Object durability sequence

For a new object:

```text
write temporary file
→ flush stream
→ fsync temporary file
→ atomic rename into objects/
→ fsync objects shard directory
→ insert/confirm chunks row
→ allow entry metadata to reference it
```

Do not reference an object before it is durably published.

Platform durability statement:

- **Linux/macOS:** flush the stream, `fsync` the temporary file (`F_FULLFSYNC` on macOS where full durability is required), `rename(2)`, then `fsync` the shard directory.
- **Windows:** flush the stream, `FlushFileBuffers` on the temporary file, then `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`. Windows has no directory fsync; NTFS journals metadata, so rename durability relies on the filesystem journal. This is a documented, weaker guarantee than POSIX (Section 39). The recovery model already tolerates a lost rename: the object is then simply absent, and its snapshot is still `pending`.

## 23.3 Snapshot publication

The final transaction:

1. Confirms no fatal worker error occurred.
2. Confirms all metadata queues are drained.
3. Calculates counters from committed metadata where practical.
4. Updates the snapshot row from `pending` to `complete`.
5. Sets completion time and duration.
6. Commits.

No other status transition can make a snapshot restorable.

## 23.4 Startup recovery

On repository open for a mutating operation:

1. Acquire the writer lock.
2. Find snapshots with `status != 'complete'`.
3. Mark stale `pending` snapshots as `failed`, preserve the row with failure information, and delete their entries, mappings, and warnings in bounded batches.
4. Resume `deleting` snapshots: continue the batched deletion of Section 22.1 to completion.
5. Remove all files under `temporary/`. The exclusive writer lock guarantees no live operation owns them (the v1 locking rule, Section 23.6), so no age threshold is needed. On Windows, tolerate sharing violations by skipping the file and retrying on the next recovery.
6. Leave orphan final objects for explicit GC.
7. Run a quick relationship check.

## 23.5 SQLite transaction wrapper

Use RAII:

```cpp
class Transaction {
public:
    explicit Transaction(Database& db);
    ~Transaction();  // rolls back if not committed

    void commit();

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

private:
    Database* db_;
    bool committed_{false};
};
```

Never call `COMMIT` from a destructor. The destructor only performs best-effort rollback.

## 23.6 Repository lock

### The v1 locking rule

One exclusive lock covers every operation that mutates the repository **or reads object files**:

- Take the exclusive lock: `init`, `snapshot`, `delete`, `gc`, recovery, `restore`, and `verify`. Restore and verify take it because they read object files and must not race garbage collection deleting those objects.
- No lock required: pure-metadata queries (`list`, `show`, `files`, `diff`, `stats`) read only SQLite, and WAL gives them a consistent view concurrent with a writer.

Relaxing this later requires a shared/exclusive (reader–writer) lock so restores and verifies can run concurrently with each other while still excluding GC. Do not relax it ad hoc.

### Implementation

POSIX (Linux/macOS): advisory lock with `flock(LOCK_EX | LOCK_NB)` or `fcntl(F_SETLK)` on `repository.lock`.

Windows: open `repository.lock` with `CreateFileW` and acquire `LockFileEx(LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY)`. Keep the handle open for the lock lifetime; release with `UnlockFileEx`/`CloseHandle` in the RAII destructor. Never delete `repository.lock` during cleanup — an open file cannot be deleted on Windows, and the file's existence is harmless.

Required behavior on all platforms:

- Open/create `repository.lock`.
- Acquire the exclusive non-blocking lock.
- Return `repository_busy` if another process owns it.
- Keep the descriptor/handle open for the lock lifetime.
- Release in the RAII destructor.
- Record PID and start time in the lock file for diagnostics, but do not treat text content alone as lock ownership.

---

# 24. Concurrency and cancellation

## 24.1 Pipeline

```text
Scanner thread
    │ FileJob
    ▼
BoundedQueue<FileJob>
    │
    ├── Worker 1 ─┐
    ├── Worker 2 ─┤ ProcessedEntry
    ├── Worker 3 ─┤
    └── Worker N ─┘
                  ▼
       BoundedQueue<ProcessedEntry>
                  │
                  ▼
          Metadata writer thread
```

## 24.2 Work units

```cpp
struct FileJob {
    std::filesystem::path absolute_path;
    std::filesystem::path relative_path;
    FileMetadata before;
};

struct ChunkReference {
    std::string hash_hex;
    std::uint64_t raw_offset{};
    std::uint32_t raw_length{};
    std::uint64_t compressed_size{};
    bool newly_stored{};
};

struct ProcessedEntry {
    EntryInfo entry;
    std::vector<ChunkReference> chunks;
    bool reused_previous_metadata{};
};
```

## 24.3 Bounded queues

Implement a queue with:

- Maximum item count and/or byte budget.
- `push` that waits until space or cancellation.
- `pop` that waits until data, closure, or cancellation.
- `close` to signal no more items.
- No busy waiting.
- Condition variables protected by one mutex.
- Exception-safe element transfer.

A byte budget is useful because one result may contain thousands of chunk references.

## 24.4 Worker count

Default:

```cpp
auto count = std::thread::hardware_concurrency();
count = count == 0 ? 4 : count;
count = std::clamp(count, 1U, 16U);
```

Allow configuration but enforce a safe maximum.

Disk-bound workloads may perform worse with too many workers. Benchmark before changing the default.

## 24.5 SQLite access

Initial policy:

- One write connection owned by the metadata writer.
- Read-only queries from the coordinating thread or separate read connections.
- No SQLite connection is shared concurrently unless opened/configured for that use and protected.
- Prepared statements are not shared across threads.

## 24.6 Cancellation

Use `std::stop_source`, `std::stop_token`, and `std::jthread`.

Check the token:

- Before scanning a directory.
- Before opening a file.
- Between chunks.
- Before compression.
- Before long database batches.
- Between objects during verification and GC.

Cancellation is cooperative. Do not forcibly terminate worker threads.

## 24.7 Error propagation

- The first fatal worker error is stored in a synchronized `std::exception_ptr`.
- Request stop on all workers.
- Close queues.
- Join all threads.
- Convert the original exception to a structured operation failure.
- Do not allow worker exceptions to escape a thread function.

## 24.8 Progress aggregation

Workers update atomic counters. A coordinator emits throttled progress events, for example at most 10 times per second, plus phase changes. Avoid invoking expensive UI callbacks for every chunk.

---

# 25. Filesystem behavior

## 25.1 Relative path representation

Store repository paths as normalized UTF-8 with `/` separators.

First-release policy:

- Accept paths convertible to valid UTF-8. On Windows, convert from UTF-16; names containing unpaired surrogates are not convertible and are skipped with a warning.
- Skip non-convertible paths with a warning.
- Do not silently replace invalid bytes.
- Use exact stored path strings for identity. **No Unicode normalization is applied**: macOS commonly produces NFD names while Linux and Windows commonly produce NFC, so two names that render identically but differ in normalization are distinct entries. Document this; never normalize silently.

On case-insensitive or normalization-insensitive destination filesystems, detect collisions during restore planning and skip losers with warnings (Section 25.12).

## 25.2 Regular files

- Open in binary mode.
- Stream contents.
- Save size, mtime, change time, and platform metadata per the policy matrix (Section 25.10).
- Do not preserve ownership in the first release.
- Do not preserve sparse holes in the first release.

## 25.3 Directories

- Store the root and empty directories.
- Restore parents before children.
- Apply final directory metadata after restoring children.

## 25.4 Symbolic links

- Use `symlink_status`.
- Save `read_symlink` text.
- Do not recurse through symlink targets.
- Restore the link itself.
- On Windows, NTFS symbolic links are saved and restored like POSIX symlinks (restore may require privilege, Section 19.4). Directory junctions are saved as symlink entries with their target text and are never traversed; volume mount points are skipped with a warning (FR-117). Never traversing junctions also prevents the classic recursion loops in Windows user profiles (for example the legacy `Application Data` junction).

## 25.5 Special files

Skip and warn for:

- Sockets.
- FIFOs/named pipes.
- Block devices.
- Character devices.
- Unknown file types.

Never attempt to read a device as a regular file.

## 25.6 Hidden files

Include hidden files by default. Ignore rules may exclude them.

## 25.7 Hard links

The first release treats hard-linked paths as independent logical files. Chunk deduplication prevents duplicated stored bytes, but restore creates separate files. Record device/inode fields for future hard-link support.

## 25.8 Source/repository containment

Reject:

- Source path equal to repository root.
- Source inside repository.
- Repository inside source.

A future version may automatically ignore the repository, but explicit rejection is safer.

## 25.9 Metadata failures

Content restore success should not be discarded because applying a nonessential timestamp or mode failed. Report metadata failure as a warning and return partial success.

## 25.10 Platform metadata policy matrix

This matrix is the single normative statement of what is saved and restored per platform. FR-104 and FR-309 refer here.

| Item | Linux | macOS | Windows |
|---|---|---|---|
| File content, size | saved + restored | saved + restored | saved + restored |
| Modification time | saved + restored | saved + restored | saved + restored |
| Change time (ctime) | saved (reuse rule only, never restored) | saved (reuse only) | saved (NTFS change time, reuse only) |
| POSIX mode | saved + restored | saved + restored | not saved (`posix_mode = 0`) |
| Windows attributes | — | — | READONLY/HIDDEN/SYSTEM saved + restored; ARCHIVE saved, not restored |
| Ownership (uid/gid, SID) | not saved | not saved | not saved |
| ACLs / xattrs / ADS | not saved | not saved | not saved |
| Symbolic links | saved + restored | saved + restored | saved; restored when privilege allows, else skip + warn |
| Directory junctions | — | — | saved as link entries, never traversed |
| Hard-link relationships | not recreated | not recreated | not recreated |

Cross-platform restores apply only the columns the destination supports; everything else degrades to a warning (Section 25.12).

## 25.11 Windows filesystem specifics

- **Long paths:** embed a `longPathAware` application manifest in both executables and use `\\?\`-prefixed absolute paths inside the Win32 platform layer, so paths beyond 260 characters work regardless of system policy.
- **Sharing violations:** files opened exclusively by other processes (`ERROR_SHARING_VIOLATION`) are skipped with a warning after the standard unstable-file retry (FR-116). VSS integration is future work (Section 40.5).
- **Cloud placeholders:** files carrying `FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS` or `FILE_ATTRIBUTE_RECALL_ON_OPEN` (OneDrive Files On-Demand and similar) are skipped with a warning; reading them would silently download content (FR-118).
- **Antivirus interference:** rapid creation of many object files can trigger transient open failures from real-time scanners. Wrap object publication in a small bounded retry, and document Defender exclusions as a user-level optimization.
- **8.3 short names:** store long names only; never store or match `PROGRA~1`-style aliases.
- **One-file-system option:** `one_file_system` (FR-119) stops the scanner at filesystem boundaries on POSIX (compare `st_dev`) and at volume boundaries on Windows. Junctions and mount points are never traversed on Windows regardless of this option.

## 25.12 Cross-platform repository portability

A repository created on any supported platform must open on every other supported platform: SQLite, zstd frames, and hash-derived object paths are byte-portable, and stored paths are normalized UTF-8 with `/` separators.

The constraint is restore-side representability. During restore planning:

- On Windows, entries whose names contain characters illegal on NTFS (`< > : " / \ | ? *`, control characters), reserved device names (`CON`, `PRN`, `AUX`, `NUL`, `COM1`–`COM9`, `LPT1`–`LPT9`, with or without extension), or trailing dots/spaces are skipped with warnings (FR-310).
- On case-insensitive or normalization-insensitive destinations, entries that collide after the filesystem's folding are detected in the plan; the first entry in path order is restored and the rest are skipped with warnings.
- Metadata degrades per the policy matrix: restoring a Windows-captured snapshot on Linux applies umask-default permissions with a warning; restoring a POSIX snapshot on Windows ignores `posix_mode`.

Add integration tests that create a repository on one platform in CI, hand it off as an artifact, and open/verify/restore it on the other platforms (Section 32.3).

---

# 26. Restore security

## 26.1 Lexical validation

For every stored relative path:

```cpp
bool is_safe_relative_path(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_path()) {
        return false;
    }

    const auto normalized = path.lexically_normal();
    for (const auto& component : normalized) {
        if (component == "..") {
            return false;
        }
    }
    return normalized != ".";
}
```

This is necessary but not sufficient when existing symbolic links are present in the destination.

On Windows, additionally reject before the checks above: paths with a root name (`C:`, `\\server\share`, including drive-relative forms like `C:foo`), backslashes in stored paths (stored paths use `/` only), reserved device names, and components ending in a dot or space (Section 25.12).

## 26.2 Containment validation

Minimum implementation:

1. Require an absolute destination root.
2. Create or canonicalize the destination root.
3. Reject unsafe relative paths.
4. Join root and normalized relative path.
5. Verify existing ancestors are not symbolic links.
6. Re-check immediately before publication.

Hardened POSIX implementation:

- Open the destination root directory once.
- Walk/create components with `openat`/`mkdirat`.
- Use `O_NOFOLLOW` for existing components.
- Create output files relative to trusted directory descriptors.
- Avoid security decisions based only on string prefix comparisons.

Hardened Windows implementation (there is no `openat` equivalent in the C runtime):

- Canonicalize the destination root once and operate on `\\?\`-prefixed absolute paths only.
- For each existing component, open it with `FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS` and reject any component carrying `FILE_ATTRIBUTE_REPARSE_POINT` (symlink, junction, or any other reparse tag).
- Create output files with `CREATE_NEW` against the validated path, and re-verify the parent chain immediately before publication.
- The same rule applies: never decide containment from string prefixes alone.

## 26.3 String-prefix checks are forbidden

This is unsafe:

```cpp
candidate.string().starts_with(root.string())
```

For example, `/safe/root2` begins with `/safe/root` as text but is not inside it.

## 26.4 Malicious repository metadata

Treat repository metadata as untrusted during restore even when LocalVault created it. A damaged database must not cause writes outside the destination.

## 26.5 Resource limits

Before allocating:

- Require chunk `raw_size <= repository chunk size`.
- Require compressed size below a reasonable configured limit.
- Detect integer overflow in byte-count arithmetic.
- Limit search page size.
- Limit worker count.
- Limit error collection or stream issues incrementally for severely corrupt repositories.

---

# 27. Ignore rules

Use a root-level file named:

```text
.localvaultignore
```

## 27.1 First-release syntax

Support:

- Blank lines.
- Comments beginning with `#`.
- Exact relative paths.
- Directory patterns ending in `/`.
- `*` within one path component.
- `?` for one character within a component.
- Patterns without `/` matching a name at any depth.
- `!` negation may be deferred; if implemented, define ordering clearly.

Example:

```gitignore
# Build output
build/
out/

# Temporary files
*.tmp
*.log

# Tool caches
.cache/
.DS_Store
```

## 27.2 Matching rules

- Normalize separators to `/`.
- Match against repository-relative paths.
- Decide case sensitivity according to source filesystem behavior; do not force lowercase.
- When a directory is ignored, do not recurse into it.
- The ignore file itself is included unless explicitly ignored.
- When `--ignore-file <path>` is supplied on the CLI, it replaces the source root's `.localvaultignore` entirely; the two are never merged.
- Always ignore the repository if a future configuration allows it inside the source, though the first release rejects that layout.

## 27.3 Test cases

Test:

- Exact filename.
- Extension wildcard.
- Directory wildcard.
- Nested directory.
- Pattern containing spaces.
- Escaped leading `#` if escape syntax is supported.
- Hidden files.
- Case differences.
- Non-match close to a pattern.

Do not claim full Git ignore compatibility unless it is actually implemented.

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

# 37. Implementation milestones

Complete milestones in order. Do not begin the full GUI before the core snapshot/restore path is tested.

Every milestone must build and pass its tests on Linux, macOS, and Windows before it is complete — platform debt is not carried forward. For each milestone, produce the implementation log and the independent verification log under `docs/implementation-logs/<milestone>/` as required by the workspace workflow, and record which functional requirements the milestone satisfies.

## Milestone 0 — Repository scaffold

Implement:

- Directory layout.
- CMake.
- CMake presets (including the Windows Visual Studio presets).
- vcpkg manifest and baseline.
- CI vcpkg binary caching (Section 35.4) — set up before anything else, or every CI run rebuilds Qt.
- Empty `localvault_core`.
- CLI `--version`.
- Empty Qt main window.
- One GoogleTest test.
- CI configure/build/test on Linux, macOS, and Windows.

Reference sections: 5–10, 35.

Acceptance:

```text
Fresh checkout → configure → build → test
```

works on Linux, macOS, and Windows.

## Milestone 1 — Database and repository lifecycle

Implement:

- SQLite RAII wrappers.
- Migration framework.
- Initial schema (including `repository_settings`).
- Repository create/open (both open modes), destination filesystem classification, and restrictive repository permissions.
- Repository format validation.
- Writer lock: POSIX (`flock`/`fcntl`) and Win32 (`LockFileEx`) implementations behind one interface.
- Query repository information.

Reference sections: 12.4, 13–15, 23.6.

Tests:

- Create/open.
- Invalid directory.
- Unsupported format.
- Transaction rollback.
- Lock contention.

## Milestone 2 — Minimal whole-file snapshot/restore

Before chunking, prove the end-to-end path with one object per file or one chunk per small test file:

- Scan directories.
- Store entries.
- Store file content.
- List snapshots.
- Restore complete tree.
- Compare bytes.

Acceptance:

```text
source → snapshot → delete copy → restore → tree comparison passes
```

This milestone reduces risk before concurrency and deduplication are added.

## Milestone 3 — Fixed-size chunks, BLAKE3, and zstd

Replace minimal object storage with:

- 4 MiB chunking.
- BLAKE3 chunk and file hashes.
- zstd compression.
- Hash-derived object paths.
- Ordered chunk mappings.
- Chunk verification.
- Duplicate reuse.

Acceptance:

- Large multi-chunk file restores correctly.
- Identical files share objects.
- Unchanged second snapshot writes no new content objects.

## Milestone 4 — Crash-safe object and snapshot publication

Implement:

- Temporary object files.
- Per-platform durability sequence (fsync / `FlushFileBuffers`, Section 23.2).
- Atomic rename (`rename` / `MoveFileExW`).
- Snapshot statuses, including `deleting` with resumable batched deletion.
- Metadata batches.
- Recovery of stale snapshots.
- Failure injection tests via `Repository::set_failure_injector`.

Reference sections: 18.6, 22.1, 23.

Acceptance:

Injected failure at each publication point never damages a previously complete snapshot.

## Milestone 5 — Concurrency and cancellation

Implement:

- Bounded queues.
- Scanner producer (junction/mount-point rules and `one_file_system`, Sections 25.4 and 25.11).
- Worker pool.
- Metadata writer.
- Stop tokens.
- Progress aggregation (including totals after scan completion).
- Fatal-error propagation.

Reference sections: 16, 24, 25.

Acceptance:

- Cancellation leaves no complete partial snapshot.
- ASan/UBSan tests pass.
- TSan-focused tests pass when enabled.
- Memory remains bounded on a large generated dataset.

## Milestone 6 — Diff, verification, delete, and GC

Implement:

- Streaming snapshot diff.
- Quick verification.
- Full object verification.
- Snapshot deletion.
- GC dry run.
- GC execution.
- Repository statistics.

Acceptance:

- Corruption tests detect missing and modified objects.
- GC never removes shared chunks.
- Dry run makes no changes.

## Milestone 7 — Complete CLI

Implement all commands, exit codes, JSON output, progress, signal cancellation (POSIX signals and `SetConsoleCtrlHandler`), and clear errors.

Acceptance:

All core functions can be exercised without the GUI, and the CLI end-to-end suite (Section 32.7) passes on all three platforms.

## Milestone 8 — Qt desktop application

Implement:

- Main window/navigation.
- First-run onboarding (create/open repository).
- Repository opening.
- Dashboard.
- Snapshot table.
- Lazy restore tree.
- Restore conflicts.
- Verification page.
- Settings (backed by `repository_settings`).
- Garbage-collection UI with mandatory dry-run preview.
- Background workers.
- Progress and cancellation.
- Error dialogs.

Acceptance:

No long core operation runs on the GUI thread, and the application remains responsive.

## Milestone 9 — Quality, performance, and packaging

Implement:

- Complete unit/integration suite, including cross-platform portability fixtures.
- Static analysis.
- Sanitizer CI.
- Deterministic benchmarks.
- Install rules.
- Packages for all three platforms (`windeployqt`/`macdeployqt` output validated on clean machines).
- Code-signing decision documented.
- Documentation (user guide and per-platform notes).
- Tagged release.

---

# 38. Definition of done

The first release is complete only when every item below is true.

## Build

- [ ] Clean Linux checkout configures with documented commands.
- [ ] Clean macOS checkout configures with documented commands.
- [ ] Clean Windows checkout configures with documented commands.
- [ ] Debug and Release builds succeed.
- [ ] CLI and GUI targets build.
- [ ] vcpkg baseline is pinned.
- [ ] Project code compiles without warnings in CI.
- [ ] No deprecated API warning is knowingly ignored without documentation.

## Repository

- [ ] Repository initializes safely.
- [ ] Format version is stored and validated.
- [ ] Migrations are transactional.
- [ ] Concurrent writer is rejected.
- [ ] Stale incomplete operations recover safely.

## Snapshot

- [ ] Nested directories snapshot correctly.
- [ ] Empty files and directories are represented.
- [ ] Symlinks are saved without following.
- [ ] Large files stream in bounded memory.
- [ ] 4 MiB chunking is correct.
- [ ] BLAKE3 hashes are verified.
- [ ] zstd round trips correctly.
- [ ] Duplicate chunks are stored once.
- [ ] Unstable files are retried or reported.
- [ ] Cancellation does not publish a partial snapshot.
- [ ] A complete snapshot contains no missing references.
- [ ] Windows: junctions are not traversed, sharing violations and cloud placeholders become warnings, and long paths work.

## Restore

- [ ] One file restores.
- [ ] One directory restores.
- [ ] Complete snapshot restores.
- [ ] Restored bytes match source.
- [ ] Chunk and final file hashes are checked.
- [ ] Temporary writes and atomic publication are used.
- [ ] Overwrite policies work.
- [ ] Path traversal is rejected.
- [ ] Existing symlink ancestors cannot escape the destination.
- [ ] Platform metadata restores per the policy matrix where supported.
- [ ] Non-representable names and collisions are skipped with warnings, never silent failures.
- [ ] Windows symlink restore degrades to skip-plus-warning without privilege.

## Maintenance

- [ ] Snapshot diff works.
- [ ] Quick verification works.
- [ ] Full verification detects corruption.
- [ ] Snapshot deletion is transactional.
- [ ] GC dry run is accurate.
- [ ] GC retains shared chunks.
- [ ] Storage statistics use documented formulas.

## Interfaces

- [ ] CLI exposes all required operations.
- [ ] CLI exit codes are stable.
- [ ] JSON output is valid where supported.
- [ ] GUI uses the core library directly.
- [ ] GUI remains responsive.
- [ ] GUI lazily loads large snapshot trees.
- [ ] Errors and partial results are visible.

## Quality

- [ ] Required unit tests pass.
- [ ] Required integration tests pass.
- [ ] ASan passes.
- [ ] UBSan passes.
- [ ] Format check passes.
- [ ] clang-tidy policy passes.
- [ ] Benchmarks record reproducible environment details.
- [ ] A release package runs on a clean target system for each platform.
- [ ] A repository created on each platform opens and restores on the other two.

---

# 39. Known limitations

Document these in the first release:

- Snapshots are application-level scans, not filesystem-atomic snapshots.
- Files changing repeatedly during a scan may be skipped.
- Fixed-size chunking handles aligned changes well but handles byte insertions less efficiently than content-defined chunking.
- Paths must be valid UTF-8 under the first-release policy; no Unicode normalization is applied, so NFC/NFD variants are distinct entries.
- Ownership, ACLs, extended attributes, alternate data streams, and sparse holes are not preserved on any platform.
- Hard-link relationships are not recreated.
- Special files are skipped.
- Linux, macOS, and Windows 10/11 are the supported platforms; network filesystems and FAT/exFAT are risky repository destinations and are rejected or warned at `init`.
- Windows: files locked by other processes are skipped (no VSS support); cloud placeholder files are skipped without hydration; symlink restore requires Developer Mode or privilege, otherwise links are skipped; junctions are stored as links and never traversed.
- Windows object publication relies on NTFS metadata journaling for rename durability (no directory fsync exists); this is weaker than the POSIX guarantee.
- macOS: backing up protected locations (Documents, Desktop, Photos) requires the user to grant Full Disk Access; without it, files are skipped with permission warnings.
- Snapshots of trees with very many small files are fsync-bound (two syncs per new object); pack-file aggregation is future work (Section 40.7).
- The SQLite database grows with snapshots × entries and does not shrink after deletions until a future maintenance command runs `VACUUM` (Section 40.9).
- There are no retention policies; snapshot deletion is manual.
- Repository content is not encrypted.
- A single repository supports one writer at a time.
- Local filesystem durability still depends on operating-system and storage behavior.
- Verification can detect missing content but cannot reconstruct it without another copy.

---

# 40. Optional advanced work

Add only after the definition of done is satisfied.

## 40.1 Content-defined chunking

Use a rolling hash to select chunk boundaries with minimum, target, and maximum sizes. Add a new repository format/configuration value so old fixed-chunk repositories remain readable.

Required comparison:

- Dedup ratio after insertion near beginning of large files.
- CPU cost.
- Average chunk count.
- Memory and throughput.

## 40.2 Encryption

Use a maintained cryptographic library and authenticated encryption. Never invent an algorithm.

Design requirements:

- Key derivation from password with a standard memory-hard KDF.
- Random repository salt.
- Authenticated metadata/object envelopes.
- Key rotation design.
- No plaintext hash leakage decision made explicitly.
- Recovery implications documented.

## 40.3 Remote object store

Define:

```cpp
class IObjectBackend {
public:
    virtual ~IObjectBackend() = default;
    virtual bool exists(std::string_view hash) = 0;
    virtual void put(std::string_view hash, std::span<const std::byte> bytes) = 0;
    virtual std::vector<std::byte> get(std::string_view hash) = 0;
    virtual void remove(std::string_view hash) = 0;
};
```

Keep repository metadata consistency and retry/idempotency rules explicit.

## 40.4 Scheduling and change monitoring

- Use platform schedulers or a background agent.
- Add filesystem event APIs only as an optimization.
- Always retain a full reconciliation scan because event streams can overflow or miss offline changes.

## 40.5 Volume Shadow Copy Service (VSS) snapshots

Windows support is part of the first release (Sections 25.10–25.12); the remaining Windows gap is point-in-time consistency for files locked by other processes.

- Create a read-only VSS snapshot of the source volume and scan through the shadow path, eliminating sharing violations and mid-read modification races.
- Requires administrator rights and careful COM lifetime management (`IVssBackupComponents`).
- Must remain optional: fall back to the current skip-with-warning behavior when elevation is unavailable.
- Do not claim application-consistent backups (writer participation) — volume-level crash consistency only.

## 40.6 Fuzz testing

Good targets:

- Ignore parser.
- Object metadata parser if a custom header is introduced.
- Path normalization and restore validation.
- Database import/migration helpers.
- CLI JSON parsing if added.

## 40.7 Pack files for small objects

Aggregate many small chunks into append-only pack files with an index, amortizing fsync cost and directory-entry explosion for many-small-file workloads (Section 39). Requires a repository format version bump; loose objects remain readable.

## 40.8 Retention policies

`localvault prune --keep-last N --keep-daily D --keep-weekly W` style selection on top of the existing transactional delete plus GC. Policy evaluation must produce a dry-run-able plan before any deletion.

## 40.9 Database maintenance command

`localvault maintenance` runs `PRAGMA wal_checkpoint(TRUNCATE)` and `VACUUM` under the exclusive lock to reclaim space after large deletions, with a disk-space precheck (VACUUM temporarily doubles database size).

---

# 41. Build and dependency maintenance

This section prevents avoidable deprecation and reproducibility problems.

## 41.1 Do not chase every newest release

Use:

- A modern language standard.
- Supported tool versions.
- A pinned dependency baseline.
- Deliberate update pull requests.

The build should be reproducible, not permanently floating.

## 41.2 CMake rules

- Keep `cmake_minimum_required` modern.
- Use target-based commands.
- Do not use global `include_directories`, `link_directories`, or global compiler flags.
- Do not depend on removed compatibility modes.
- Test with the minimum supported CMake and the CI CMake.
- Update CMake policy behavior intentionally.
- Avoid setting policy versions based on an untested future release.

## 41.3 C++ rules

- Use `target_compile_features(... cxx_std_20)`.
- Set extensions off.
- Do not depend on compiler-specific extensions without an abstraction.
- Prefer standard-library facilities.
- Keep compiler-specific warning options inside CMake conditionals.

## 41.4 Third-party warnings

- Do not apply `-Werror` to third-party source.
- Mark external include directories `SYSTEM` when needed.
- Keep adapter wrappers small.
- Update or patch dependencies in isolated commits.
- Never suppress a project warning merely because a dependency emits a similar warning.

## 41.5 vcpkg rules

- Commit `vcpkg.json`.
- Commit the generated `builtin-baseline`.
- Pin the vcpkg tool checkout in CI.
- Regenerate and test the baseline deliberately.
- Never use a developer's globally integrated vcpkg state as the only documented setup.
- Delete build directories after changing triplets/toolchains when CMake cache conflicts occur.

## 41.6 Qt rules

- Use Qt 6 APIs only.
- Avoid Qt 5 compatibility examples.
- Keep GUI optional in CMake.
- Keep Qt types out of core public APIs.
- Test deployment separately from development execution.
- Update Qt and the deployment process together in a dedicated change.

## 41.7 SQLite rules

- Use prepared statements.
- Check every result code.
- Enable foreign keys on every connection.
- Run migrations transactionally.
- Do not expose raw `sqlite3*` outside the database module.
- Do not interpolate paths or messages directly into SQL.

## 41.8 BLAKE3 and zstd rules

- Keep wrapper tests using known vectors/round trips.
- Check all C API return values.
- Store hash algorithm and chunk size in repository metadata.
- Never reinterpret compressed data without expected raw-size limits.
- A dependency update must pass full repository verification on a fixture created by the previous release.

## 41.9 CI and action maintenance

- Enable Dependabot for GitHub Actions.
- Pin action versions or SHAs.
- Update retired runner images before removal dates.
- Keep a scheduled CI run so dormant breakage is detected.
- Do not make local success the only build guarantee.

## 41.10 Repository compatibility tests

Keep fixture repositories produced by released versions:

```text
tests/fixtures/repositories/v0_1_0/
tests/fixtures/repositories/v0_2_0/
```

Tests must verify:

- Current code opens supported old formats.
- Unsupported future formats are rejected clearly.
- Migrations produce valid current repositories.
- Old complete snapshots restore correctly.

Do not store large binary fixture repositories in Git; create small deterministic fixtures or use release assets with checksums.

---

# 42. Appendix: implementation skeletons

The following skeletons establish conventions. They are not substitutes for tests and error handling.

## 42.1 BLAKE3 wrapper

```cpp
#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string>

struct blake3_hasher;

namespace localvault {

class Blake3Hasher final {
public:
    static constexpr std::size_t digest_size = 32;
    using Digest = std::array<std::byte, digest_size>;

    Blake3Hasher();
    ~Blake3Hasher();

    Blake3Hasher(Blake3Hasher&&) noexcept;
    Blake3Hasher& operator=(Blake3Hasher&&) noexcept;

    Blake3Hasher(const Blake3Hasher&) = delete;
    Blake3Hasher& operator=(const Blake3Hasher&) = delete;

    void update(std::span<const std::byte> bytes);
    [[nodiscard]] Digest finalize() const;
    [[nodiscard]] static std::string to_hex(const Digest& digest);

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace localvault
```

Prefer `std::unique_ptr<Impl>` in the actual implementation to simplify ownership.

## 42.2 zstd wrapper

```cpp
class ZstdCodec final {
public:
    explicit ZstdCodec(int compression_level);

    [[nodiscard]] std::vector<std::byte> compress(
        std::span<const std::byte> raw) const;

    [[nodiscard]] std::vector<std::byte> decompress(
        std::span<const std::byte> compressed,
        std::size_t expected_raw_size,
        std::size_t maximum_raw_size) const;

private:
    int compression_level_;
};
```

## 42.3 SQLite statement wrapper

```cpp
class Statement final {
public:
    Statement(sqlite3* db, std::string_view sql);
    ~Statement();

    Statement(Statement&&) noexcept;
    Statement& operator=(Statement&&) noexcept;

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void bind(std::string_view name, std::int64_t value);
    void bind(std::string_view name, std::string_view value);
    void bind_null(std::string_view name);

    [[nodiscard]] bool step();
    void execute();
    void reset();

    [[nodiscard]] std::int64_t column_int64(int index) const;
    [[nodiscard]] std::string column_text(int index) const;
    [[nodiscard]] bool column_is_null(int index) const;

private:
    sqlite3_stmt* statement_{nullptr};
};
```

The implementation must check every bind, step, reset, and finalize result that can report an error.

## 42.4 Bounded queue skeleton

```cpp
template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity)
        : capacity_(capacity) {
        if (capacity_ == 0) {
            throw std::invalid_argument("queue capacity must be positive");
        }
    }

    bool push(T value, std::stop_token stop_token) {
        std::unique_lock lock(mutex_);
        condition_not_full_.wait(lock, stop_token, [this] {
            return closed_ || queue_.size() < capacity_;
        });

        if (closed_ || stop_token.stop_requested()) {
            return false;
        }

        queue_.push_back(std::move(value));
        condition_not_empty_.notify_one();
        return true;
    }

    std::optional<T> pop(std::stop_token stop_token) {
        std::unique_lock lock(mutex_);
        condition_not_empty_.wait(lock, stop_token, [this] {
            return closed_ || !queue_.empty();
        });

        if (queue_.empty()) {
            return std::nullopt;
        }

        T value = std::move(queue_.front());
        queue_.pop_front();
        condition_not_full_.notify_one();
        return value;
    }

    void close() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        condition_not_empty_.notify_all();
        condition_not_full_.notify_all();
    }

private:
    std::size_t capacity_;
    std::deque<T> queue_;
    bool closed_{false};
    std::mutex mutex_;
    std::condition_variable_any condition_not_empty_;
    std::condition_variable_any condition_not_full_;
};
```

Add tests for closure, cancellation, multiple producers/consumers, and exception-safe moves.

## 42.5 Safe integer conversion

Avoid unchecked narrowing from SQLite signed integers to unsigned byte counts:

```cpp
std::uint64_t checked_to_u64(std::int64_t value, std::string_view field) {
    if (value < 0) {
        throw LocalVaultError(
            ErrorCode::database_error,
            std::string(field) + " contains a negative value");
    }
    return static_cast<std::uint64_t>(value);
}
```

Check additions:

```cpp
std::uint64_t checked_add(std::uint64_t a, std::uint64_t b) {
    if (b > std::numeric_limits<std::uint64_t>::max() - a) {
        throw LocalVaultError(ErrorCode::internal_error, "byte count overflow");
    }
    return a + b;
}
```

## 42.6 Object path derivation

```cpp
std::filesystem::path object_relative_path(std::string_view hash_hex) {
    if (hash_hex.size() != 64 ||
        !std::ranges::all_of(hash_hex, [](unsigned char c) {
            return std::isdigit(c) != 0 ||
                   (c >= static_cast<unsigned char>('a') &&
                    c <= static_cast<unsigned char>('f'));
        })) {
        throw LocalVaultError(ErrorCode::invalid_argument, "invalid BLAKE3 hash");
    }

    return std::filesystem::path("objects") /
           std::string(hash_hex.substr(0, 2)) /
           (std::string(hash_hex) + ".zst");
}
```

Cast safely when calling character-classification functions.

## 42.7 Snapshot metadata publication pseudocode

```cpp
SnapshotResult SnapshotEngine::create_snapshot(...) {
    auto lock = repository_.acquire_exclusive_lock();
    repository_.recover_stale_operations();

    const auto snapshot_id =
        repository_.database().insert_pending_snapshot(source_root, options.message);

    try {
        run_pipeline(snapshot_id, source_root, options, stop_token, progress);

        if (stop_token.stop_requested()) {
            repository_.database().cancel_and_clean_snapshot(snapshot_id);
            throw LocalVaultError(ErrorCode::cancelled, "snapshot cancelled");
        }

        auto result = repository_.database().calculate_snapshot_result(snapshot_id);
        repository_.database().publish_snapshot(snapshot_id, result);
        return result;
    } catch (const LocalVaultError& error) {
        repository_.database().fail_and_clean_snapshot(snapshot_id, error.what());
        throw;
    } catch (const std::exception& error) {
        repository_.database().fail_and_clean_snapshot(snapshot_id, error.what());
        throw LocalVaultError(ErrorCode::internal_error, error.what());
    }
}
```

Ensure failure cleanup itself cannot replace the original exception. Log cleanup failure and preserve the original cause.

## 42.8 Full-file reconstruction pseudocode

```cpp
void RestoreEngine::restore_file(
    const EntryInfo& entry,
    const std::filesystem::path& destination,
    std::stop_token stop_token) {

    validate_restore_destination(destination);

    TemporaryOutputFile output(destination);
    Blake3Hasher file_hasher;
    std::uint64_t written = 0;

    for (const auto& chunk : database_.chunks_for_entry(entry.id)) {
        if (stop_token.stop_requested()) {
            throw LocalVaultError(ErrorCode::cancelled, "restore cancelled");
        }

        auto raw = object_store_.read_verified(
            chunk.hash_hex,
            chunk.raw_length);

        output.write(raw);
        file_hasher.update(raw);
        written = checked_add(written, raw.size());
    }

    if (written != entry.logical_size) {
        throw LocalVaultError(
            ErrorCode::object_corrupt,
            "reconstructed size does not match snapshot metadata",
            entry.relative_path);
    }

    const auto restored_hash = Blake3Hasher::to_hex(file_hasher.finalize());
    if (!entry.file_hash_hex || restored_hash != *entry.file_hash_hex) {
        throw LocalVaultError(
            ErrorCode::object_corrupt,
            "reconstructed file hash does not match snapshot metadata",
            entry.relative_path);
    }

    output.flush_and_sync();
    output.apply_metadata(entry);
    output.publish_atomically();
}
```

Handle empty files: their hasher must finalize the BLAKE3 digest of empty input.

## 42.9 Stats queries

Distinct chunks referenced by one snapshot:

```sql
SELECT
    COALESCE(SUM(c.raw_size), 0) AS unique_raw_bytes,
    COALESCE(SUM(c.compressed_size), 0) AS stored_bytes
FROM chunks AS c
WHERE c.hash IN (
    SELECT DISTINCT ec.chunk_hash
    FROM entry_chunks AS ec
    JOIN entries AS e ON e.id = ec.entry_id
    WHERE e.snapshot_id = :snapshot_id
);
```

Logical bytes:

```sql
SELECT COALESCE(SUM(logical_size), 0)
FROM entries
WHERE snapshot_id = :snapshot_id
  AND entry_type = 'file';
```

Repository-wide retained unique bytes:

```sql
SELECT
    COALESCE(SUM(raw_size), 0),
    COALESCE(SUM(compressed_size), 0)
FROM chunks
WHERE hash IN (
    SELECT DISTINCT ec.chunk_hash
    FROM entry_chunks AS ec
    JOIN entries AS e ON e.id = ec.entry_id
    JOIN snapshots AS s ON s.id = e.snapshot_id
    WHERE s.status = 'complete'
);
```

## 42.10 Minimal CLI bootstrap

```cpp
#include <CLI/CLI.hpp>

#include "localvault/error.hpp"

#include <iostream>

int main(int argc, char** argv) {
    CLI::App app{"LocalVault snapshot backup tool"};

    std::string repository_path;
    bool verbose = false;

    app.add_option("--repo", repository_path, "Repository path");
    app.add_flag("--verbose", verbose, "Enable detailed diagnostics");

    // Add subcommands and bind option structures here.

    try {
        CLI11_PARSE(app, argc, argv);
        // Dispatch selected subcommand.
        return 0;
    } catch (const localvault::LocalVaultError& error) {
        std::cerr << "localvault: " << error.what() << '\n';
        return map_error_to_exit_code(error.code());
    } catch (const std::exception& error) {
        std::cerr << "localvault: unexpected error: " << error.what() << '\n';
        return 4;
    }
}
```

`CLI11_PARSE` may return from `main` internally for parse errors. If centralized mapping is needed, use CLI11's explicit parse/error handling API instead of the convenience macro.

## 42.11 Conflict resolver usage rules

`ConflictDecision`, `ConflictResolver`, and the `conflict_resolver` field are defined once in Section 12.6; do not redeclare them elsewhere.

Rules:

- Require a non-empty resolver when policy is `prompt`; reject the request otherwise.
- The core invokes the resolver synchronously from the restore thread.
- GUI adapters must marshal the question safely to the GUI thread and block the restore worker (not the GUI) while waiting.
- Support "apply to all" by letting the adapter cache its own decision; the core asks per conflict.
- `cancel` behaves exactly like cooperative cancellation (Section 24.6).

## 42.12 Recommended implementation order inside each component

For every module:

1. Define public behavior and invariants.
2. Write failing unit tests.
3. Implement the smallest correct synchronous version.
4. Add integration tests.
5. Add error context.
6. Add cancellation.
7. Add concurrency only when synchronous correctness is established.
8. Benchmark.
9. Optimize measured bottlenecks.
10. Update documentation and compatibility tests.

## 42.13 Platform lock interface

`src/core/filesystem/platform/platform_lock.hpp`:

```cpp
class RepositoryLock final {
public:
    // Throws LocalVaultError(repository_busy) when another process holds it.
    static RepositoryLock acquire_exclusive(const std::filesystem::path& lock_file);

    ~RepositoryLock();
    RepositoryLock(RepositoryLock&&) noexcept;
    RepositoryLock& operator=(RepositoryLock&&) noexcept;

    RepositoryLock(const RepositoryLock&) = delete;
    RepositoryLock& operator=(const RepositoryLock&) = delete;

private:
    struct Impl;                 // POSIX: fd + flock; Win32: HANDLE + LockFileEx
    std::unique_ptr<Impl> impl_;
};
```

Exactly one of `posix_lock.cpp` / `win32_lock.cpp` is compiled per platform (CMake source selection, Section 10.7 — no `#ifdef` bodies). Both implementations must pass the same lock-contention integration test.

---

End of technical implementation guide.
