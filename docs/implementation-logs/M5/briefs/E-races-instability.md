# M5 Packet E Brief — Duplicate Races and Unstable Files

Class: critical. Run after Packet C and scanner integration. Sequential pipeline-core work. Smallest
complete implementation; state assumptions explicitly and report a summary.

## Verbatim requirements

From §18.5:

> Two worker threads may discover the same new hash simultaneously. Prevent duplicate publication
> with a striped mutex table: `std::array<std::mutex, 256> object_mutexes;` and select by the first
> hash byte.
>
> Inside the selected mutex:
> 1. Recheck whether the object already exists.
> 2. If absent, write and publish it.
> 3. Insert or verify the database `chunks` row through the metadata writer.

From §16.4:

> `before = {size, mtime_ns, ctime_ns, file_id}`
> `read and process file`
> `after  = {size, mtime_ns, ctime_ns, file_id}`
>
> Treat the file as unstable when size, modification time, change time, or file identifier changed;
> the read ended earlier than expected; or the file disappeared. Retry once by default. Do not
> silently store a mixture of versions.

From §16.5:

> Permission denied, sharing violation, file disappeared, and file changed repeatedly are non-fatal
> per-entry warnings. A snapshot with warnings may still be complete, but the result is partial.

## File boundary

- Duplicate protection: `src/core/storage/object_store.hpp/.cpp` plus a small shared stripe owner if
  necessary; preserve the complete M3/M4 durability sequence.
- Stable identity: platform `file_metadata` interface/implementations and worker code in
  `snapshot_engine.cpp`.
- Add a private test-controllable hook through existing `SnapshotEngineTestAccess`; do not expose a
  production mutation callback.
- Add focused race and unstable-file integration tests; update CMake as needed.
- Do not alter scanner ignore/platform policy or progress throttling.

## Required tests

- Forced identical-content race with configured 16 workers and a delay hook inside the selected
  stripe: exactly one immutable object, one `chunks` row, correct new/reused counters.
- Pre/post identity equal succeeds; changes once retries and stores the second stable version.
- Changes twice warns/skips; disappears warns/skips; permission denied warns/skips; Windows sharing
  violation warns/skips. Snapshot completes as partial success with no mixed entry metadata.
- Cancellation during chunk processing is cooperative and leaves no complete partial snapshot.
- Existing M3 corruption/orphan adoption and M4 crash-publication tests stay green.

## Watchpoints

- Hold the stripe across recheck then the existing M4 durable publication sequence; never replace
  that sequence.
- SQLite `chunks` insertion remains on the sole metadata-writer thread.
- A failed first attempt must not leak entry/chunk mappings or counters into the result; durable
  orphan objects are allowed for later M6 GC.
- One retry means at most two complete reads. The seam must be deterministic, not timing-based.
- The duplicate test must force the window and is included in TSan; do not rerun-until-green.
