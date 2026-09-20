# M3 Orchestrator — Fixed-size chunks, BLAKE3, and zstd

Spec: [../milestones/M3-chunking-blake3-zstd.md](../milestones/M3-chunking-blake3-zstd.md) · Loop: [00-COMMON.md](00-COMMON.md)
Reading set: Part 04 (§13) · 05 (§18) · 06 (§19.3) · 07 (§26.5) · 09 (§32.2/33.3) · 11 (§42.1–42.6)

## Fix first (orchestrator)

- The invariants set here are permanent format: `object_id = BLAKE3(raw bytes)`, `objects/<2-hex>/<hash>.zst`, one zstd frame per chunk (§13). Every brief states them.
- Still single-threaded — no stripe locks, but publish sequence must be §18.6's recheck-then-publish (M5 wraps it).

## Packets

| # | Packet | Scope | Order | Class |
|---|---|---|---|---|
| A | BLAKE3 wrapper | `Blake3Hasher` (§42.1) + official-vector battery | parallel | standard |
| B | zstd wrapper | `ZstdCodec` (§42.2, §18.3), decompress bounds (§26.5) + battery | parallel | standard |
| C | Chunker | ≤4 MiB streaming reads (§18.1) + §32.2 battery | parallel | tricky |
| D | ObjectStore | hash-derived paths (§13.3, §42.6), publish §18.6, dedup lookup §18.4 + battery | after A/B | tricky |
| E | Integration | dual hashing §18.2, ordered `entry_chunks`, restore verification §19.3; dataset generator large profile (§33.3) | after all | critical |

## Watchpoints (put in briefs)

- **Hash raw bytes, before compression — always.** The single most damaging mistake available (§13.5).
- Exactly-4 MiB file = one chunk; final zero-byte read emits nothing.
- Decompress allocates exactly `raw_size` from DB, checked against repo chunk size — never trust the frame header.
- Object path = pure function of hash; no timestamps or nondeterminism.
- Incompressible data grows — expected; store the frame anyway; no ad-hoc raw mode.
- Existing hash with mismatched `raw_size` → corruption report (§18.7).

## Verify

- Fresh agent: checklist → `docs/implementation-logs/M3/`; FR-105–110 mapped; M2's NULL-`file_hash` allowance closed.
- Acceptance counts: unchanged second snapshot writes **zero** new objects (count `objects/` before/after).
- Multi-GB round-trip in bounded memory runs on Richard's Mac. No VM session needed.
