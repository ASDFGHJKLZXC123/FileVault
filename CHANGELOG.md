# Changelog

## Unreleased — M7 CLI

- Exposed all eleven core commands, stable exit codes, schema-versioned JSON, progress,
  terminal/scripted prompts, and cooperative/forced interrupt handling.
- Added streamed whole-file verification, paged snapshot warnings, and per-snapshot statistics.
- Added non-writing maintenance views and read-only lock access for verification/GC preview,
  while preserving concurrent ordinary queries.
- Honored explicit restore `--no-final-hash` while retaining mandatory chunk and path checks.
- Added cross-platform black-box CLI acceptance tests and native Windows human-check instructions.

## 0.1.0 — Scaffold

- Initial build scaffolding completed for milestone 0.
- Added vcpkg manifest and pinned dependency setup.
- Added CMake root configuration, presets, and module files.
- Added empty core library target, CLI `--version` command, empty Qt shell window, and one GoogleTest.
- Added tri-platform CI workflow for configure/build/test.
- Fixed CI: replaced the removed `x-gha` vcpkg binary cache with `actions/cache` + the `files` provider, and filled Linux/macOS system-package gaps for autotools-based vcpkg ports.
