# Milestone 5 — Concurrency and cancellation

> Part of the LocalVault build plan ([index](../00-INDEX.md)). Spec source: guide §37.
> **Have these open — the complete reading set for this milestone:**
> [Part 03](../03-architecture-and-public-api.md) (§12.3: progress API and totals · §12.5: snapshot options) ·
> [Part 05](../05-snapshot-engine.md) (§16: full pipeline steps · §18.5: striped-mutex dedup race) ·
> [Part 07](../07-consistency-concurrency-platform.md) (§24: queues, workers, SQLite policy, cancellation, errors · §25.4/§25.11: scanner platform rules) ·
> [Part 09](../09-quality-testing-ci-packaging.md) (§33.2: dataset profiles · §34.5: TSan policy) ·
> [Part 11](../11-appendix-skeletons.md) (§42.4: bounded queue skeleton).
> Nothing else is required; no other milestone file is ever needed.

## Role in the overall project

M5 makes snapshots fast and interruptible: the scanner → bounded queue → worker pool → single metadata writer pipeline (§24.1), cooperative cancellation via `std::stop_token`, throttled progress with post-scan totals, and the scanner's full platform rules (junctions, mount points, `one_file_system`). It is deliberately the *last* core-engine milestone: concurrency multiplies whatever correctness already exists — which is why M2–M4 built that correctness single-threaded first.

## Why it matters

- Bounded queues are the memory guarantee (NFR-002/003): a million-file tree must not become a million-item in-memory list.
- The single-writer rule for SQLite (§24.5) is what keeps the database sane under parallelism — every dedup/insert race funnels through one thread.
- Cancellation discipline set here (check points, cooperative-only) is inherited by the CLI's Ctrl+C (M7) and the GUI's Cancel button (M8). If cancel is flaky here, it's flaky everywhere.
- The striped-mutex duplicate-write handling (§18.5) closes the last dedup correctness hole: two workers discovering the same new hash simultaneously.

## How it works

1. `BoundedQueue<T>` per §42.4: capacity/byte budget, `push`/`pop` blocking with `condition_variable_any` + stop_token, `close()` semantics, no busy-waiting.
2. One scanner thread produces `FileJob`s; N workers (default per §24.4) read/hash/compress/store; one metadata writer owns the write connection and commits batches.
3. First fatal error → stored `exception_ptr`, stop requested everywhere, queues closed, threads joined, original exception rethrown as the operation failure (§24.7).
4. Workers bump atomic counters; a coordinator emits ≤10 progress events/second and fills `total_entries/total_bytes` once scanning completes (§24.8, §12.3).

## Specification (verbatim from §37)

Implement:

- Bounded queues.
- Scanner producer (junction/mount-point rules and `one_file_system`, Sections 25.4 and 25.11).
- Worker pool.
- Metadata writer.
- Stop tokens.
- Progress aggregation (including totals after scan completion).
- Fatal-error propagation.

Reference sections: 16, 24, 25.

Acceptance:

- Cancellation leaves no complete partial snapshot.
- ASan/UBSan tests pass.
- TSan-focused tests pass when enabled.
- Memory remains bounded on a large generated dataset.

## Likely problems and confusions — with answers

1. **"Everything deadlocks on shutdown."** The usual chain: workers blocked on a full result queue while the writer already exited on error. The rule: on any stop/error, `close()` **all** queues before joining anything, and make `push` on a closed queue return false rather than block. Test shutdown from every role (scanner error, worker error, writer error, external cancel).
2. **"The first exception gets lost and I see a follow-on error instead."** Store only the *first* `exception_ptr` under a mutex (`std::call_once` works), then request stop. Everything after is noise caused by shutdown — never overwrite.
3. **"Can I run TSan on the Mac?"** Yes — Apple Clang supports `-fsanitize=thread` (add a local preset if useful), and it's the fastest way to shake out queue races during development. The *official* TSan gate stays on the Linux CI job (§34.5); MSVC has no TSan, which is fine — the concurrent code is platform-neutral.
4. **"Duplicate-write race never triggers in my test."** Force it: a dataset of many copies of one unique file + max workers + a tiny artificial delay hook inside the stripe (test-only). The assertion is one object file, one `chunks` row, correct counters — run it under TSan too.
5. **"Cancellation test flakes: sometimes the snapshot completed before the cancel."** Make the dataset big enough that scan+process reliably outlasts the cancel trigger, or gate the cancel on a progress event ("after ≥100 entries processed, request stop"). Assert the post-condition (no `complete` row, recovery leaves prior snapshots intact) rather than timing.
6. **"How do I test 'memory remains bounded'?"** Generate the many-small-files benchmark dataset (§33.2 profile A), run a snapshot, and sample peak RSS (`getrusage` on Mac/Linux) — assert it stays under a generous fixed ceiling (e.g., a few hundred MB) rather than a precise number.
7. **"hardware_concurrency on Apple Silicon counts efficiency cores — too many workers?"** The §24.4 clamp (1..16) is the answer; don't tune further without a benchmark (disk-bound work often prefers fewer workers — §24.4's own warning).
8. **"Junction/mount-point code is untestable on the Mac."** Correct — those scanner branches are Windows-only. Unit-test the decision function with faked attributes locally; the real junction fixture test runs in Windows CI (§32.3 Windows suite). One human-observed Windows session: create a real junction loop in a test tree and confirm the scanner records the link without traversing it, within a 30-second watchdog.

