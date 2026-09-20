# M9 Orchestrator — Quality, performance, and packaging

Spec: [../milestones/M9-quality-performance-packaging.md](../milestones/M9-quality-performance-packaging.md) · Loop: [00-COMMON.md](00-COMMON.md)
Reading set: Part 02 (§10.14) · 07 (§25.12) · 08 (§30.3) · 09 (§32–36) · 10 (§38–39, §41.10)

## Fix first (orchestrator)

- Ratio inverts: little new code, mostly verification. The loop's verify step *is* the milestone. Exit = every §38 box true with evidence.
- Heaviest human involvement of any milestone — schedule Richard's items (benchmarks, clean-machine installs, signing decision) early, not at the end.

## Packets

| # | Packet | Scope | Order | Class |
|---|---|---|---|---|
| A | Test-gap audit | Explore agent: name-by-name audit vs §32.2/32.3 lists (not from memory); then fill gaps; §32.4 property tests with printed seeds | first, parallel | tricky |
| B | Portability fixtures | CI artifacts: repo created on each OS → `verify --full` + restore + byte-compare on the other two (§41.10: small, deterministic) | parallel | tricky |
| C | Analysis gates | clang-format clean; clang-tidy fix-by-category, promote to error once clean (§10.14); ASan/UBSan full suite; TSan run | parallel | tricky |
| D | Benchmark harness | five §33.2 profiles deterministic (§33.3); §33.4 JSON + §33.5 environment capture; results under `benchmarks/results/` | parallel | standard |
| E | Packaging | install rules + CPack (three targets); `macdeployqt`/`windeployqt`; version single-sourced (§30.3) | parallel | tricky |
| F | Docs + release | user guide, per-platform notes, §39 limitations, `CHANGELOG.md`; tag `v0.1.0`, release workflow, §36.4 checklist | last | mechanical |

## Watchpoints (put in briefs)

- **Never publish VM/sandbox benchmark numbers** — Mac host only; profile A being fsync-bound is expected.
- vcpkg-Qt + `macdeployqt` is fiddly: run on the `cmake --install` output, check `PlugIns/platforms/`, `otool -L` for stray vcpkg paths.
- Version bump = one edit (`project(VERSION)`); if it's three edits, fix the wiring first.
- Portability fixtures: `upload-artifact`/`download-artifact` between jobs; no big binaries in git.
- Signing: unsigned-with-documented-workarounds or certificates — either is valid, undocumented is not.

## Verify

- Fresh agent: this checklist **and** §38 master checklist → `docs/implementation-logs/M9/`; every box with an evidence link; FR-001–503 mapped to closing milestone + proof.
- Richard: benchmark runs on Mac host; clean-environment install test per platform (fresh macOS account, snapshot-reverted Windows/Linux VMs); code-signing decision; final release-checklist walk (§36.4).
