# Milestone 3 — Fixed-size chunks, BLAKE3, and zstd

> Part of the LocalVault build plan ([index](../00-INDEX.md)). Spec source: guide §37.
> **Have these open — the complete reading set for this milestone:**
> [Part 04](../04-repository-format-and-database.md) (§13: object IDs, paths, payload, invariants) ·
> [Part 05](../05-snapshot-engine.md) (§18: chunking, hashing, compression, dedup, object writes) ·
> [Part 06](../06-restore-diff-verify-gc.md) (§19.3: chunk-verified file reconstruction on restore) ·
> [Part 07](../07-consistency-concurrency-platform.md) (§26.5: allocation/resource limits on decompress) ·
> [Part 09](../09-quality-testing-ci-packaging.md) (§32.2: unit-test batteries · §33.3: dataset generator) ·
> [Part 11](../11-appendix-skeletons.md) (§42.1–42.6: hasher/codec/path skeletons).
> Nothing else is required; no other milestone file is ever needed.

## Role in the overall project

M3 turns the walking skeleton into the real product idea: content-addressed, deduplicated, compressed storage. Files become ordered lists of 4 MiB chunks; identical content collapses to one stored object; objects live at hash-derived paths. After this milestone the repository format is doing its actual job — everything later (crash-safety, concurrency, GC) protects and exploits what M3 creates.

## Why it matters

- **The invariants set here are permanent.** `object_id = BLAKE3(raw bytes)`, `objects/<2-hex>/<hash>.zst`, one zstd frame per chunk (§13). Objects written by M3 must remain readable by every future version.
- Deduplication is the product's core value claim ("reuse chunks already present"). Its correctness — same bytes, same object, different bytes, different object — is established here and merely *raced* later (M5).
- The verification chain (chunk hash on read, full-file hash after reconstruction) added here is what makes every future restore self-checking.

## How it works

1. `Chunker` streams files in ≤4 MiB reads; last chunk short; empty file = zero chunks (§18.1).
2. Every raw chunk feeds two hashers: its own chunk hash and the running full-file hash (§18.2) — hash **raw** bytes, never compressed bytes.
3. New chunks compress (zstd level from repo config, `ZSTD_compressBound` buffer, `ZSTD_isError` check) and store via the ObjectStore write path; known hashes are reused (in-memory cache → DB/object check, §18.4).
4. Restore now reads ordered `entry_chunks`, decompresses with the expected-size bound, verifies each chunk hash, then the whole-file hash (§19.3).

## Specification (verbatim from §37)

Replace minimal object storage with:

- 4 MiB chunking.
- BLAKE3 chunk and file hashes.
- zstd compression.
- Hash-derived object paths.
- Ordered chunk mappings.
- Chunk verification.
- Duplicate reuse.

Acceptance:

- Large multi-chunk file restores correctly.
- Identical files share objects.
- Unchanged second snapshot writes no new content objects.

## Likely problems and confusions — with answers

