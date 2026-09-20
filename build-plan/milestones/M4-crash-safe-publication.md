# Milestone 4 — Crash-safe object and snapshot publication

> Part of the LocalVault build plan ([index](../00-INDEX.md)). Spec source: guide §37.
> **Have these open — the complete reading set for this milestone:**
> [Part 03](../03-architecture-and-public-api.md) (§12.4: `set_failure_injector` seam) ·
> [Part 05](../05-snapshot-engine.md) (§16.2–16.3: batches and statuses · §18.6: object write algorithm) ·
> [Part 06](../06-restore-diff-verify-gc.md) (§22.1: `deleting` batched deletion) ·
> [Part 07](../07-consistency-concurrency-platform.md) (§23: consistency model, durability, publication, recovery, locking) ·
> [Part 08](../08-interfaces-cli-gui-config.md) (§31.5: cleanup errors and non-throwing destructors) ·
> [Part 09](../09-quality-testing-ci-packaging.md) (§32.5: failure-injection points).
> Nothing else is required; no other milestone file is ever needed.

## Role in the overall project

M4 upgrades "works" to "survives": temp-file-fsync-rename object publication, the `pending → complete` snapshot state machine, batched metadata commits, startup recovery, and resumable `deleting`. This is the milestone that makes LocalVault a *backup tool* rather than a file copier — the consistency model in §23.1 becomes enforceable fact.

## Why it matters

- A backup tool's one unbreakable promise: **a crash can never damage a previously complete snapshot.** Everything in this milestone serves that sentence.
- The publication ordering (object durable → chunks row → entry references → snapshot `complete`) is a dependency chain; any shortcut creates a window where metadata references bytes that don't exist. Those bugs are invisible in testing and catastrophic in real crashes — which is why this milestone's acceptance is *injected* failure, not observed luck.
- Recovery (§23.4) is what makes every other milestone's failure modes acceptable: cancel, crash, kill -9 — all funnel into "recovery cleans it up."

## How it works

1. Object writes follow §18.6/§23.2 exactly: temp file under `temporary/objects` → flush → fsync (`F_FULLFSYNC` on the Mac where full durability is required) → atomic rename → parent dir fsync (POSIX) / `MOVEFILE_WRITE_THROUGH` (Windows).
2. Snapshots: insert `pending` row first (crash-detectable), commit entry metadata in bounded batches, flip to `complete` in one final transaction (§23.3). Only `complete` is ever visible.
3. Recovery on first mutating operation: fail stale pendings, resume `deleting`, clear `temporary/`, leave orphan objects for GC (§23.4).
4. `FailureInjector` (§32.5) installed via `Repository::set_failure_injector`; tests throw at each `FailurePoint`, reopen, and assert invariants.

## Specification (verbatim from §37)

Implement:

- Temporary object files.
- Per-platform durability sequence (fsync / `FlushFileBuffers`, Section 23.2).
- Atomic rename (`rename` / `MoveFileExW`).
- Snapshot statuses, including `deleting` with resumable batched deletion.
- Metadata batches.
- Recovery of stale snapshots.
- Failure injection tests via `Repository::set_failure_injector`.

Reference sections: 18.6, 22.1, 23.

Acceptance:

Injected failure at each publication point never damages a previously complete snapshot.

## Likely problems and confusions — with answers

