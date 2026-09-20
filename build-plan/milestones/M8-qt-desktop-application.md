# Milestone 8 — Qt desktop application

> Part of the LocalVault build plan ([index](../00-INDEX.md)). Spec source: guide §37.
> **Have these open — the complete reading set for this milestone:**
> [Part 03](../03-architecture-and-public-api.md) (§12.3: progress marshaling contract · §12.6/§12.8: restore request and paged queries) ·
> [Part 07](../07-consistency-concurrency-platform.md) (§24.8: progress throttling contract) ·
> [Part 08](../08-interfaces-cli-gui-config.md) (§29: the whole GUI spec · §30: settings storage split) ·
> [Part 11](../11-appendix-skeletons.md) (§42.11: conflict-resolver usage rules).
> Nothing else is required; no other milestone file is ever needed.

## Role in the overall project

M8 delivers the desktop product: onboarding, dashboard, snapshot browsing, restore with conflict dialogs, verification, settings, and GC — all as a *thin* Qt layer over the same `localvault_core` the CLI uses. It is deliberately last-but-one: by now every operation it exposes is already tested three ways (unit, integration, e2e), so GUI work is purely presentation and threading.

## Why it matters

- The GUI is most users' entire experience of the tool; a frozen window during a 30-minute snapshot reads as "broken program" no matter how correct the engine is. The acceptance criterion is exactly that: **no core operation on the GUI thread, ever.**
- The dependency rules (§11.2: no engine logic in widgets, no Qt in core) pay off or fail here. If M8 requires core changes beyond small API affordances, that's a signal to fix the core seam, not to fork logic into the GUI.
- Lazy models (§29.3) are what make million-entry snapshots browsable; loading a whole tree into a `QTreeWidget` would undo NFR-002 in the interface layer.

## How it works

1. `OperationController` owns a `QThread`; `CoreOperationWorker` moves onto it and calls the engine; core progress callbacks are re-emitted as Qt signals over queued connections; Cancel triggers the worker's `std::stop_source` (§29.4).
2. Snapshot list = `QAbstractTableModel` over `QueryService::list_snapshots`; the restore tree = lazy `QAbstractItemModel` fetching children per directory via `list_children` pages (§29.3).
3. Conflict prompts: the core's `ConflictResolver` callback blocks the *worker* thread while the GUI shows a dialog and returns the decision (§42.11).
4. UI preferences in `QSettings`; repository settings read/written through `repository_settings` (§30); first-run onboarding gates the main window (§29.1).

## Specification (verbatim from §37)

Implement:

- Main window/navigation.
- First-run onboarding (create/open repository).
- Repository opening.
- Dashboard.
- Snapshot table.
- Lazy restore tree.
- Restore conflicts.
- Verification page.
- Settings (backed by `repository_settings`).
- Garbage-collection UI with mandatory dry-run preview.
- Background workers.
- Progress and cancellation.
- Error dialogs.

Acceptance:

No long core operation runs on the GUI thread, and the application remains responsive.

## Likely problems and confusions — with answers

1. **"Random crashes with no pattern."** A worker thread touched a widget or model directly. The iron rule: worker → signal (queued) → slot on GUI thread → widget. Auditing every `connect` for connection type is faster than debugging one of these crashes.
2. **"The conflict dialog deadlocks."** The worker must emit a request and **block on its own condition variable** until the GUI slot stores the answer and notifies. If instead the GUI thread waits on the worker (any `exec()`-while-worker-waits inversion), you deadlock. Draw the handoff before coding it.
3. **"The lazy tree is the hardest part — indexes go stale."** Symptoms: expand arrows on empty dirs, crashes on refresh. Rules that keep it sane: `canFetchMore`/`fetchMore` per §29.3, immutable model data while a fetch is in flight, full model reset after mutating operations (explicit refresh — §29.3), and parent pointers owned by the model, never by Qt internals.
4. **"Progress events flood the event loop."** The core already throttles to ~10/s (§24.8); if the UI still stutters, coalesce in the controller (keep only the latest event per tick). Never call `processEvents()` as a fix.
5. **"How do I test any of this?"** `QT_QPA_PLATFORM=offscreen` (§29.7) for model unit tests (table row counts, lazy fetch behavior, conflict-resolver marshaling) in CI; the responsiveness criterion is checked by hand — run a huge snapshot and use every page while it runs.
6. **"macOS blocks my app from reading ~/Documents."** TCC/Full Disk Access (§39): during development grant Full Disk Access to your terminal (tests) and to the built app bundle. Permission-denied entries surfacing as warnings — not crashes — is itself a required behavior to verify.
7. **"Do I build the GUI on Windows/Linux too?"** CI already compiles it everywhere; functional VM passes matter here more than in any other milestone because look-and-feel and file dialogs differ. Budget one session per VM: onboarding → snapshot → browse → restore → verify → GC dry-run, watching for frozen paints and path-display oddities.
8. **"GC button feels dangerous."** It must be: the spec mandates dry-run preview *before* the destructive step is even enabled (§29.6), plus confirmation. Wire the button to run dry-run first and show reclaimable bytes; "Execute" only after.

