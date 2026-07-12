# Milestone 0 — Independent Verification Log (Repository Scaffold)

Date: 2026-07-12
Verifier: independent verification pass (no memory of prior implementation session; every
row below was re-derived from the current committed state of the repo, plus commands run
during this session).
Repo: `/Users/f8fq/dev/LocalVault` (relocated from a path containing a space; this is a known,
expected, already-documented relocation — not a defect).
Milestone source: `build-plan/milestones/M0-repository-scaffold.md`
Reference plan sections: `build-plan/02-toolchain-and-build-system.md` (§5–10),
`build-plan/09-quality-testing-ci-packaging.md` (§35)

## Functional scope statement

**M0 closes zero functional requirements.** It is infrastructure only: toolchain, pinned
dependency manifest, CMake/preset configuration, four minimal target skeletons (empty
`localvault_core`, `localvault --version` CLI, empty `localvault_desktop` Qt window, one
GoogleTest test), and a tri-platform CI workflow. No storage, snapshot, restore, or repository
logic exists yet, by design (see M0 doc, "Likely problems and confusions" item 8).

## Completion checklist — verdicts and evidence

### Implementation

1. **Directory tree matches §7** (`cmake/`, `include/localvault/`, `src/{core,cli,desktop}`,
   `tests/{unit,integration,cli,support}`, `benchmarks/`, `docs/`, `scripts/`,
   `.github/workflows/`) — **PASS**. All top-level directories named in the checklist exist and
   are tracked (`git ls-files` confirms tracked content in each; `tests/integration`,
   `tests/cli`, `tests/support` intentionally contain only `.gitkeep`, correct for M0).

2. **`vcpkg.json` contains the §9.1 manifest with a real committed `builtin-baseline`, no
   placeholder** — **PASS**. Manifest name/version/dependency list matches §9.1 exactly.
   `builtin-baseline` is `03e366fb91e38b9432ebd5f8cc79f7c8f55e96ab`, a real commit present in
   the local vcpkg clone (`git cat-file -e` against `$HOME/tools/vcpkg` succeeds). `grep -c
   "REPLACE_WITH" vcpkg.json` → 0.

3. **Root `CMakeLists.txt` matches §10.1**: all six `LOCALVAULT_*` options, `version.hpp`
   generation, module includes, conditional GUI/tests/benchmarks — **PASS**. Diffed by eye
   against the spec block; content is byte-for-byte identical (options, `CMAKE_CXX_STANDARD
   20`, `configure_file(... version.hpp.in ...)`, `list(APPEND CMAKE_MODULE_PATH ...)`, the four
   `include(...)` module lines, and the `add_subdirectory`/conditional blocks).

4. **`CMakePresets.json` defines `development`, `release`, `sanitizers`, and the `windows-*`
   presets (§10.2)** — **PASS**. All four configure presets present plus matching build/test
   presets, including `windows-development-debug`/`-release` using the Visual Studio 17 2022
   generator (multi-config, no Ninja) gated by a `hostSystemName == Windows` condition.

5. **`cmake/Dependencies.cmake`, `CompilerWarnings.cmake`, `Sanitizers.cmake`,
   `StaticAnalysis.cmake` present per §10.3–10.6** — **PASS**. All four files exist and are
   byte-for-byte identical to the spec blocks (verified by direct read/comparison).

6. **`.clang-format` (§10.13) and `.clang-tidy` (§10.14) populated; `scripts/format.sh` and
   `check-format.sh` run clean on the skeleton** — **PASS**. Both config files match the spec
   verbatim. Both scripts resolve `REPO_ROOT` via
   `$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)` (not a hardcoded path); ran
   `check-format.sh` from `cwd=/tmp` (a different directory than the repo) and it correctly
   found and checked the repo's tracked `.cpp`/`.hpp` files, exit 0.

7. **`localvault_core` builds as an (empty) library with the warning set applied** — **PASS**.
   `src/core/CMakeLists.txt` calls `localvault_set_warnings`/`localvault_enable_sanitizers`/
   `localvault_enable_clang_tidy` on the target; a full rebuild with
   `-DLOCALVAULT_WARNINGS_AS_ERRORS=ON` succeeded with zero warnings from `core.cpp` (empty
   namespace body).

8. **`localvault --version` prints the version from generated `version.hpp`; change
   `project(VERSION)` and confirm the output follows** — **PASS**. `project(LocalVault VERSION
   0.1.0 ...)` in root `CMakeLists.txt`; `./build/development/src/cli/localvault --version`
   printed `0.1.0`; `--version-json` printed `{"version":"0.1.0"}`. `version.hpp.in` uses
   `@PROJECT_VERSION@` substitution, so the printed value is mechanically tied to
   `project(VERSION)`, not hardcoded.

