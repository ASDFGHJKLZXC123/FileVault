# Milestone 0 — Repository scaffold

> Part of the LocalVault build plan ([index](../00-INDEX.md)). Spec source: guide §37.
> **Have these open — the complete reading set for this milestone:**
> [Part 02](../02-toolchain-and-build-system.md) (§5–10: tools, layout, bootstrap, all build files verbatim) ·
> [Part 09](../09-quality-testing-ci-packaging.md) (§35: CI jobs, workflow template, caching).
> Nothing else is required; no other milestone file is ever needed.

## Role in the overall project

M0 builds the factory before the product: toolchain, dependency pinning, build system, and a CI pipeline that compiles and tests on Linux, macOS, and Windows from the very first commit. Every later milestone assumes this exists — no other milestone re-visits build infrastructure.

For your workflow it has one extra job: **CI is your Linux and Windows machine.** You develop on the Mac; the tri-platform CI set up here is what lets you write Win32/Linux-touching code locally and still catch breakage within minutes of a push, without opening a VM.

## Why it matters

- A pinned vcpkg baseline makes builds reproducible; without it, "works today, breaks next month" is guaranteed with a dependency set this heavy (Qt).
- CI binary caching is set up here because it changes the economics of everything after: with it, CI runs take minutes; without it, every push rebuilds Qt (~1 hour × 3 platforms).
- Proving the empty skeleton builds on all three platforms now means later failures are always *your code*, never the environment — a huge debugging advantage.

## How it works

1. Fill each committed placeholder file with its content from §9–§10 (vcpkg manifest, root CMakeLists, presets, warning/sanitizer/analysis modules, per-target CMakeLists, `.clang-format`, `.clang-tidy`).
2. Bootstrap vcpkg per §8.2, **write the §9.1 manifest first, then** run the baseline command (§8.3).
3. Create the four minimal targets: empty `localvault_core`, CLI printing `--version`, empty Qt window, one GoogleTest test.
4. Build/test locally on the Mac (`development` preset), then push; the workflow from §35.3 proves Linux and Windows.

## Specification (verbatim from §37)

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

## Likely problems and confusions — with answers

1. **"vcpkg x-update-baseline fails."** You ran it against the empty placeholder `vcpkg.json`. Write the §9.1 manifest content into the file first; the ordering fix is deliberate (§8.3).
2. **"The first configure has been running for an hour — is it hung?"** No. vcpkg is compiling Qt from source on first run. This happens once per machine; later configures reuse the local binary cache. Same on CI, which is why the `x-gha` cache env vars in the workflow are non-negotiable — verify the second CI run is fast (~minutes) before calling M0 done.
3. **"`cmake --preset development` can't find the toolchain."** `VCPKG_ROOT` isn't set in that shell. Export it (§6) and persist it in your shell profile.
4. **"Presets file rejected."** Presets need CMake ≥ 3.24 (`brew install cmake` on the Mac; the CI images are fine).
5. **"Linux CI fails while my Mac build is green."** Almost always the qtbase system prerequisites — the workflow's `apt-get` list (§35.3) must not be trimmed. This is the expected shape of Mac-primary development: read the CI log, fix, push again. Do not debug Linux issues by guessing locally.
6. **"Windows CI: 'no CMAKE_BUILD_TYPE set' or Ninja errors."** Use the `windows-development` presets — they use the Visual Studio generator (multi-config, no Ninja, no developer prompt). Never point the Ninja-based `development` preset at Windows.
7. **"Do I need a Windows/Linux VM for this milestone?"** No. CI green on all three platforms is the acceptance check. VMs only become useful from M1 onward for hands-on verification.
8. **Scope temptation:** do not write any storage code here, even "just the Repository class." M0 ends with an empty core library on purpose — infrastructure problems and code problems must never be debugged at the same time.

## Completion checklist

M0 is complete only when **every** box is checked. Copy this checklist into the verification log and check items there with evidence (command output, CI run links).

**Implementation**

- [ ] Directory tree matches §7 exactly (`cmake/`, `include/localvault/`, `src/{core,cli,desktop}`, `tests/{unit,integration,cli,support}`, `benchmarks/`, `docs/`, `scripts/`, `.github/workflows/`).
- [ ] `vcpkg.json` contains the §9.1 manifest with a real committed `builtin-baseline` — no placeholder string anywhere in the file.
- [ ] Root `CMakeLists.txt` matches §10.1: all six `LOCALVAULT_*` options, `version.hpp` generation, module includes, conditional GUI/tests/benchmarks.
- [ ] `CMakePresets.json` defines `development`, `release`, `sanitizers`, and the `windows-*` presets (§10.2).
- [ ] `cmake/Dependencies.cmake`, `CompilerWarnings.cmake`, `Sanitizers.cmake`, `StaticAnalysis.cmake` present per §10.3–10.6.
- [ ] `.clang-format` (§10.13) and `.clang-tidy` (§10.14) populated; `scripts/format.sh` and `check-format.sh` run clean on the skeleton.
- [ ] `localvault_core` builds as an (empty) library with the warning set applied.
- [ ] `localvault --version` prints the version that comes from the generated `version.hpp` (change `project(VERSION)` and confirm the output follows).
- [ ] `localvault_desktop` opens an empty Qt main window and closes cleanly.
- [ ] One GoogleTest test exists and `ctest` discovers and runs it.
- [ ] CI workflow has all three jobs (linux/macos/windows), pinned vcpkg commit, binary-cache env vars, Linux Qt system packages, and `-DLOCALVAULT_WARNINGS_AS_ERRORS=ON` on every configure (§35.3).
- [ ] Every `PINNED_*` placeholder in the workflow is replaced with a real version or commit SHA.

**Verification**

- [ ] Mac local: `cmake --preset development && cmake --build --preset development && ctest --preset development` — zero failures.
- [ ] Fresh-clone test: clone into a brand-new directory and reach green tests using only the documented commands (no leftover state from your working copy).
- [ ] All three CI jobs green on the same commit.
- [ ] A second CI run on a trivial commit is fast (minutes, not ~an hour) — proof the vcpkg binary cache works.
- [ ] Zero compiler warnings from project code in any CI log.

**Process**

- [ ] Implementation log written: `docs/implementation-logs/M0/…-claude-code-….md`.
- [ ] Independent verification log written: `docs/implementation-logs/M0/…-verifier-….md`, including this checklist's state.
- [ ] Log records: no functional requirements closed (M0 is infrastructure); any deviations from §7–§10 documented with reasons.
