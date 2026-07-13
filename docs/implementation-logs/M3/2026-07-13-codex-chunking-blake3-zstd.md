# Milestone 3 Implementation Log

Date: 2026-07-13
Milestone: Fixed-size chunks, BLAKE3, and zstd

## Decisions

- Make the permanent object identity BLAKE3 of raw chunk bytes and derive only
  `objects/<2-hex>/<64-hex>.zst`; one `ZSTD_compress` call stores one frame, including when
  incompressible input grows.
- Bound repository chunk metadata to `(0, 4 MiB]`. `Chunker` reuses one buffer, emits no empty
  tail, and remains single-threaded; M5 still owns stripe locks.
- Make `ObjectStore` own publish-before-metadata ordering. A warm M3 cache rechecks immutable final
  file type/size; a cold miss checks authoritative SQLite plus the object. Publication performs the
  required final/DB recheck, writes and syncs a temp object, rechecks, then atomically publishes
  without replacement. Conflicting raw size is corruption.
- Allocate decompression output from the trusted expected raw size only after checking the
  repository limit. Compressed input is independently size-bounded and exactly one frame is
  required before BLAKE3 verification.
- Hash every regular file during the same chunk-read pass. Empty files finalize BLAKE3-empty with
  zero mappings. Snapshot completion rejects NULL regular-file hashes, explicitly closing M2's
  temporary allowance.
- Restore always verifies chunk BLAKE3, contiguous reconstructed size, and full-file BLAKE3 before
  syncing or publishing, even if the legacy request flag is false.
- Keep the multi-GB case opt-in for the ordinary suite. The deterministic large-files generator
  streams SHAKE-256 blocks and records generator version, seed, profile, file count, and logical
  bytes in its manifest.

## Functional requirements and proving tests

- **FR-105 — 4 MiB chunking:** `ChunkerTest.*`,
  `SnapshotRestoreAcceptanceTest.RestoresDeletedFixtureIntoNonemptyAlternateDestination`.
- **FR-106 — raw-chunk BLAKE3:** `Blake3HasherTest.*`,
  `ObjectStoreFixture.MapsStrictHashStoresOneZstdFrameAndReusesMetadata`, and
  `RestoreEngineTest.ValidZstdFrameWithWrongRawBytesFailsChunkHashBeforePublication`.
- **FR-107 — zstd for every stored chunk:** `ZstdCodecTest.*` and `ObjectStoreFixture.*`.
- **FR-108 — duplicate reuse:**
  `SnapshotRestoreAcceptanceTest.ReusesIdenticalChunksAcrossUnchangedCopyAndOneAlignedRegionChange`.
- **FR-109 — ordered mappings:**
  `SnapshotEngineTest.StoresCompleteFixtureWithOrderedVerifiedZstdChunksAndFileHashes`.
- **FR-110 — streaming full-file BLAKE3:** the same SnapshotEngine test,
  `StoreFixture.CannotCompleteSnapshotWhileARegularFileHashIsNull`, and
  `RestoreEngineTest.MissingInvalidAndWrongFileHashesFailEvenWhenFinalVerificationFlagIsFalse`.

## Local evidence

- Warning-strict development configure and serial build passed; development CTest passed all 103
  discovered tests, with only the explicitly opt-in large test skipped.
- Focused acceptance, deduplication, corruption, and object-store run passed 13/13.
- ASan/UBSan direct binary passed 102 tests with the large test skipped and no diagnostic, using
  `ASAN_OPTIONS=detect_leaks=0` because Apple ASan does not support the preset's leak detection.
- Generated 2 GiB large-files profile (`seed=12345`) round-tripped byte-for-byte in 170.12 seconds;
  `/usr/bin/time -l` reported maximum RSS 17,006,592 bytes.
- The large-file test validates manifest version/profile/seed presence/file count/logical bytes against
  the supplied dataset. A separate 8 MiB + 17 byte same-seed generator check produced identical
  output (`sha256=c1a7c17a8372f87feea0addf9dc9703a54dad5e8968dc950c1f7b253587b0e1b`).
- A fresh critical invariant review reported zero blockers. Its suggestion to make ordered
  reconstruction observable was applied by giving each region of the ordinary >8 MiB fixture
  distinct content; the focused acceptance and full suites remained green.
- Full clang-format dry run, Ruff checks, Python syntax check, and `git diff --check` passed.

## Gotchas for later milestones

- macOS `/tmp` is a symlink; restore tests must start from the canonical temporary root or path
  safety correctly rejects the ancestor.
- The M3 cache's SQLite bypass relies on the current single-threaded/no-GC operation. M5 must add
  synchronization without weakening final-file rechecks or atomic no-replace publication.