9. **`localvault_desktop` opens an empty Qt main window and closes cleanly** — **PASS**.
   Launched `build/development/src/desktop/localvault_desktop.app/Contents/MacOS/
   localvault_desktop` directly (native `cocoa` platform — the vcpkg-built qtbase on this
   machine does not expose an `offscreen` QPA plugin, see Deviations). Process stayed alive
   (event loop running) for the observation window, was sent `SIGTERM`, and exited cleanly
   with no crash/hang. `MainWindow` sets a title and geometry only — no other behavior, as
   expected for M0.

10. **One GoogleTest test exists and `ctest` discovers and runs it** — **PASS**.
    `tests/unit/version_test.cpp` has one `TEST(Version, LocalVaultVersionIsDefined)`;
    `tests/CMakeLists.txt` uses `gtest_discover_tests`; `ctest --preset development
    --output-on-failure` found and ran `Version.LocalVaultVersionIsDefined`, 1/1 passed.

11. **CI workflow has all three jobs (linux/macos/windows), pinned vcpkg commit,
    binary-cache env vars, Linux Qt system packages, and
    `-DLOCALVAULT_WARNINGS_AS_ERRORS=ON` on every configure (§35.3)** — **PASS** (structural
    check only; no CI run exists — see Verification section). `.github/workflows/build-test.yml`
    has `linux`/`macos`/`windows` jobs; `VCPKG_COMMIT` env var matches `vcpkg.json`'s
    `builtin-baseline` (`03e366fb91e38b9432ebd5f8cc79f7c8f55e96ab`); `VCPKG_BINARY_SOURCES:
    "clear;x-gha,readwrite"` plus the `actions/github-script` step exporting
    `ACTIONS_CACHE_URL`/`ACTIONS_RUNTIME_TOKEN` is present in all three jobs; the Linux job
    installs the full `apt-get` Qt/X11/xcb prerequisite list from §35.3 verbatim; all three
    `Configure` steps pass `-DLOCALVAULT_WARNINGS_AS_ERRORS=ON`.

12. **Every `PINNED_*` placeholder in the workflow is replaced with a real version or commit
    SHA** — **PASS**. `grep -c "PINNED_" .github/workflows/build-test.yml` → 0. Actions are
    pinned to major-version tags (`actions/checkout@v4`, `actions/github-script@v7`) rather
    than full commit SHAs — acceptable under §35.2's "where practical" wording, but a stricter
    reading of §35.2 would prefer SHA pinning for these two actions (noted in Deviations).

### Verification

13. **Mac local: `cmake --preset development && cmake --build --preset development && ctest
    --preset development` — zero failures** — **PASS**. Ran all three commands in this repo
    (reusing the existing `build/development` tree) with `VCPKG_ROOT="$HOME/tools/vcpkg"`:
    configure succeeded (only benign CMake "author" warnings about the deprecated
    `SQLite::SQLite3` target name — a CMake package-config warning inherent to the vcpkg SQLite3
    port, not a deviation from the spec, which itself specifies `find_package(SQLite3
    REQUIRED)` / `SQLite::SQLite3` verbatim in §10.3/§10.7); build reported `ninja: no work to
    do` (already up to date, exit 0); `ctest --preset development --output-on-failure` → 1/1
    tests passed. Separately did a full clean rebuild with
    `-DLOCALVAULT_WARNINGS_AS_ERRORS=ON` into a scratch build dir to positively confirm
    zero-warning compilation (see item 17), then removed the scratch dir.

14. **Fresh-clone test: clone into a brand-new directory and reach green tests using only the
    documented commands** — **PASS**. `/tmp/localvault-freshclone` exists with logs at
    `/tmp/freshclone-configure.log`, `/tmp/freshclone-build.log`, `/tmp/freshclone-ctest.log`
    (all dated today, 2026-07-12). Configure log ends with "Build files have been written to:
    .../build/development" (only the same benign SQLite3 author warnings, no errors). Build log
    shows all 13/13 steps succeeding (core lib, CLI, tests, benchmarks, desktop app all built;
    the same benign `ranlib: ... has no symbols` notice on empty `core.cpp`). ctest log shows
    `Version.LocalVaultVersionIsDefined` passed, 100% tests passed.

15. **All three CI jobs green on the same commit** — **UNVERIFIABLE**. No GitHub/`gh` access in
    this session, and `git status` shows the repo is 1 commit ahead of `origin/main` — nothing
    has been pushed, so no CI run exists to inspect. Structural review of the workflow (item 11)
    is the only available partial evidence.

16. **A second CI run on a trivial commit is fast (minutes, not ~an hour) — proof the vcpkg
    binary cache works** — **UNVERIFIABLE**. Same reason as item 15: no CI runs exist yet
    (nothing pushed). The workflow does correctly wire `x-gha` binary caching (item 11), which
    is the necessary precondition, but cache-hit speed can only be confirmed once two runs exist
    on GitHub Actions.

