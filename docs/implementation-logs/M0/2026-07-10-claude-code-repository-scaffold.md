# Milestone 0 Implementation Log (Repository Scaffold)

Date: 2026-07-10 (build/format/commit fixes applied 2026-07-12)
Milestone source: `build-plan/milestones/M0-repository-scaffold.md`
Reference plan sections: `build-plan/02-toolchain-and-build-system.md` (§5–10),
`build-plan/09-quality-testing-ci-packaging.md` (§35)

## Scope

This milestone establishes the repository scaffold: build toolchain, pinned dependency
manifest, CMake + preset configuration, target skeletons, and tri-platform CI. It
intentionally does **not** include production storage logic or feature code beyond
empty/minimal entry points (no functional requirements are closed by M0).

## What was implemented

- Root bootstrap files: `.clang-format`, `.clang-tidy`, `.gitignore`, `LICENSE`,
  `README.md`, `CHANGELOG.md`, `CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json`
  (pinned `builtin-baseline` `03e366fb91e38b9432ebd5f8cc79f7c8f55e96ab`).
- `cmake/{Dependencies,CompilerWarnings,Sanitizers,StaticAnalysis}.cmake`, wired into
  the root `CMakeLists.txt` with all six `LOCALVAULT_*` options and generated
  `version.hpp`.
- Targets: `localvault_core` (empty library), `localvault` CLI (`--version` /
  `--version-json`), `localvault_desktop` (empty `QMainWindow`), `localvault_tests`
  (one GoogleTest test), `localvault_benchmarks` skeleton.
- Directory scaffolding: `tests/{unit,integration,cli,support}` (the latter three as
  empty dirs via `.gitkeep`, populated by later milestones), `scripts/format.sh` and
  `scripts/check-format.sh`.
- CI workflow `.github/workflows/build-test.yml`: linux/macos/windows jobs, pinned
  vcpkg commit, `x-gha` binary-cache env vars, Linux Qt system packages,
  `-DLOCALVAULT_WARNINGS_AS_ERRORS=ON` on every configure.

## Deviations from §7–§10

None. Root files, presets, and cmake modules match the spec verbatim. The full §7
tree (per-component `.cpp`/`.hpp` files, desktop controllers/models/pages,
`benchmarks/generate_dataset.py`, `docs/*.md`, `.github/dependabot.yml`,
static-analysis/release workflows) is the end state for the whole project, not an M0
deliverable — M0's own scope note says explicitly not to write storage code yet;
those files are scoped to M1, M3, M8, and M9 respectively.

## Issues found and fixed during verification (2026-07-12)

1. **Format scripts hardcoded an absolute path.** `scripts/format.sh` and
   `scripts/check-format.sh` used `git -C "<literal absolute path>"`, which only
   worked on the original machine/location and would break (or silently check the
   wrong tree) from any other clone. Fixed to resolve the repo root relative to the
   script's own location (`$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)`).
2. **Real formatting violations in 3 skeleton files**, previously hidden because the
   scripts only check `git ls-files`-tracked files (none were tracked yet):
   `src/core/core.cpp`, `src/desktop/main_window.hpp`, `src/desktop/main_window.cpp`.
   Fixed by running `clang-format -i` with the committed `.clang-format` style;
   verified clean via direct `clang-format --dry-run --Werror` against every
   `.cpp`/`.hpp` in the tree.
3. **Local build never completed.** Root cause: `VCPKG_INSTALLED_DIR` defaults to
   `${sourceDir}/build/<preset>/vcpkg_installed`, and the repo lived under
   `/Users/f8fq/coding projects/Unfinished/LocalVault` — a path containing a space,
   which broke `libb2`'s autotools build. `VCPKG_ROOT` itself was already at a
   space-free path, so reinstalling vcpkg elsewhere would not have fixed this. Fixed
   by relocating the repository (working tree and git history, no repo content
   changed) to `/Users/f8fq/dev/LocalVault`.

## Verification after fixes (evidence)

```
$ cmake --preset development   # VCPKG_ROOT=$HOME/tools/vcpkg
... 13/13 build steps succeeded, exit 0

$ ./build/development/src/cli/localvault --version
0.1.0                          # matches project(VERSION 0.1.0) in CMakeLists.txt

$ ctest --preset development --output-on-failure
1/1 Test #1: Version.LocalVaultVersionIsDefined ...   Passed    0.01 sec
100% tests passed out of 1

$ ./build/development/src/desktop/localvault_desktop.app/Contents/MacOS/localvault_desktop
# launched, stayed alive (event loop running), exited 143 (clean SIGTERM) — no crash/hang

Build log: only warning is a benign `ranlib: ... has no symbols` notice on the
intentionally-empty src/core/core.cpp; zero compiler warnings from project code.
```

## Known remaining gaps (not closed by this log)

- CI has not yet been exercised: nothing was pushed to `origin` prior to this commit.
  Tri-platform CI green, second-run cache-speed proof, and CI-log warning check
  (checklist rows 15–17) require a push and are out of scope for this log.
- Visual confirmation that the desktop window renders on screen (vs. process-level
  launch/exit evidence) was not captured via screenshot.