### M5 environment amendment — 2026-09-20

The user approved native Windows x64 / NTFS validation in place of the original VM session for
this junction-loop exercise. The real self-referencing junction, human-reported result, scanner
assertions, and 30-second watchdog remain required. This amendment applies only to M5; later
VM/clean-machine checks are unchanged. The accepted PASS is recorded in the
[verification log](../../docs/implementation-logs/M5/verification.md#one-time-windows-human-gate--pass).

## Completion checklist

M5 is complete only when **every** box is checked. Copy this checklist into the verification log and check items there with evidence.

**Implementation**

- [ ] `BoundedQueue`: capacity (and byte budget where results are large), blocking push/pop with stop_token support, `close()` semantics, no busy-waiting (§24.3, §42.4).
- [ ] Pipeline wired: one scanner producer → job queue → worker pool → result queue → one metadata writer that owns the sole write connection (§24.1, §24.5).
- [ ] **Ignore rules implemented here** (`.localvaultignore` per §27, `--ignore-file` replaces it entirely) — the plan assigns them to the scanner, so they land in this milestone; record this mapping in the log.
- [ ] Scanner platform rules: junctions/mount points never traversed (decision function unit-testable), `one_file_system` honored, hidden-file option wired, cloud placeholders skipped with warnings (§25.4, §25.11).
- [ ] Worker count default per §24.4 (the single normative definition), configurable and clamped.
- [ ] Striped-mutex duplicate-object protection wrapped around the M3 publish sequence (§18.5).
- [ ] Stop-token checks at every §24.6 point; cancellation is cooperative only.
- [ ] Unstable-file handling: pre/post `{size, mtime, ctime, file_id}` comparison, one retry, then skip with warning (§16.4–16.5), via a test-controllable seam.
- [ ] First fatal error captured in a synchronized `exception_ptr`; stop requested, queues closed, all threads joined, original error rethrown (§24.7).
- [ ] Progress: atomic counters, coordinator emits ≤10 events/second, `total_entries`/`total_bytes` filled once scanning completes (§24.8, §12.3).

**Tests (all green)**

- [ ] Queue battery: close-while-waiting, cancel-while-waiting, multiple producers/consumers, byte-budget enforcement.
- [ ] Shutdown from every role: scanner error, worker error, writer error, external cancel — no deadlock, no lost first-error.
- [ ] Forced duplicate-write race (identical-content dataset, max workers): one object file, one `chunks` row, correct counters.
- [ ] Ignore-rule battery (§32.2): comments, exact names, wildcards, directory pruning (no recursion into ignored dirs), nested paths, hidden files, spaces, case behavior.
- [ ] Unstable-file scenarios (§32.3): changes once → retried and stored; changes twice → skipped with warning; disappears; permission denied; snapshot still completes as partial success.
- [ ] Cancellation during scan and during chunk processing: no `complete` partial snapshot; recovery clean (acceptance).
- [ ] Memory bound: many-small-files dataset (§33.2 profile A) snapshot stays under a fixed RSS ceiling (acceptance).
- [ ] Progress test: totals are empty during scan, present and correct after.

**Platform & CI**

- [ ] ASan/UBSan preset green locally on the Mac **and** on the Linux sanitizer CI job (acceptance).
- [ ] TSan run over the concurrency-focused tests (locally on the Mac and/or the Linux job) — clean (acceptance).
- [ ] All three CI jobs green; the junction/one-file-system fixture tests pass on the Windows CI job.

**Process**

- [ ] Implementation + verification logs under `docs/implementation-logs/M5/`, including this checklist's state.
- [ ] Log records FR-112–FR-119 and FR-114 with proving test names; notes the ignore-rules mapping decision.
