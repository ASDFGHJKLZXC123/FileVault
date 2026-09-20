# Milestone 1 — Database and repository lifecycle

> Part of the LocalVault build plan ([index](../00-INDEX.md)). Spec source: guide §37.
> **Have these open — the complete reading set for this milestone:**
> [Part 02](../02-toolchain-and-build-system.md) (§10.7: platform source selection in CMake) ·
> [Part 03](../03-architecture-and-public-api.md) (§12.4: Repository API, open modes) ·
> [Part 04](../04-repository-format-and-database.md) (§13–15: on-disk format, full schema, initialization steps) ·
> [Part 07](../07-consistency-concurrency-platform.md) (§23.5: transaction RAII · §23.6: locking rule and implementations) ·
> [Part 11](../11-appendix-skeletons.md) (§42.3: statement wrapper · §42.5: safe conversions · §42.13: platform lock skeleton).
> Nothing else is required; no other milestone file is ever needed.

## Role in the overall project

M1 creates the repository itself: the SQLite schema and migration framework, `Repository::create`/`open`, format validation, and the cross-process writer lock. Every mutating operation in every later milestone flows through what is built here. It is also where the **on-disk format is born** — the schema written in this milestone is what format version 1 means forever.

## Why it matters

- Schema mistakes discovered after M3 (when real objects reference real rows) cost migrations; discovered here they cost an edit. This is why Rev 2 added `change_time_ns`, `windows_attributes`, and `repository_settings` *before* any code existed.
- The locking rule (§23.6) is the project's whole answer to concurrent-process corruption. If it's implemented loosely now, every later milestone inherits race conditions that only appear under contention.
- Transactional migrations and the RAII `Transaction` wrapper establish the discipline (never commit in a destructor, check every SQLite result code) that crash consistency in M4 depends on.

## How it works

1. Wrap SQLite: `Database`, `Statement`, `Transaction` (§42.3, §23.5) — prepared statements only, every result code checked.
2. Migration runner: exclusive transaction, apply numbered migrations in order, record each in `schema_migrations` (§14.10). Migration 001 creates the full schema from §14 including `repository_settings`.
3. `Repository::create` follows the §15 step list exactly — including filesystem classification (reject network mounts without the flag, warn on FAT/exFAT) and `0700` on the repository root.
4. `Repository::open` validates format version and honors `OpenMode` semantics (§12.4): `read_only` never writes, never locks, never runs recovery.
5. The lock lives behind `platform_lock.hpp` with `posix_lock.cpp` (flock/fcntl) and `win32_lock.cpp` (`LockFileEx`) selected by CMake (§10.7, §42.13).

## Specification (verbatim from §37)

Implement:

- SQLite RAII wrappers.
- Migration framework.
- Initial schema (including `repository_settings`).
- Repository create/open (both open modes), destination filesystem classification, and restrictive repository permissions.
- Repository format validation.
- Writer lock: POSIX (`flock`/`fcntl`) and Win32 (`LockFileEx`) implementations behind one interface.
- Query repository information.

Reference sections: 12.4, 13–15, 23.6.

Tests:

- Create/open.
- Invalid directory.
- Unsupported format.
- Transaction rollback.
- Lock contention.

## Likely problems and confusions — with answers

