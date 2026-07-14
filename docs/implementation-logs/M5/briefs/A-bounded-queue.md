# M5 Packet A Brief — BoundedQueue

Class: tricky. This packet may run in parallel with Packet B. Smallest complete implementation;
state assumptions explicitly and report a summary.

## Verbatim requirements

From §24.3:

> Implement a queue with:
> - Maximum item count and/or byte budget.
> - `push` that waits until space or cancellation.
> - `pop` that waits until data, closure, or cancellation.
> - `close` to signal no more items.
> - No busy waiting.
> - Condition variables protected by one mutex.
> - Exception-safe element transfer.
>
> A byte budget is useful because one result may contain thousands of chunk references.

From §42.4:

> Add tests for closure, cancellation, multiple producers/consumers, and exception-safe moves.

Milestone invariant:

> Queue bounds must cover item capacity and byte budget where results are large.
> Queue push/pop operations must support stop_token; close() must wake blocked operations; no
> busy-waiting.

## File boundary

- Add `src/core/concurrency/bounded_queue.hpp` (header-only template).
- Add `tests/unit/bounded_queue_test.cpp`.
- Update only `tests/CMakeLists.txt` as needed.
- Do not edit the scanner, snapshot engine, object store, database, public API, or build plan.

## Required tests

- Reject zero item capacity and invalid byte-budget configuration.
- FIFO and normal close/drain behavior; push on closed returns false.
- `close()` wakes blocked producer and blocked consumer.
- Cancellation wakes blocked producer and blocked consumer.
- Multiple producers and consumers deliver every item exactly once.
- Byte budget blocks even when item capacity remains, and releases on pop.
- One individually oversized item is rejected without deadlock.
- Throwing size calculation / element move leaves queue usable and accounting correct.

## Watchpoints

- Use `condition_variable_any` stop-token waits; no polling or sleeps.
- Notify waiters only after state is coherent under the single queue mutex.
- Never move from the caller's value until capacity is reserved and insertion can proceed.
- `close()` is idempotent and wakes both conditions.
- Keep the interface small enough for Packet C to use directly.
