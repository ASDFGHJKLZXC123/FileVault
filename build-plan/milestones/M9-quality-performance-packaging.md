# Milestone 9 — Quality, performance, and packaging

> Part of the LocalVault build plan ([index](../00-INDEX.md)). Spec source: guide §37.
> **Have these open — the complete reading set for this milestone:**
> [Part 02](../02-toolchain-and-build-system.md) (§10.14: clang-tidy configuration note) ·
> [Part 07](../07-consistency-concurrency-platform.md) (§25.12: portability rules the fixtures prove) ·
> [Part 08](../08-interfaces-cli-gui-config.md) (§30.3: single-source version wiring) ·
> [Part 09](../09-quality-testing-ci-packaging.md) (§32–36: tests, benchmarks, analysis, CI, packaging — the core of this milestone) ·
> [Part 10](../10-milestones-and-definition-of-done.md) (§38: the checklist you are closing · §39: limitations to document · §41.10: compatibility fixtures).
> Nothing else is required; no other milestone file is ever needed.

## Role in the overall project

M9 turns a working program into a releasable product: the complete test matrix (including cross-platform portability fixtures), static analysis and sanitizer gates, honest benchmarks, install rules, per-platform packages, documentation, and the tagged release. Its exit criterion is not "features done" — that happened in M8 — but **every checkbox in the definition of done (§38) is true.**

## Why it matters

- Packaging is where "works on my machine" dies: a Qt app that launches from the build tree routinely fails on a clean machine (missing plugins, wrong rpaths). Discovering that yourself in M9 is a checklist item; letting a user discover it is a reputation event.
- Benchmarks with recorded environments (§33.5) are the only defensible performance claims — and they set the baseline that future optimizations are measured against.
- The portability fixtures (create on one OS, restore on another) are the proof of the §25.12 promise — the single most cross-platform-sensitive behavior in the product.
- Gatekeeper/SmartScreen behavior (§36.3) is a *decision*, and M9 forces it to be made consciously and documented rather than discovered by users.

## How it works

1. Close every test gap against the §32 lists; wire the portability fixtures through CI artifacts (Linux-created repo verified on macOS/Windows jobs and vice versa).
2. Run clang-tidy clean, sanitizer jobs green, formatting enforced (§34).
3. Generate deterministic datasets (§33.3), run the benchmark matrix on defined hardware, store JSON results with environment details (§33.4–33.5).
4. Install rules + CPack per platform; `macdeployqt` / `windeployqt` bundling; validate each package on a clean environment (§36).
5. Walk the release checklist (§36.4), tag `v0.1.0`, attach artifacts and checksums via the release workflow (§35.1).

## Specification (verbatim from §37)

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

## Likely problems and confusions — with answers

1. **"Where do I get 'clean machines' when I develop on one Mac?"** macOS: a brand-new user account (or a macOS VM) approximates clean well enough for Qt-plugin/rpath issues. Windows/Linux: fresh VM snapshots without dev tools — exactly what your VMs are for. Revert-to-snapshot after each install test so it stays clean.
2. **"macdeployqt from a vcpkg-built Qt misbehaves (missing plugins, rpath errors)."** Common. Check: run it on the `.app` produced by `cmake --install`, confirm `platforms/libqcocoa.dylib` landed in `PlugIns/`, and use `otool -L` on the binary to find stray absolute paths into the vcpkg tree. Budget real time for this — it's fiddly, not hard.
3. **"My unsigned app won't open on macOS / SmartScreen flags it on Windows."** Expected (§36.3). Decide: ship unsigned with documented right-click-Open / "More info → Run anyway" workarounds, or buy certificates (Apple Developer ID + notarization; Authenticode). Either is valid for v0.1 — undocumented is not.
4. **"Are benchmarks in VMs meaningful?"** Only functionally. **Never publish VM numbers** — virtualized I/O distorts exactly what LocalVault stresses (fsync). Perf runs happen on the Mac host, labeled with hardware/OS/filesystem per §33.5; profile A being fsync-bound is expected (§33.2), not a bug to chase.
5. **"clang-tidy explodes with hundreds of findings."** Don't gate on everything at once. Fix by category, promote checks to errors only once clean (§10.14 note) — the §41 rules exist to stop warning-suppression culture.
6. **"How do portability fixtures move between CI jobs?"** `actions/upload-artifact` in the creating job, `download-artifact` in the verifying jobs, then `verify --full` + restore + byte-compare. Keep fixtures small and deterministic (§41.10: no big binaries in git).
7. **"The release workflow needs versions in three places."** Single source: the CMake `project(VERSION)` → generated `version.hpp` (§30.3) → CPack. The checklist's "version updated" means one edit; if it means three, fix the wiring first.
8. **"When am I actually done?"** When §38 is all checked, including the Rev 2 additions: Windows behavior suite, portability round-trip, package-runs-on-clean-machine per platform, and the code-signing line in the release notes. Print the checklist into the M9 verification log and check items there.