1. **"How can a test verify fsync actually worked?"** It can't — real durability needs power-cut rigs. The tests verify *ordering and recoverability*: throw at each injection point, reopen, assert the old snapshot restores and the new one is invisible/cleaned. Trust the sequence, test the state machine.
2. **"macOS fsync confusion."** On Darwin, `fsync()` doesn't force media flush; `fcntl(fd, F_FULLFSYNC)` does (§23.2). Put the choice inside the platform layer with a comment; don't sprinkle `#ifdef`s (rule §11.2.6).
3. **"Do I fsync the directory on Windows?"** You can't — there is no directory fsync. The spec's answer (§23.2): `MOVEFILE_WRITE_THROUGH` plus NTFS journaling, documented as a weaker guarantee (§39). Don't invent a workaround; implement the documented sequence.
4. **"My injected crash left a temp file / an orphan object — is that a failure?"** No. Leftovers are the *designed* crash residue (§23.1); the invariant is that recovery removes them and complete snapshots are untouched. Assert cleanliness *after recovery*, not after the crash.
5. **"Injection points aren't firing."** Classic miss: an injector call was added to the object store but not to metadata batch commit or snapshot publish. Write one meta-test that runs a snapshot with a counting injector and asserts every `FailurePoint` enum value was hit at least once.
6. **"Where does the batch size live?"** Commit every ~500 entries or 64 MiB (§16.2.11). Make it a constant with a test override so failure-injection tests can force multiple batches with tiny datasets.
7. **"Rollback in the Transaction destructor threw during exception unwinding."** Destructors must swallow and log (§31.5). The M1 wrapper already promised this; M4 is where it gets exercised — add the injection test that fails mid-batch and confirms no `std::terminate`.
8. **Mac-primary note:** all injection tests are platform-neutral — full local development. The Windows durability *code path* (`FlushFileBuffers`/`MoveFileExW`) is compiled and unit-exercised by CI; use one Windows VM session to run the full snapshot+kill+reopen cycle by hand (Task Manager kill mid-snapshot, reopen, verify) — it's the cheapest real-world confidence you can buy for this milestone.

## Completion checklist

M4 is complete only when **every** box is checked. Copy this checklist into the verification log and check items there with evidence.

**Implementation**

- [ ] Object temp files live under `temporary/objects` with unique names, on the same filesystem as `objects/`.
- [ ] POSIX durability sequence implemented in the platform layer: flush → fsync (`F_FULLFSYNC` on macOS where full durability is required) → `rename` → parent-directory fsync (§23.2).
- [ ] Win32 durability sequence implemented: flush → `FlushFileBuffers` → `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` (§23.2).
- [ ] Snapshot lifecycle: `pending` row committed before scanning; entry metadata commits in bounded batches (batch size a constant, overridable in tests); final transaction computes counters and flips to `complete` (§16.2–16.3, §23.3).
- [ ] Failure/cancel paths: snapshot marked `failed`/`cancelled` with message; its entries/mappings/warnings removed transactionally; objects left for GC.
- [ ] `deleting` status machinery: batched entry deletion, resumable, final row delete (§22.1) — the machinery, even though the user-facing delete command lands in M6.
- [ ] Recovery on first mutating operation (§23.4): stale pendings failed and cleaned in batches, `deleting` resumed to completion, everything under `temporary/` removed, quick relationship check runs.
- [ ] Only `status='complete'` is visible to list/browse/restore — verified everywhere snapshots are queried.
- [ ] `FailureInjector` + `Repository::set_failure_injector` seam; production default is a no-op; `hit()` called at every §32.5 `FailurePoint`.

**Tests (all green)**

- [ ] Injection matrix: for **each** `FailurePoint`, throw → reopen → assert: previously complete snapshots restore byte-identically, the interrupted snapshot is not `complete`, recovery leaves no temp files.
- [ ] Meta-test: a counting injector proves every `FailurePoint` enum value fires at least once across a snapshot + delete cycle.
- [ ] Crash between object publish and `chunks`-row commit yields an orphan object that is tolerated (quick check passes; object queryable as orphan).
- [ ] Batch boundaries: with batch size forced to a tiny value, a multi-batch snapshot completes and an injected failure between batches recovers.
- [ ] Cancellation mid-snapshot: no partial snapshot becomes `complete`; recovery cleans its metadata (FR-113).
- [ ] Interrupted `deleting` resumes to completion on reopen.
- [ ] Exception during rollback path does not `std::terminate` and preserves the original error (§31.5, §42.7).

**Platform & CI**

- [ ] All three CI jobs green (Windows durability code compiles and unit-level tests pass).
- [ ] Windows VM (once): kill `localvault` mid-snapshot from Task Manager, reopen, recovery is clean and old snapshots restore. Result recorded in the verification log.

**Process**

- [ ] Implementation + verification logs under `docs/implementation-logs/M4/`, including this checklist's state.
- [ ] Log records FR-005, FR-111, FR-113, FR-408 (partial — full GC in M6) with proving test names.
