# M8 Orchestrator — Qt desktop application

Spec: [../milestones/M8-qt-desktop-application.md](../milestones/M8-qt-desktop-application.md) · Loop: [00-COMMON.md](00-COMMON.md)
Reading set: Part 03 (§12.3/12.6/12.8) · 07 (§24.8) · 08 (§29, §30) · 11 (§42.11)

## Fix first (orchestrator)

- Iron rule in every brief: **no core operation on the GUI thread; worker → queued signal → GUI slot → widget.** No Qt in core; no engine logic in widgets (§11.2).
- If M8 needs core changes beyond small API affordances, stop and fix the core seam.
- Acceptance is partly human — plan the manual protocol up front.

## Packets

| # | Packet | Scope | Order | Class |
|---|---|---|---|---|
| A | Worker infra | `OperationController` + `CoreOperationWorker` on `QThread`, queued progress signals, Cancel → `stop_source`, safe teardown (§29.4) | first | critical |
| B | Onboarding + dashboard | first-run gate, create/open on worker, §29.2 dashboard | after A, parallel | standard |
| C | Models | snapshot `QAbstractTableModel`; lazy restore tree (`canFetchMore`/`fetchMore` over `list_children`, §29.3 rules) — hardest packet, isolate | after A, parallel | critical |
| D | Conflicts | `ConflictResolver` blocks worker on its own condvar; GUI dialog answers; apply-to-all (§42.11) | after A, parallel | critical |
| E | Verify/settings/GC pages | streaming issue table; `QSettings` + `repository_settings` split (§30); GC dry-run mandatory before Execute (§29.6) | after A, parallel | standard |
| F | Offscreen tests | `QT_QPA_PLATFORM=offscreen`: table behavior, lazy fetch, resolver marshaling | last | standard |

Fresh critical invariant review before verification: every cross-thread `connect` is queued; zero widget/model access from workers (per `00-COMMON.md`).

## Watchpoints (put in briefs)

- Random crashes = worker touched a widget. Audit connects; don't debug instances.
- Conflict handoff: worker waits on its own condition variable; GUI never waits on the worker. Draw it before coding.
- Lazy tree: model immutable while a fetch is in flight; full reset after mutating ops; model owns parent pointers.
- Progress flood: coalesce latest-per-tick in controller; never `processEvents()`.
- State rules §29.6: conflicting actions disabled during ops, cancel always live, `pending` hidden, destructive actions confirmed.

## Verify

- Fresh agent: checklist (implementation + offscreen tests) → `docs/implementation-logs/M8/`; FR-501–503 mapped.
- Richard (recorded in log): Mac responsiveness protocol — large snapshot running, visit every page, browse big tree, search, cancel — no freeze; macOS TCC check (warnings, not crashes); one manual VM pass each on Windows and Linux (onboarding → snapshot → browse → restore → verify → GC dry-run).