1. **"How do I develop `win32_lock.cpp` on a Mac?"** You write it without compiling it — CMake excludes it from the macOS build, and the Windows CI job is what compiles it. Expect a push-fix-push loop for Win32 API typos; that is normal for this workflow. When CI is green, run the lock-contention test interactively once in your Windows VM to see it behave (two `localvault`-test processes, second gets `repository_busy`).
2. **"Foreign keys silently not enforced."** `PRAGMA foreign_keys = ON` is **per connection**, not per database. Enable it (and `busy_timeout`) in the `Database` constructor so no future connection can forget. Write a test that proves an FK violation actually fails.
3. **"Extra `-wal` and `-shm` files appeared next to repository.db."** Normal WAL-mode artifacts; never delete them while a connection is open, and don't add them to verification checks.
4. **"How do I test lock contention in one test process?"** `flock` locks are per file-description, so two handles in one process may not conflict the way two processes do. Spawn a child process (or a tiny helper binary) that grabs the lock, then assert the parent gets `repository_busy`. Same technique works on Windows.
5. **"Filesystem classification: what do I return on an unknown filesystem?"** Treat unknown as ordinary-local (allow, no warning). The classifier only needs to positively identify the risky classes: network (`nfs`, `smbfs`, `cifs`, `DRIVE_REMOTE`) and FAT/exFAT (§15 step 3).
6. **"Where does recovery run?"** Not here — recovery logic is M4. In M1, `open(read_write)` only *reserves the place* where recovery will be called. Keep the seam, leave the body empty.
7. **"The `0700` test fails."** Check it on the directory you created, not its parent, and remember `umask` doesn't matter if you `chmod` explicitly after creation. On Windows, skip this assertion (inherited-ACL policy, §15 step 4).
8. **VM check for this milestone:** create a repository inside the Windows VM once, then open the same repository folder from macOS (shared folder) in `read_only` mode — this is your first taste of the portability promise (§25.12) and costs five minutes.

## Completion checklist

M1 is complete only when **every** box is checked. Copy this checklist into the verification log and check items there with evidence.

**Implementation**

- [ ] `Database`/`Statement`/`Transaction` wrappers: prepared statements only, every SQLite result code checked, `Transaction` destructor rolls back and never commits or throws (§23.5, §42.3).
- [ ] Every connection sets `foreign_keys`, WAL, `synchronous=FULL`, `busy_timeout` in the constructor path — impossible to open a connection without them (§14).
- [ ] Migration runner: exclusive transaction, ordered application, one `schema_migrations` row per migration, full rollback on any failure (§14.10).
- [ ] Migration 001 creates the complete §14 schema: `repository_info`, `snapshots` (CHECK includes `'deleting'`), `entries` (with `change_time_ns`, `windows_attributes`), `chunks`, `entry_chunks`, `snapshot_warnings`, `repository_settings`, all indexes.
- [ ] Decision recorded (yes/no) on including optional `operation_history` now (§14.8).
- [ ] `Repository::create` performs all §15 steps in order, including filesystem classification (network rejected without `allow_risky_filesystem`; FAT/exFAT warns) and `0700` root on POSIX.
- [ ] `Repository::open` validates format version; `OpenMode::read_only` never creates files, never locks, never runs recovery (§12.4).
- [ ] `RepositoryLock`: POSIX (`flock`/`fcntl`) and Win32 (`LockFileEx`) behind `platform_lock.hpp`; PID/start-time written as diagnostics only; lock file never deleted (§23.6, §42.13).
- [ ] Repository information query returns UUID, format version, chunk size, zstd level, hash algorithm.

**Tests (all green)**

- [ ] Create → open round trip.
- [ ] Open rejects a random non-repository directory (`invalid_repository`).
- [ ] Open rejects a bumped/future format version with a clear error (`unsupported_repository_version`).
- [ ] Transaction rollback: exception mid-transaction leaves no partial rows.
- [ ] Foreign-key enforcement proven: an FK-violating insert actually fails.
- [ ] Re-opening does not re-run migrations (idempotent).
- [ ] Lock contention: a second **process** receives `repository_busy` (subprocess helper, not a second handle).
- [ ] Create rejects an existing non-empty directory without explicit approval.
- [ ] Filesystem classifier unit tests (network → reject, FAT → warn, unknown → allow).
- [ ] POSIX-only: repository root mode is `0700`.

**Platform & CI**

- [ ] All three CI jobs green (this proves `win32_lock.cpp` compiles).
- [ ] Windows VM (once, ~10 min): watch lock contention behave live; open a Mac-created repository read-only from the VM (first portability data point, §25.12). Result noted in the verification log.

**Process**

- [ ] Implementation + verification logs under `docs/implementation-logs/M1/`, including this checklist's state.
- [ ] Log records FR-001–FR-004 as satisfied, with the test names that prove each.
- [ ] Log records FR-005 as deferred to M4; M1 contains only the intentionally empty
      recovery seam and therefore cannot honestly provide an FR-005 proving test.