17. **Zero compiler warnings from project code in any CI log** — **UNVERIFIABLE** for the actual
    CI log (none exists yet). As partial/local evidence: a clean rebuild in this repo with
    `-DLOCALVAULT_WARNINGS_AS_ERRORS=ON` (matching what CI passes on every configure) produced
    zero `warning:` lines from `src/` or `include/` — the only two matches for `warning:` in the
    full build log are the benign `ranlib: 'liblocalvault_core.a(core.cpp.o)' has no symbols`
    notices, which are a linker/archiver informational message about the intentionally-empty
    `src/core/core.cpp`, not a compiler warning, and are expected at this stage.

### Process

18. **Implementation log written**: `docs/implementation-logs/M0/…-claude-code-….md` —
    **PASS**. Present at
    `docs/implementation-logs/M0/2026-07-10-claude-code-repository-scaffold.md`, tracked in git.
    It documents scope, what was implemented, a "Deviations from §7–§10: None" section, three
    issues found/fixed during a prior verification pass (hardcoded path in format scripts, real
    clang-format violations in 3 files, and the space-in-path build failure that motivated the
    repo relocation), and verification evidence.

19. **Independent verification log written**: `docs/implementation-logs/M0/…-verifier-….md`,
    including this checklist's state — **PASS**. This document, at
    `docs/implementation-logs/M0/2026-07-12-verifier-repository-scaffold.md`.

20. **Log records: no functional requirements closed (M0 is infrastructure); any deviations
    from §7–§10 documented with reasons** — **PASS**. See "Functional scope statement" above
    and "Deviations from §7–§10" below.

## Deviations from §7–§10

Real deviations found during this independent pass (all minor, none blocking):

1. **`benchmarks/CMakeLists.txt` omits the three `localvault_set_warnings` /
   `localvault_enable_sanitizers` / `localvault_enable_clang_tidy` calls** that §10.11 shows for
   the `localvault_benchmarks` target. The other four targets (`localvault_core`, `localvault`,
   `localvault_desktop`, `localvault_tests`) all call these three functions; the benchmark
   target does not. Not required by the M0 checklist (which does not mention benchmarks), but
   worth fixing before benchmarks become load-bearing (M8-ish).
2. **`.gitignore` has extra entries beyond §10.12**: `/build-plan/` and the two
   `LocalVault_Technical_Implementation_Guide*.md` planning-doc filenames, reflecting a
   deliberate decision (see commit `45eab04`, "Remove planning docs from the published repo")
   to keep the build-plan/reference material local and unpublished. This is an addition, not a
   removal of anything §10.12 requires, and does not affect any checklist row.
3. **CI workflow bootstraps vcpkg into `${{ github.workspace }}/.tmp/vcpkg`
   (linux/macos) / `${{ github.workspace }}\vcpkg` (windows) with `VCPKG_ROOT` set via a step
   `env:` block**, rather than the §35.3 template's `$HOME/vcpkg` /
   `$env:USERPROFILE\vcpkg`. Functionally equivalent (still a fresh clone at the pinned commit,
   still exported to `GITHUB_ENV`/`GITHUB_PATH`), does not affect caching correctness.
4. **GitHub Actions pinned to major-version tags** (`actions/checkout@v4`,
   `actions/github-script@v7`) rather than full commit SHAs. Permitted by §35.2's "where
   practical" wording but not the strictest reading of that section.
5. **Observation (not a spec deviation): the locally-installed vcpkg `qtbase` build does not
   expose an `offscreen` QPA platform plugin** on this Mac (only `cocoa` is available; attempting
   `QT_QPA_PLATFORM=offscreen` fails to launch). The macOS CI job sets
   `QT_QPA_PLATFORM: offscreen` for its `ctest` step (matching §35.1's "Run any Qt tests with
   offscreen platform" guidance) — this does not affect M0 today because no test currently
   exercises the desktop binary via ctest (the only test is `Version.LocalVaultVersionIsDefined`),
   but it is worth checking once GUI-touching tests are added in a later milestone, in case the
   CI-side qtbase build has the same gap.

No other deviations from §7–§10 were found; the root bootstrap files, CMake presets, and cmake
modules that were compared verbatim (items 3–6 above) matched the specification exactly.

## Summary

- **PASS: 17** (items 1–14, 18–20)
- **FAIL: 0**
- **UNVERIFIABLE: 3** (items 15–17, all CI-log-dependent)

**What blocks M0 from being called fully complete:** only a push to `origin` and a resulting CI
run. The repo is 1 local commit ahead of `origin/main` with nothing pushed yet, so items 15–17
(all three CI jobs green, second-run cache-speed proof, zero-warnings-in-CI-log) cannot be
verified until that happens. Every other row — including the structural correctness of the
workflow that would produce that CI run — passes. No FAIL rows were found; the five items listed
under Deviations are minor and none of them block closing M0 once CI is exercised.
