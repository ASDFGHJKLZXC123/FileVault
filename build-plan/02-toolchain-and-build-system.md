# LocalVault Build Plan — Part 02: Toolchain and Build System

> Sections 5–10 of the Technical Implementation Guide (Revision 2), split verbatim.
> Section numbers are unchanged, so cross-references like “Section 25.10”
> resolve via the lookup table in [00-INDEX.md](00-INDEX.md).

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

Workflow note: local development happens on macOS; the Linux and Windows rows are normally satisfied through CI on every push and through VM verification sessions, not local machines (development workflow in Part 10 §37).

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
