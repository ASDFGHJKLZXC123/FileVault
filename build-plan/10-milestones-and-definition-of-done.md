# LocalVault Build Plan — Part 10: Milestones, Definition of Done, and Maintenance

> Sections 37–41 of the Technical Implementation Guide (Revision 2).
> §38–41 are verbatim. §37 is an overview: the full per-milestone specifications,
> with role/importance/how-it-works commentary and pitfall answers, live in one
> file per milestone under [milestones/](milestones/).
> Cross-references like “Section 25.10” resolve via [00-INDEX.md](00-INDEX.md).

---

# 37. Implementation milestones

Complete milestones in order. Do not begin the full GUI before the core snapshot/restore path is tested.

The full specification for each milestone — its role in the overall project, why it matters, how it works, the verbatim implement/tests/acceptance lists, and likely problems with answers — lives in one file per milestone:

| Milestone | File |
|---|---|
| M0 — Repository scaffold | [milestones/M0-repository-scaffold.md](milestones/M0-repository-scaffold.md) |
| M1 — Database and repository lifecycle | [milestones/M1-database-and-repository-lifecycle.md](milestones/M1-database-and-repository-lifecycle.md) |
| M2 — Minimal whole-file snapshot/restore | [milestones/M2-whole-file-snapshot-restore.md](milestones/M2-whole-file-snapshot-restore.md) |
| M3 — Fixed-size chunks, BLAKE3, and zstd | [milestones/M3-chunking-blake3-zstd.md](milestones/M3-chunking-blake3-zstd.md) |
| M4 — Crash-safe object and snapshot publication | [milestones/M4-crash-safe-publication.md](milestones/M4-crash-safe-publication.md) |
| M5 — Concurrency and cancellation | [milestones/M5-concurrency-and-cancellation.md](milestones/M5-concurrency-and-cancellation.md) |
| M6 — Diff, verification, delete, and GC | [milestones/M6-diff-verify-delete-gc.md](milestones/M6-diff-verify-delete-gc.md) |
| M7 — Complete CLI | [milestones/M7-complete-cli.md](milestones/M7-complete-cli.md) |
| M8 — Qt desktop application | [milestones/M8-qt-desktop-application.md](milestones/M8-qt-desktop-application.md) |
| M9 — Quality, performance, and packaging | [milestones/M9-quality-performance-packaging.md](milestones/M9-quality-performance-packaging.md) |

## Shared rules (apply to every milestone)

- **Ordering:** milestones complete strictly in order; no future-milestone work lands early (scope discipline).
- **Tri-platform completion:** a milestone is complete only when it builds and passes its tests on Linux, macOS, and Windows — platform debt is not carried forward.
- **Development workflow:** primary development happens on macOS. Linux and Windows are verified through CI on every push (M0 sets this up first for exactly this reason) and through VM sessions for the hands-on checks CI cannot cover — each milestone file states when a VM session is worth it. Practical consequences:
  - Push early and often; CI is the Linux/Windows compiler for code written on the Mac.
  - Win32- and Linux-only code is written on the Mac and compiled by CI. Keep it isolated per §11.2 rule 6 so the macOS build always builds completely.
  - Performance benchmarks run only on the Mac host; VM numbers are distorted by virtualized I/O and must never be published (§33.5).
- **Completion is checklist-defined:** every milestone file ends with a **Completion checklist** of objectively verifiable items (implementation, tests, acceptance, platform/CI, process). A milestone is done when — and only when — every box is checked; there is no other definition of done for a milestone. Copy the checklist into the verification log and check items there with evidence.
- **Logs:** for each milestone, produce the implementation log and the independent verification log under `docs/implementation-logs/<milestone>/`, and record which functional requirements the milestone satisfies.

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