1. **"Do I hash before or after compression?"** Before, always. The hash identifies *raw* bytes (§13.5). Hashing compressed bytes breaks dedup (zstd output isn't guaranteed stable across versions) and corrupts the format. This is the single most damaging mistake available in this milestone.
2. **"A file of exactly 4 MiB produces two chunks."** Off-by-one in the read loop: a final zero-byte read must not emit a chunk. The §32.2 chunker cases (exactly 4 MiB, 4 MiB+1) exist precisely for this.
3. **"'Unchanged second snapshot writes no new objects' fails."** Usually the acceptance test is right and the dedup lookup is wrong — but check the trivial cause first: your test rewrote the fixture files, changing mtimes *and* you're re-reading them (fine) but you stored `created_at` in the object path or similar nondeterminism. Object path must be a pure function of hash (§32.4 invariant).
4. **"Incompressible data grows when compressed."** Expected; zstd adds small framing overhead. Store the compressed frame anyway (v1 has no store-raw mode) and don't add one ad hoc — that's a format change.
5. **"Where do BLAKE3 test vectors come from?"** The official BLAKE3 repo's test vector JSON; embed a few (empty input, short, >4 MiB via repeated pattern) as test data (§32.2). Verify incremental == one-shot hashing.
6. **"Decompression allocates how much?"** Exactly `raw_size` from the DB, after checking it against the repository chunk size (§26.5). Never trust the frame header alone — a corrupt DB or object must not cause a giant allocation.
7. **"Do I need the striped mutex now?"** Not yet — M3 is still single-threaded, so the §18.5 stripe locks would be dead code. But implement ObjectStore's *recheck-then-publish* sequence (§18.6) now; M5 wraps it in stripes without restructuring.
8. **Mac-primary note:** all of this is portable code — full local development. One thing worth a VM/CI glance: the dedup acceptance tests exercise mtime precision, and Linux filesystems in CI will happily confirm ns-precision handling matches the Mac.

## Completion checklist

M3 is complete only when **every** box is checked. Copy this checklist into the verification log and check items there with evidence.

**Implementation**

- [ ] `Blake3Hasher` wrapper: incremental updates, 32-byte digest, lowercase 64-char hex (§42.1).
- [ ] `ZstdCodec`: compress with `ZSTD_compressBound` sizing and `ZSTD_isError` checks; decompress enforces expected raw size and a maximum bound (§42.2, §18.3, §26.5).
- [ ] `Chunker`: ≤4 MiB reads, short final chunk, exact-multiple files produce no trailing empty chunk, empty file produces zero chunks, offsets/lengths correct (§18.1).
- [ ] Single read pass feeds both the chunk hasher and the running full-file hasher (§18.2); `file_hash` now stored for **every** regular file, including BLAKE3-of-empty for empty files.
- [ ] Object path is a pure function of the hash with strict hash validation (§13.3, §42.6); shard directories created on demand.
- [ ] `ObjectStore` publish sequence: recheck final path and DB → write temp → publish; duplicate hash reuses the existing object and deletes the temp (§18.6, single-threaded form — stripes come in M5).
- [ ] Ordered `entry_chunks` rows with `raw_offset`/`raw_length`; sum of lengths equals `logical_size`.
- [ ] Restore verifies every chunk hash, the reconstructed size, and the full-file hash (§19.3, §42.8).
- [ ] Dedup lookup: in-process hash cache, then DB/object existence (§18.4); existing hash with mismatched `raw_size` reports corruption (§18.7).

**Tests (all green)**

- [ ] Chunker battery (§32.2): empty, 1 byte, <4 MiB, exactly 4 MiB, 4 MiB+1, multiple chunks, mid-file read error, offsets/lengths, cancellation between chunks.
- [ ] BLAKE3 battery: official vectors, incremental == one-shot, empty input, binary with zero bytes, hex format.
- [ ] zstd battery: text/binary round trips, incompressible data, max chunk size, invalid input, truncated frame, raw-size mismatch.
- [ ] Object-store battery: path mapping, first write, reuse, temp cleanup after failure, invalid hash rejected, missing object, corrupt object.
- [ ] Acceptance: a >8 MiB (multi-chunk) file restores byte-identically.
- [ ] Acceptance: two identical files in one snapshot share one object set.
- [ ] Acceptance: an unchanged second snapshot writes **zero** new content objects (count files under `objects/` before/after).
- [ ] Copying a large file adds logical bytes but no new chunks; modifying one aligned region stores only the affected chunks (§32.3 dedup list).
- [ ] A multi-GB deterministic file (dataset script, §33.3 — at least the large-file profile must exist now) round-trips on the Mac in bounded memory.

**Platform & CI**

- [ ] Mac local suite green; all three CI jobs green. No VM session required.

**Process**

- [ ] Implementation + verification logs under `docs/implementation-logs/M3/`, including this checklist's state.
- [ ] Log records FR-105–FR-110 satisfied, with proving test names; notes that M2's "file_hash may be NULL" allowance is now closed.
