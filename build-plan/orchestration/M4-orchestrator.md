# M4 Orchestrator — Crash-safe publication

Spec: [../milestones/M4-crash-safe-publication.md](../milestones/M4-crash-safe-publication.md) · Loop: [00-COMMON.md](00-COMMON.md)
Reading set: Part 03 (§12.4) · 05 (§16.2–16.3, §18.6) · 06 (§22.1) · 07 (§23) · 08 (§31.5) · 09 (§32.5)

## Fix first (orchestrator)

- **Cross-cutting milestone: mostly sequential.** It modifies invariants inside existing code; parallel edits to the publish/commit paths will collide.
- The one promise: a crash never damages a previously complete snapshot. Publication ordering (object durable → chunks row → entry refs → `complete`) is stated in every brief.

## Packets

| # | Packet | Scope | Order | Class |
|---|---|---|---|---|
| A | Durability layer | POSIX flush→fsync (`F_FULLFSYNC` mac)→rename→dir-fsync; Win32 `FlushFileBuffers`→`MoveFileExW(...WRITE_THROUGH)` (§23.2), temp files under `temporary/objects` | parallel with B | tricky |
| B | Injector seam | `FailureInjector` + `Repository::set_failure_injector` (§32.5, §12.4), no-op default | parallel with A | standard |
| C | Snapshot lifecycle | `pending` first, bounded metadata batches (overridable constant), `complete` flip, failed/cancelled cleanup (§16.2–16.3, §23.3) | after A+B | critical |
| D | Deleting + recovery | resumable `deleting` machinery (§22.1); recovery on first mutating op (§23.4) | after C | critical |
| E | Injection matrix | per-`FailurePoint` throw→reopen→assert; meta-test all points fire | last | tricky |

Fresh critical invariant review on the fsync sequence, visibility rule, and recovery before verification (per `00-COMMON.md`).

## Watchpoints (put in briefs)

- Tests verify **ordering and recoverability**, not physical durability — trust the sequence, test the state machine.
- Crash residue (temp files, orphan objects) is designed; assert cleanliness *after recovery*.
- Injector must fire in metadata batch commit and snapshot publish, not just the object store — the meta-test proves it.
- Only `status='complete'` visible anywhere snapshots are queried.
- Dtor rollback swallows and logs (§31.5); injection test proves no `std::terminate`.
- No directory fsync on Windows — documented weaker guarantee (§39); don't invent workarounds.

## Verify

- Fresh agent: checklist → `docs/implementation-logs/M4/`; FR-005/111/113/408-partial mapped.
- Richard: Windows VM (once) — Task Manager kill mid-snapshot, reopen, recovery clean, old snapshots restore.
