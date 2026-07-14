# M3 Independent Verification Log

Date: 2026-07-13
Implementation baseline: `8cbfe0eca3b368999f2138c2323290e5a048ef8d`. Verified M3 implementation head: `afa9eccb216cb4277d1ed07fc0531102e1697cdd`.

## Verdict

The integrated M3 implementation passes every implementation, test, platform/CI, and process item. There are no implementation blockers and no pending gates.

Fresh verifier evidence:

- `ctest --test-dir build/development --output-on-failure`: 103 discovered, 102 passed, and only the opt-in `M3LargeFileRoundTripTest.ExternalDatasetRoundTripsWithBoundedChunks` skipped.
- Focused acceptance/dedup/corruption/object-store CTest filter: 13/13 passed.
- `ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 build/sanitizers/tests/localvault_tests --gtest_brief=1`: 102 passed and the opt-in large test skipped, with no ASan/UBSan diagnostic. Leak detection is disabled because Apple ASan does not support the preset behavior.
- The development cache has `LOCALVAULT_WARNINGS_AS_ERRORS=ON`; the tested binary is newer than every integrated M3 source/test edit. The implementation log records the warning-strict configure/build passing.
- All repository C++ files passed `clang-format --dry-run --Werror`; `ruff check .`, `git diff --check`, and the focused generator check passed.
- GitHub Actions [`build-and-test` run 29295154368](https://github.com/ASDFGHJKLZXC123/FileVault/actions/runs/29295154368) completed successfully for the exact M3 head `afa9eccb216cb4277d1ed07fc0531102e1697cdd`. Linux job `86966896783`, Windows job `86966896785`, and macOS job `86966896808` all completed successfully with Configure, Build, and Test steps green; their logs report 0 failed tests (Linux 103, Windows 99, macOS 103).
- Initial run [`29294460533`](https://github.com/ASDFGHJKLZXC123/FileVault/actions/runs/29294460533) exposed MSVC warning C4996 for `getenv`, promoted to build-stopping C2220 by warnings-as-errors. Commit `afa9ecc` fixes the Windows environment lookup with `_dupenv_s`; its patch and the successful replacement run independently confirm the fix.
- The implementation log records the Mac 2 GiB `large-files`, seed 12345 round trip in 170.12 seconds with maximum RSS 17,006,592 bytes. The verifier corroborated the streaming generator and manifest checks in `benchmarks/generate_dataset.py:41-80` and the >=2 GiB, byte-identical opt-in test in `tests/integration/m3_large_file_roundtrip_test.cpp:80-171`. It also records the same-seed 8 MiB+17 deterministic sample SHA-256 `c1a7c17a8372f87feea0addf9dc9703a54dad5e8968dc950c1f7b253587b0e1b`.

## Permanent invariant audit

PASS. `ObjectStore::store` hashes raw bytes before its single `ZSTD_compress` call (`src/core/storage/object_store.cpp:223-245`), and strict lowercase hash validation derives only `objects/<first-two>/<full-hash>.zst` (`src/core/storage/object_store.cpp:25-37`). `ZstdCodec::decompress` bounds compressed input, allocates from the trusted expected raw size, enforces the repository maximum, and rejects anything other than exactly one frame (`src/core/storage/zstd_codec.cpp:41-75`).

PASS. `Chunker` performs ordered reads no larger than 4 MiB and emits no empty final chunk (`src/core/storage/chunker.cpp:12-84`). Snapshotting feeds the same raw span to the running file hasher and chunk object store, writes ordered offsets, and finalizes even an empty file (`src/core/snapshot_engine.cpp:188-235`). Restore validates contiguous mappings and logical size, verifies every object BLAKE3, then checks reconstructed size and full-file BLAKE3 before sync/publication (`src/core/restore_engine.cpp:474-502,567-575,623-667`).

PASS. Dedup uses the in-process cache, then the authoritative chunk row/object, rejects conflicting `raw_size`, and adopts only fully verified orphan objects (`src/core/storage/object_store.cpp:153-220`). Publication preserves initial lookup, temp write plus sync, immediate recheck, and atomic no-replace publish (`src/core/storage/object_store.cpp:237-283`; macOS `RENAME_EXCL` at `src/core/filesystem/platform/posix_file_metadata.cpp:255-284`). RAII removes a losing or failed temp. M3 remains single-threaded with no stripe locks (`src/core/storage/object_store.hpp:52-57`).

## Completion checklist

### Implementation

- [x] **PASS** — `Blake3Hasher` wrapper: incremental updates, 32-byte digest, lowercase 64-char hex (§42.1).
  Evidence: `src/core/storage/blake3_hasher.hpp:11-27`, `src/core/storage/blake3_hasher.cpp:12-57`, and all six `Blake3HasherTest.*` cases pass.

- [x] **PASS** — `ZstdCodec`: compress with `ZSTD_compressBound` sizing and `ZSTD_isError` checks; decompress enforces expected raw size and a maximum bound (§42.2, §18.3, §26.5).
  Evidence: `src/core/storage/zstd_codec.cpp:13-75`; all seven `ZstdCodecTest.*` cases pass.

- [x] **PASS** — `Chunker`: ≤4 MiB reads, short final chunk, exact-multiple files produce no trailing empty chunk, empty file produces zero chunks, offsets/lengths correct (§18.1).
  Evidence: `src/core/storage/chunker.cpp:12-84`; all six `ChunkerTest.*` cases pass.

- [x] **PASS** — Single read pass feeds both the chunk hasher and the running full-file hasher (§18.2); `file_hash` now stored for **every** regular file, including BLAKE3-of-empty for empty files.
  Evidence: `src/core/snapshot_engine.cpp:204-235`; `SnapshotEngineTest.StoresCompleteFixtureWithOrderedVerifiedZstdChunksAndFileHashes` checks every regular hash and the official empty digest.

- [x] **PASS** — Object path is a pure function of the hash with strict hash validation (§13.3, §42.6); shard directories created on demand.
  Evidence: `src/core/storage/object_store.cpp:25-37,62-74,148-150`; `ObjectStoreFixture.MapsStrictHashStoresOneZstdFrameAndReusesMetadata` checks mapping, validation, and shard creation.

- [x] **PASS** — `ObjectStore` publish sequence: recheck final path and DB → write temp → publish; duplicate hash reuses the existing object and deletes the temp (§18.6, single-threaded form — stripes come in M5).
  Evidence: `src/core/storage/object_store.cpp:186-283`; `ObjectStoreFixture.FailedMetadataInsertCleansTempAndOrphanRequiresFullVerification` and `ObjectStoreFixture.AtomicNoReplaceDuplicateOutcomePreservesImmutableFinal` pass.

- [x] **PASS** — Ordered `entry_chunks` rows with `raw_offset`/`raw_length`; sum of lengths equals `logical_size`.
  Evidence: insertion and ordered query are at `src/core/database/metadata_store.cpp:371-383,417-436`; snapshot/restore validate offsets and sums. `StoreFixture.StoresAndListsOrderedChunkRelationships` and the SnapshotEngine integration test pass.

- [x] **PASS** — Restore verifies every chunk hash, the reconstructed size, and the full-file hash (§19.3, §42.8).
  Evidence: `src/core/storage/object_store.cpp:286-340` and `src/core/restore_engine.cpp:623-667`; the five focused restore corruption/layout tests pass before publication.

- [x] **PASS** — Dedup lookup: in-process hash cache, then DB/object existence (§18.4); existing hash with mismatched `raw_size` reports corruption (§18.7).
  Evidence: `src/core/storage/object_store.cpp:153-220`; `ObjectStoreFixture.ColdCacheChecksAuthoritativeRowAndRegularFinalFile` changes `raw_size` and observes corruption, while cache and cold-cache reuse pass.

### Tests (all green)

- [x] **PASS** — Chunker battery (§32.2): empty, 1 byte, <4 MiB, exactly 4 MiB, 4 MiB+1, multiple chunks, mid-file read error, offsets/lengths, cancellation between chunks.
  Evidence: the six named `ChunkerTest.*` tests cover the complete list and pass in fresh CTest.

- [x] **PASS** — BLAKE3 battery: official vectors, incremental == one-shot, empty input, binary with zero bytes, hex format.
  Evidence: `Blake3HasherTest.MatchesOfficialVectors`, `IncrementalHashMatchesOneShotAboveFourMiB`, `AcceptsEmptyAndBinaryInputContainingZeroBytes`, and `HexEncodingIsExactlyLowercase` pass.

- [x] **PASS** — zstd battery: text/binary round trips, incompressible data, max chunk size, invalid input, truncated frame, raw-size mismatch.
  Evidence: all seven `ZstdCodecTest.*` tests pass, including one-frame enforcement and safety-bound rejection.

- [x] **PASS** — Object-store battery: path mapping, first write, reuse, temp cleanup after failure, invalid hash rejected, missing object, corrupt object.
  Evidence: all six `ObjectStoreFixture.*` tests pass; together they also cover cold-cache DB validation, orphan verification, one-frame storage, incompressible growth, and atomic no-replace.

- [x] **PASS** — Acceptance: a >8 MiB (multi-chunk) file restores byte-identically.
  Evidence: `SnapshotRestoreAcceptanceTest.RestoresDeletedFixtureIntoNonemptyAlternateDestination` uses 8 MiB+17 bytes with distinct chunk regions and passes streaming tree comparison.

- [x] **PASS** — Acceptance: two identical files in one snapshot share one object set.
  Evidence: `SnapshotRestoreAcceptanceTest.ReusesIdenticalChunksAcrossUnchangedCopyAndOneAlignedRegionChange` compares all three hashes of the original and copied files and observes no new object.

- [x] **PASS** — Acceptance: an unchanged second snapshot writes **zero** new content objects (count files under `objects/` before/after).
  Evidence: the same acceptance test asserts `new_chunks == 0`, `new_stored_bytes == 0`, and an unchanged `.zst` object count on the second snapshot.

- [x] **PASS** — Copying a large file adds logical bytes but no new chunks; modifying one aligned region stores only the affected chunks (§32.3 dedup list).
  Evidence: the same acceptance test asserts the copy adds 12 MiB logical size with zero objects, then a 4 MiB aligned edit adds exactly one object and changes only chunk index 1.

- [x] **PASS** — A multi-GB deterministic file (dataset script, §33.3 — at least the large-file profile must exist now) round-trips on the Mac in bounded memory.
  Evidence: the recorded 2 GiB seed-12345 Mac run restored byte-identically in 170.12 seconds at 17,006,592-byte maximum RSS; the generator and opt-in test enforce profile/manifest provenance, >=2 GiB, bounded chunk metadata, and tree equality.

### Platform & CI

- [x] **PASS** — Mac local suite green; all three CI jobs green. No VM session required.
  Evidence: Mac warning-strict build, development CTest, focused CTest, ASan/UBSan, formatting, Ruff, and diff checks are green. GitHub Actions run `29295154368` is completed/success on exact commit `afa9eccb216cb4277d1ed07fc0531102e1697cdd`: Linux job `86966896783`, Windows job `86966896785`, and macOS job `86966896808` each have successful Configure, Build, and Test steps. The initial Windows C4996 `getenv` failure in run `29294460533` was corrected with `_dupenv_s` in `afa9ecc`, and the replacement run is green.

### Process

- [x] **PASS** — Implementation + verification logs under `docs/implementation-logs/M3/`, including this checklist's state.
  Evidence: `2026-07-13-codex-chunking-blake3-zstd.md` and this independent verifier log are present under M3.

- [x] **PASS** — Log records FR-105–FR-110 satisfied, with proving test names; notes that M2's "file_hash may be NULL" allowance is now closed.
  Evidence: mappings and closure are recorded below.

## FR-105–FR-110 mapping

- **FR-105 — 4 MiB chunking: PASS.** `ChunkerTest.*`; `SnapshotRestoreAcceptanceTest.RestoresDeletedFixtureIntoNonemptyAlternateDestination`.
- **FR-106 — BLAKE3 of raw chunks: PASS.** `Blake3HasherTest.*`; `ObjectStoreFixture.MapsStrictHashStoresOneZstdFrameAndReusesMetadata`; `RestoreEngineTest.ValidZstdFrameWithWrongRawBytesFailsChunkHashBeforePublication`.
- **FR-107 — zstd for every new chunk: PASS.** `ZstdCodecTest.*`; all `ObjectStoreFixture.*`, especially `AcceptsIncompressibleFrameEvenWhenItGrows`.
- **FR-108 — duplicate reuse: PASS.** `SnapshotRestoreAcceptanceTest.ReusesIdenticalChunksAcrossUnchangedCopyAndOneAlignedRegionChange`; object-store warm/cold cache tests.
- **FR-109 — ordered chunk list: PASS.** `SnapshotEngineTest.StoresCompleteFixtureWithOrderedVerifiedZstdChunksAndFileHashes`; `StoreFixture.StoresAndListsOrderedChunkRelationships`; `RestoreEngineTest.RejectsMalformedChunkLayoutsBeforePublication`.
- **FR-110 — streaming full-file BLAKE3: PASS.** `SnapshotEngineTest.StoresCompleteFixtureWithOrderedVerifiedZstdChunksAndFileHashes`; `StoreFixture.CannotCompleteSnapshotWhileARegularFileHashIsNull`; `RestoreEngineTest.MissingInvalidAndWrongFileHashesFailEvenWhenFinalVerificationFlagIsFalse`.

## M2 allowance closure

M2's temporary allowance for a complete snapshot regular file to have NULL `file_hash` is closed. A regular entry may be NULL only as transient state while its snapshot is pending. `SnapshotEngine` always finalizes a full-file BLAKE3, including BLAKE3-empty with zero chunks; `MetadataStore::mark_snapshot_complete` rejects any pending snapshot containing a regular file with NULL `file_hash` (`src/core/database/metadata_store.cpp:183-207`); and restore rejects missing or malformed regular-file hashes before publication. `StoreFixture.CannotCompleteSnapshotWhileARegularFileHashIsNull` proves the completion gate.

## Totals and gates

- PASS: **21**
- FAIL: **0**
- PENDING: **0**
- Implementation blockers: **none**
- Pending gates: **none**

Final independent verdict: **M3 PASS**.
