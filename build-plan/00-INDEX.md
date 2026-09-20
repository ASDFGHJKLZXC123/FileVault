# LocalVault Build Plan — Index

This folder is the working copy of the **LocalVault Technical Implementation Guide, Revision 2**, split into parts small enough to follow during implementation. The content is copied verbatim from the single-file guide: top-level section numbers (1–42) are unchanged, so any cross-reference like "Section 25.10" is still valid — use the lookup table below to find which part holds a section.

Revision 2 adds Windows 10/11 as a first-release platform and resolves the planning-review findings (locking rules, batched deletion, settings storage, bootstrap ordering, CI hardening). This folder is tracked in Git and published on the `Latest-Dev` branch.

The original single files (`../LocalVault_Technical_Implementation_Guide.md` and `.ORIGINAL.md`) are frozen local archives and are not included in the repository. Make any future plan edits **here**, not there.

---

## Parts

| Part | File | Sections | Contents |
|---|---|---|---|
| 01 | [01-scope-and-requirements.md](01-scope-and-requirements.md) | 1–4 | Project definition, functional requirements (FR-xxx), non-functional requirements, scope and non-goals |
| 02 | [02-toolchain-and-build-system.md](02-toolchain-and-build-system.md) | 5–10 | Technology stack, environments, repository layout, bootstrap steps, vcpkg, all CMake files and presets |
| 03 | [03-architecture-and-public-api.md](03-architecture-and-public-api.md) | 11–12 | Layered architecture, dependency rules, core components, every public header (`types`, `error`, `progress`, `Repository`, engines, queries) |
| 04 | [04-repository-format-and-database.md](04-repository-format-and-database.md) | 13–15 | On-disk repository format, object paths, full SQLite schema, migrations, repository initialization |
| 05 | [05-snapshot-engine.md](05-snapshot-engine.md) | 16–18 | Snapshot algorithm, stable-file checks, incremental reuse, chunking, hashing, compression, dedup, object writes |
| 06 | [06-restore-diff-verify-gc.md](06-restore-diff-verify-gc.md) | 19–22 | Restore algorithm and conflict policies, snapshot diff, integrity verification, deletion and garbage collection |
| 07 | [07-consistency-concurrency-platform.md](07-consistency-concurrency-platform.md) | 23–27 | Crash consistency, durability, locking rule, worker pipeline, cancellation, filesystem/platform policy matrix, Windows specifics, portability, restore security, ignore rules |
| 08 | [08-interfaces-cli-gui-config.md](08-interfaces-cli-gui-config.md) | 28–31 | CLI commands and exit codes, Qt desktop application, configuration, logging and error handling |
| 09 | [09-quality-testing-ci-packaging.md](09-quality-testing-ci-packaging.md) | 32–36 | Testing strategy (unit/integration/e2e), benchmarks, sanitizers, CI workflows, packaging and release |
| 10 | [10-milestones-and-definition-of-done.md](10-milestones-and-definition-of-done.md) | 37–41 | Milestone overview and shared rules (full per-milestone specs live in [milestones/](milestones/)), definition of done, known limitations, future work, build/dependency maintenance |
| 11 | [11-appendix-skeletons.md](11-appendix-skeletons.md) | 42 | Implementation skeletons: wrappers, bounded queue, pseudocode, stats SQL, platform lock |

## Section → part lookup

| Sections | Part | | Sections | Part |
|---|---|---|---|---|
| 1–4 | 01 | | 23–27 | 07 |
| 5–10 | 02 | | 28–31 | 08 |
| 11–12 | 03 | | 32–36 | 09 |
| 13–15 | 04 | | 37–41 | 10 |
| 16–18 | 05 | | 42 | 11 |
| 19–22 | 06 | | | |

## Milestone reading map

Each milestone has its own file under [milestones/](milestones/) containing its role in the project, why it matters, how it works, the verbatim spec, likely problems with answers, and a **Completion checklist**. The checklist is the definition of done for that milestone: it is complete when — and only when — every box is checked (with evidence in the verification log). Start every milestone by opening its file; keep Part 01 (requirements) at hand; §38 (Part 10) remains the release-level master checklist that M9 closes.

| Milestone | Milestone file | Complete reading set (parts) | Key sections |
|---|---|---|---|
| M0 — Repository scaffold | [M0](milestones/M0-repository-scaffold.md) | 02, 09 | 5–10, 35 |
| M1 — Database and repository lifecycle | [M1](milestones/M1-database-and-repository-lifecycle.md) | 02, 03, 04, 07, 11 | 12.4, 13–15, 23.5–23.6, 42.3/42.13 |
| M2 — Minimal whole-file snapshot/restore | [M2](milestones/M2-whole-file-snapshot-restore.md) | 03, 04, 05, 06, 07, 09, 11 | 11.3, 14.4, 16, 19, 25.1–25.6, 32.1 |
| M3 — Chunks, BLAKE3, zstd | [M3](milestones/M3-chunking-blake3-zstd.md) | 04, 05, 06, 07, 09, 11 | 13, 18, 19.3, 26.5, 32.2, 42.1–42.6 |
| M4 — Crash-safe publication | [M4](milestones/M4-crash-safe-publication.md) | 03, 05, 06, 07, 08, 09 | 18.6, 22.1, 23, 31.5, 32.5 |
| M5 — Concurrency and cancellation | [M5](milestones/M5-concurrency-and-cancellation.md) | 03, 05, 07, 09, 11 | 12.3, 16, 24, 25.4/25.11, 42.4 |
| M6 — Diff, verify, delete, GC | [M6](milestones/M6-diff-verify-delete-gc.md) | 04, 05, 06, 07, 09, 11 | 14.5, 18.8, 20–22, 23.4, 42.9 |
| M7 — Complete CLI | [M7](milestones/M7-complete-cli.md) | 08, 09, 11 | 28, 31, 32.7, 42.10 |
| M8 — Qt desktop application | [M8](milestones/M8-qt-desktop-application.md) | 03, 07, 08, 11 | 12.3/12.6, 29, 30, 42.11 |
| M9 — Quality, performance, packaging | [M9](milestones/M9-quality-performance-packaging.md) | 02, 07, 08, 09, 10 | 30.3, 32–36, 38–41 |

## Development workflow

Primary development happens on **macOS**. Linux and Windows are verified through CI on every push and through VM sessions for hands-on checks CI cannot cover — each milestone file says when a VM session is worth the time. A milestone still completes only when all three platforms are green (shared rules in Part 10 §37). Performance benchmarks run only on the Mac host; VM numbers are never published.

Workflow reminder (applies to every milestone): implementation + verification logs under `docs/implementation-logs/<milestone>/`.
