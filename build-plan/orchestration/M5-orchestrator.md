# M5 Orchestrator — Concurrency and cancellation

Spec: [../milestones/M5-concurrency-and-cancellation.md](../milestones/M5-concurrency-and-cancellation.md) · Loop: [00-COMMON.md](00-COMMON.md)
Reading set: Part 03 (§12.3/12.5) · 05 (§16, §18.5) · 07 (§24, §25.4/25.11) · 09 (§33.2, §34.5) · 11 (§42.4)

## Fix first (orchestrator)

- **Cross-cutting: pipeline core is sequential; only A/B/D parallelize.** Concurrency multiplies existing correctness — don't touch M3/M4 semantics, wrap them.
- Rules stated in every brief: single metadata writer owns the sole write connection (§24.5); cancellation cooperative only; bounded memory (NFR-002/003).

## Packets

| # | Packet | Scope | Order | Class |
|---|---|---|---|---|
| A | BoundedQueue | §42.4: capacity/byte budget, stop_token push/pop, `close()` + battery | parallel | tricky |
| B | Ignore rules | `.localvaultignore` parser (§27), `--ignore-file` + battery | parallel | standard |
| C | Pipeline | scanner → job queue → N workers → result queue → metadata writer (§24.1); first-error `exception_ptr`, stop, close, join, rethrow (§24.7); worker count §24.4 | after A | critical |
| D | Scanner platform rules | junction/mount decision fn (unit-testable, faked attributes), `one_file_system`, placeholders (§25.4/25.11); wire B in | after C (decision fn parallel) | standard |
| E | Races + instability | striped mutex around M3 publish (§18.5); unstable-file pre/post check, retry, skip (§16.4–16.5) | after C | critical |
| F | Progress | atomic counters, ≤10 events/s, totals after scan (§24.8, §12.3) | after C | standard |
| G | Acceptance runs | cancellation, forced dup race, RSS-bounded profile-A run; ASan/UBSan/TSan | last | standard |

Fresh critical invariant review on shutdown paths (all queues closed before any join) before verification (per `00-COMMON.md`).

## Watchpoints (put in briefs)

- Deadlock pattern: worker blocked on full queue while writer exited. On any stop/error close **all** queues first; `push` on closed returns false.
- Store only the *first* `exception_ptr`; everything after is shutdown noise.
- Dup-race test: many copies of one file + max workers + test-only delay hook in the stripe; assert one object, one row; run under TSan.
- Cancel test gates on progress ("≥100 entries then stop"), asserts post-condition, never timing.
- Memory bound: peak RSS under a generous fixed ceiling, not a precise number.
- Junction branches are Windows-only: unit-test the decision function locally; real fixture runs in Windows CI.

## Verify

- Fresh agent: checklist → `docs/implementation-logs/M5/`; FR-112–119 mapped; ignore-rules mapping noted.
- Sanitizers: ASan/UBSan green locally + Linux CI; TSan clean over concurrency tests.
- Richard: one human-observed real Windows junction-loop run with a 30-second watchdog. Native
  Windows x64 / NTFS is accepted for M5 under the user-approved 2026-09-20 milestone amendment;
  record the actual environment and user-reported result, never self-certify the human gate.