## Completion checklist

M9 is complete only when **every** box is checked — and §38 (Part 10) is the master release checklist that must be fully true alongside this one. Copy both into the verification log.

**Test completeness**

- [ ] Every §32.2 unit battery and §32.3 integration list exists and passes — do a name-by-name audit against Part 09, not from memory.
- [ ] Windows-specific behavior suite (§32.3) green on Windows CI: reserved names, drive-relative rejection, junction non-traversal, sharing violation, symlink-privilege fallback, long paths.
- [ ] Portability fixtures wired through CI artifacts: repository created on each OS opens, verifies (full), and restores byte-identically on the other two.
- [ ] Property/invariant tests (§32.4) run with printed deterministic seeds.

**Static analysis & sanitizers**

- [ ] `clang-format` check clean; `clang-tidy` clean under the adopted check set, with promotions to error recorded (§10.14 note, §34.2).
- [ ] ASan/UBSan CI job green over the full suite; TSan-focused run clean.

**Benchmarks (Mac host only — never VM numbers)**

- [ ] All five dataset profiles (§33.2) generate deterministically with manifests (§33.3).
- [ ] Snapshot/restore/verify benchmarks recorded as §33.4 JSON with full environment details (§33.5); warm/cold labeled; results stored under `benchmarks/results/`, not invented into the README.
- [ ] Profile A explicitly reports objects/second (the fsync bound) alongside throughput.

**Packaging & release**

- [ ] Install rules and CPack produce all three packages: `.tar.gz` (Linux), `.zip`/bundle (macOS with `macdeployqt`), `.zip` (Windows with `windeployqt`).
- [ ] Clean-environment test *per platform*: fresh macOS user account, snapshot-reverted Windows VM, snapshot-reverted Linux VM — CLI runs and GUI launches on each; results logged.
- [ ] Code-signing decision made and written into the release notes (unsigned workarounds documented, or signing/notarization done).
- [ ] Version is single-sourced: bumping `project(VERSION)` alone updates `--version`, the GUI about box, and the package names (§30.3).
- [ ] `CHANGELOG.md` updated; repository format compatibility stated; vcpkg baseline committed.
- [ ] `docs/user-guide.md` covers install, quick start, every command, and the per-platform notes (Full Disk Access, Windows symlink privilege, Gatekeeper/SmartScreen workarounds).
- [ ] Tag `v0.1.0` pushed; release workflow attaches artifacts + checksums; release checklist (§36.4) walked item by item.

**Definition of done**

- [ ] Every §38 checkbox is true, with an evidence link (test name, CI run, or log entry) recorded next to each in the verification log.

**Process**

- [ ] Implementation + verification logs under `docs/implementation-logs/M9/`, including both checklists' final state.
- [ ] Log confirms all functional requirements FR-001–FR-503 are mapped to their closing milestone and proof.
