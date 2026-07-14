# M5 Packet C Brief — Pipeline Core

Class: critical. Run after Packet A is integrated. This is sequential pipeline-core work. Smallest
complete implementation; state assumptions explicitly and report a summary.

## Verbatim requirements

From §24.1:

> Scanner thread → `BoundedQueue<FileJob>` → Worker 1..N →
> `BoundedQueue<ProcessedEntry>` → Metadata writer thread.

From §24.4:

> `auto count = std::thread::hardware_concurrency();`
> `count = count == 0 ? 4 : count;`
> `count = std::clamp(count, 1U, 16U);`
>
> Allow configuration but enforce a safe maximum.

From §24.5:

> - One write connection owned by the metadata writer.
> - Read-only queries from the coordinating thread or separate read connections.
> - No SQLite connection is shared concurrently unless opened/configured for that use and protected.
> - Prepared statements are not shared across threads.

From §24.7:

> - The first fatal worker error is stored in a synchronized `std::exception_ptr`.
> - Request stop on all workers.
> - Close queues.
> - Join all threads.
> - Convert the original exception to a structured operation failure.
> - Do not allow worker exceptions to escape a thread function.

Milestone invariants:

> Preserve all M3 object-integrity and M4 crash-publication semantics; wrap them with concurrency
> rather than weakening or replacing them.
>
> On any fatal error or cancellation, close all queues before joining any thread.
>
> Capture only the first exception_ptr, request stop, close queues, join every thread, then rethrow
> the original error.
>
> Memory must remain bounded independently of the total number of files.

## File boundary

- Primary: `src/core/snapshot_engine.cpp`, `include/localvault/snapshot_engine.hpp` only for private
  queue/test configuration.
- Adapt `src/core/filesystem/file_scanner.hpp/.cpp` only enough to expose streaming emission; Packet
  D owns ignore/platform behavior.
- Adapt `src/core/storage/object_store.hpp/.cpp` only enough that workers never write SQLite and the
  metadata writer can insert/verify chunk rows. Packet E owns striped locking and instability.
- Add focused `tests/integration/m5_pipeline_test.cpp`; update CMake lists as needed.
- Preserve M3/M4 tests and public behavior. Do not implement D/E/F features here beyond interfaces
  strictly needed to compile the pipeline.

## Required tests

- Normative default worker function and explicit worker count clamp to 1..16.
- Real pipeline snapshot round trip with one scanner, requested workers, bounded queues, and writer.
- Writer is the only pipeline thread that calls SQLite write methods.
- Fatal scanner, worker, and writer failures preserve the first error, close both queues, join all
  threads, and leave no complete partial snapshot.
- External cancellation closes both queues before joins and leaves no complete partial snapshot.
- Existing M3 object-integrity and M4 failure/publication suites remain green.

## Watchpoints

- Deadlock pattern: worker blocked on a full result queue while writer exited. Every error/cancel path
  closes **all** queues before any join; push on closed returns false.
- Store only the first `exception_ptr`; shutdown fallout must not overwrite it.
- The `Repository` connection may be used by the coordinator before threads and after all joins, but
  during the pipeline it is exclusively owned/used by the metadata writer.
- Workers may publish immutable objects but must not call `MetadataStore` or touch SQLite.
- Scanner emission must be streaming; retaining the whole scan tree violates the memory invariant.
- Cancellation is cooperative only. Do not detach, terminate, or abandon a thread.
