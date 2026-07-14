# M5 Packet F Brief — Progress Aggregation

Class: standard. Run after the core pipeline and race/instability work. Smallest complete
implementation; state assumptions explicitly and report a summary.

## Verbatim requirements

From §12.3:

> The callback may be invoked from worker threads. Interface adapters must marshal events to their
> own threads.
>
> `total_entries` and `total_bytes` are empty until the scan phase completes, because the pipeline
> overlaps scanning with processing. Interfaces must show indeterminate progress until totals
> arrive, then switch to a percentage.

From §24.8:

> Workers update atomic counters. A coordinator emits throttled progress events, for example at
> most 10 times per second, plus phase changes. Avoid invoking expensive UI callbacks for every
> chunk.

Milestone invariant:

> Progress counters must be safe under concurrency, emitted no more than 10 times per second, and
> expose totals only after scanning completes.

## File boundary

- Primary: progress aggregation inside `src/core/snapshot_engine.cpp`; add one small private helper
  file only if it materially reduces code.
- Public `ProgressEvent` shape is already normative; do not expand it.
- Add `tests/integration/m5_progress_test.cpp` or focused tests in the M5 pipeline test file.
- Do not change scanner decisions, object publication, database schema, or CLI/GUI.

## Required tests

- Totals are empty on every event observed before scan completion, then present and exact.
- Discovered/processed entries and bytes are monotonic and final counters are correct.
- Callback emission is no more than 10 events in any one-second interval, while allowing the
  required final completion event without a timing sleep.
- Callback exceptions through `finalizing` become the first fatal pipeline error; queues close
  before joins and the snapshot is not complete. Plan amendment: the final `complete` notification
  runs after the irreversible M4 publication commit and is best-effort. Its exception is contained
  because a complete snapshot may never be retracted and user code may not run inside SQLite's
  publication transaction.
- Progress aggregation is clean under TSan.

## Watchpoints

- Callback invocation must not hold a queue, counter, error, SQLite, or object stripe mutex.
- Keep current path/message ownership safe; do not publish references to worker-local data.
- Tests gate on explicit progress/state transitions, not elapsed sleeps.
- The final event reports completed counters and totals; cancellation never emits `complete`.
