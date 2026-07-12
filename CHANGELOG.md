# Changelog

## 0.1.0 — Scaffold

- Initial build scaffolding completed for milestone 0.
- Added vcpkg manifest and pinned dependency setup.
- Added CMake root configuration, presets, and module files.
- Added empty core library target, CLI `--version` command, empty Qt shell window, and one GoogleTest.
- Added tri-platform CI workflow for configure/build/test.
- Fixed CI: replaced the removed `x-gha` vcpkg binary cache with `actions/cache` + the `files` provider, and filled Linux/macOS system-package gaps for autotools-based vcpkg ports.
