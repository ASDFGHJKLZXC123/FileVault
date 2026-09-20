# M1 Orchestrator — Database and repository lifecycle

Spec: [../milestones/M1-database-and-repository-lifecycle.md](../milestones/M1-database-and-repository-lifecycle.md) · Loop: [00-COMMON.md](00-COMMON.md)
Reading set: Part 02 (§10.7) · 03 (§12.4) · 04 (§13–15) · 07 (§23.5–23.6) · 11 (§42.3/42.5/42.13)

## Fix first (orchestrator)

- Public headers: `Repository::create/open`, `OpenMode`, `platform_lock.hpp` interface.
- Record decision: include `operation_history` table now? (§14.8)

## Packets

| # | Packet | Scope | Order | Class |
|---|---|---|---|---|
| A | SQLite wrappers | `Database`/`Statement`/`Transaction` (§42.3, §23.5) + tests | parallel with D | standard |
| B | Schema + migrations | runner (§14.10), migration 001 = full §14 schema + tests | after A | tricky |
| C | Repository lifecycle | `create` per §15 (fs classification, `0700`), `open` + format validation, info query + tests | after B | tricky |
| D | Writer lock | `posix_lock.cpp` + `win32_lock.cpp` behind one header (§42.13), CMake selection (§10.7) + contention test | parallel with A | tricky |

## Watchpoints (put in briefs)

- `foreign_keys`, WAL, `synchronous=FULL`, `busy_timeout` set in `Database` ctor — per connection. Test that an FK violation actually fails.
- `Transaction` dtor: rollback only, never commit/throw.
- Lock-contention test needs a child **process**, not a second handle.
- `win32_lock.cpp` compiles only in CI — expect push-fix-push.
- `read_only`: no writes, no lock, no recovery. Recovery seam stays empty (M4).
- Unknown filesystem → allow silently; only network (reject) and FAT/exFAT (warn) are flagged.
- POSIX-only `0700` assert; skip on Windows.

## Verify

- Fresh agent: checklist → `docs/implementation-logs/M1/`; FR-001–004 mapped to test names.
  FR-005 remains assigned to M4, where interrupted-operation recovery is implemented
  and proven; M1 provides only the intentionally empty recovery seam.
- Richard: Windows VM (~10 min) — watch lock contention live; open Mac-created repo read-only (§25.12 data point).