## Completion checklist

M8 is complete only when **every** box is checked. Copy this checklist into the verification log and check items there with evidence.

**Implementation**

- [ ] First-run onboarding: Create Repository / Open Repository gate before the main pages; create runs on a worker thread with the same filesystem checks and warnings as the CLI (§29.1, §15).
- [ ] Dashboard shows all §29.2 items (repository, latest snapshot, health, counts, sizes, savings) plus Create Snapshot and the Maintenance/GC action.
- [ ] Snapshot table = `QAbstractTableModel` over paged `QueryService::list_snapshots`; all §29.2 columns and row actions (browse, compare, restore, delete, warnings).
- [ ] Restore page: snapshot selector, lazy tree (`canFetchMore`/`fetchMore` over `list_children` pages), path search, multi-select, destination chooser, overwrite policy, progress + result summary.
- [ ] Lazy tree obeys the §29.3 rules: no widget-per-file, no full-tree loads, model immutable while a fetch is in flight, explicit refresh after mutating operations.
- [ ] Verify page: quick/full choice, streaming issue table, checked-objects/bytes/elapsed.
- [ ] Settings page: UI prefs in `QSettings`; repository settings read/write `repository_settings`; chunk size shown read-only (§29.2, §30).
- [ ] GC UI: dry-run preview is mandatory before Execute enables; both confirmed (§29.6).
- [ ] Worker pattern per §29.4: `OperationController` + `CoreOperationWorker` on a `QThread`; progress via queued signals; Cancel → `std::stop_source`; completion/failure signals rejoin the GUI thread; worker torn down safely.
- [ ] Conflict prompts: core `ConflictResolver` blocks the worker while a GUI dialog answers; apply-to-all supported (§42.11).
- [ ] Every `ErrorCode` maps to a §29.5 dialog with expandable technical detail; partial success visibly reported.
- [ ] State rules (§29.6): conflicting actions disabled during operations, cancel stays enabled, `pending` snapshots hidden, statistics refresh only on completion, destructive actions confirmed.
- [ ] Zero direct widget/model access from worker threads — every cross-thread `connect` is queued (audit them all once).

**Tests & manual verification**

- [ ] Offscreen (`QT_QPA_PLATFORM=offscreen`) model tests in CI: snapshot table row/column behavior, lazy fetch (children only on expand), conflict-resolver marshaling.
- [ ] Manual responsiveness protocol on the Mac, recorded in the log: start a large snapshot (profile-A dataset), then visit every page, browse a big tree, run a search, cancel the snapshot — no freeze at any point, cancel always live (acceptance).
- [ ] macOS TCC behavior verified: without Full Disk Access, protected folders produce permission warnings, not crashes; grant procedure documented in the user guide notes.

**Platform & CI**

- [ ] GUI builds and offscreen tests pass on all three CI jobs.
- [ ] One manual VM pass on Windows and one on Linux: onboarding → snapshot → browse → restore → verify → GC dry-run; file dialogs, paths, and repaints sane. Both recorded in the verification log.

**Process**

- [ ] Implementation + verification logs under `docs/implementation-logs/M8/`, including this checklist's state.
- [ ] Log records FR-501–FR-503 with proof (test names + manual protocol results).
