# M5 independent verification

Date: 2026-07-14 (America/Los_Angeles)
Verifier role: fresh independent verifier; no implementation or repair performed.

## Result

- Milestone checklist: **21 PASS / 0 FAIL / 2 PENDING** (23 items total).
- Local automated gates: **PASS**; no local automated item remains pending. A first
  `ruff format --check` found two benchmark Python files needing formatting; Packet G made the
  narrow formatting-only repair, and the affected format/lint/AST/diff gates were independently
  rerun and passed.
- External/human supplement: **1 PENDING HUMAN** for the one-time Windows VM junction exercise.
- Exact blockers to milestone closure:
  1. **PENDING CI:** there is no pushed exact-head run for the current M5 worktree. The Linux
     ASan/UBSan job and the complete Linux/macOS/Windows workflow (including the Windows native
     junction fixture) must be green on the pushed M5 commit.
  2. **PENDING HUMAN:** Richard must perform the one-time Windows VM `mklink /J` loop exercise. An
     agent cannot self-certify it.

## Independently rerun gates

- Warning-strict development gate:
  `cmake --preset development -DLOCALVAULT_WARNINGS_AS_ERRORS=ON && cmake --build --preset development --parallel && ctest --preset development`.
  Configure/build passed; CTest registered 179 tests, disabled the manual profile-A test, and
  reported all 178 enabled tests non-failing in 30.57 s (177 passed; the opt-in M3 external-dataset
  test skipped).
- Focused M5 gate:
  `ctest --test-dir build/development --output-on-failure -R '(BoundedQueueTest|FileScanner(Test|DecisionTest)|IgnoreRulesTest|StableIdentityUsesOnly|ForcedSixteenWorkerDuplicateRace|EqualPreAndPostIdentity|OneIdentityChange|TwoIdentityChanges|DisappearingFile|PermissionAndSharing|ProgressIsBounded|ProgressCallbackFailure|CompleteProgress|UnstableWarning|FirstFailureCloses|NonStandardWorkerFailure|ExternalCancellationCloses|DuringScanExternalCancellation|CancellationDuringChunkProcessing|CancellationFromFinalizing|CancellationIsCheckedBeforeReading|PathOverloadChecksCancellation)'`.
  **50/50 passed in 7.63 s on the first run**. This includes scanner, worker, writer, and external
  cancellation shutdown origins; scan/chunk cancellation; duplicate race; and all requested
  queue/scanner/ignore/unstable/progress roles.
- ASan/UBSan: warning-strict sanitizer configure/build passed. `ctest --preset sanitizers`
  reproduced the host limitation before test bodies ran:
  `AddressSanitizer: detect_leaks is not supported on this platform.` The freshly built executable
  was then run as
  `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 build/sanitizers/tests/localvault_tests --gtest_brief=1`:
  **177 passed, one opt-in dataset test skipped, 178 total in 17.776 s**, with no ASan/UBSan
  diagnostics.
- TSan:
  `cmake --preset thread-sanitizer -DLOCALVAULT_WARNINGS_AS_ERRORS=ON && cmake --build --preset thread-sanitizer --parallel && ctest --preset thread-sanitizer`:
  the registered concurrency filter passed **19/19 in 32.83 s** with no TSan diagnostics.
- Quality/config gates: `clang-format --dry-run --Werror` passed all 82 tracked/untracked C/C++
  files. After the narrow Packet G formatting repair,
  `ruff format --check benchmarks/generate_dataset.py benchmarks/run_m5_profile_a.py`, `ruff check`
  on those two files, Python AST compilation, `git diff --check`, and the no-`__pycache__` check all
  passed. Independent JSON parsing passed 2 files; Ruby/Psych YAML parsing passed the workflow;
  trailing-whitespace inspection passed all 16 then-untracked files. `clang-tidy`, `scan-build`, and
  `cppcheck` are not installed on this host.
- Memory evidence was not wastefully regenerated. I inspected
  `benchmarks/run_m5_profile_a.py`, `benchmarks/generate_dataset.py`, and
  `benchmarks/m5_profile_a.cpp`: fixed seed 12345; exactly 50,000 files; 0..16 KiB; text, JSON,
  source, and deterministic binary payloads; deep and wide paths; 16 workers; `getrusage` peak RSS;
  fixed 512 MiB ceiling. Independent arithmetic confirms 187,641,280 logical bytes. The recorded
  fixed-seed Mac run in `implementation.md` measured **89,473,024-byte peak RSS** versus the
  **536,870,912-byte ceiling**, with all 50,000 files snapshotted.

## Critical-review closure and M4 amendment

The fresh critical reviewer initially blocked on nine issues: progress counters updated on the
writer; complete-callback publication retraction/visibility conflict; missing pre-open
cancellation; collecting-scan ordering regression; unbounded per-file chunk references/oversized
result rejection; unbounded object caches; raw non-standard exception escape; a Windows-incompatible
`std::tmpfile` repair attempt; and unnecessary per-file spool `fsync`. The repairs move processed
counters to workers, spool fixed-size chunk-reference records through repository
`TemporaryOutputFile`, cap each worker cache at 64, preserve canonical collecting order while the
pipeline remains streaming, structure unknown exceptions, and retain platform-compatible bounded
storage. The reviewer's final recheck reported no open code blocker and specifically closed
first-error store-once; stop plus both queues closed before joins; all fatal roles/external cancel;
writer-only SQLite; bounded queue/spool/cache; striped M3/M4 publication; pre-open cancellation; and
canonical collecting scan. Reviewer gates were 27/27 warning-strict M4/M5/ObjectStore, 36/36 broader
snapshot/chunker, 2/2 ASan/UBSan repaired cases, 2/2 TSan repaired cases, and 176 passed/one external
skip in the then-current full suite. The current independent full/focused/sanitizer gates above
corroborate the repaired tree.

The explicit M4 publication amendment is verified in `SnapshotEngine::create_snapshot`: a
`finalizing` callback failure or cancellation occurs before the publication transaction and is
fatal; the final `complete` callback runs only after the committed `complete` row, is best-effort,
and cannot retract a published snapshot or execute user code inside the SQLite publication
transaction. Proving tests are
`M5PipelineTest.CancellationFromFinalizingCallbackPreventsPublication`,
`M5PipelineTest.ProgressCallbackFailureStopsPipelineAndLeavesSnapshotIncomplete`, and
`M5PipelineTest.CompleteProgressCallbackFailureCannotRetractPublishedSnapshot`.

## FR-112 through FR-119 map

| Requirement | Decision/evidence | Proving tests |
|---|---|---|
| FR-112 — detect files modified while read | Pre/post `{size, mtime, ctime, file_id}` identity, at most one retry, then warning/skip; no mixed entry metadata. | `PlatformFileMetadataTest.StableIdentityUsesOnlySizeTimesAndFileId`; `SnapshotEngineTest.EqualPreAndPostIdentityCompletesAfterOneRead`; `SnapshotEngineTest.OneIdentityChangeRetriesAndStoresOnlySecondStableVersion`; `SnapshotEngineTest.TwoIdentityChangesWarnAndSkipWithoutEntryMetadata`; `SnapshotEngineTest.DisappearingFileWarnsAndSkipsWithoutMixedEntry` |
| FR-113 — cancellation without damaging completed snapshots | Cooperative pipeline stop closes both queues, joins all threads, cleans the cancelled snapshot, and preserves prior complete snapshots. | `M5PipelineTest.DuringScanExternalCancellationAfterObservedHundredEntriesCleansAndPreservesPriorSnapshot`; `SnapshotEngineTest.CancellationDuringChunkProcessingLeavesNoCompleteSnapshot`; `M5PipelineTest.CancellationFromFinalizingCallbackPreventsPublication`; `ChunkerTest.PathOverloadChecksCancellationBeforeOpeningSource` |
| FR-114 — `.localvaultignore` | **Assigned to M5 scanner work.** Root `.localvaultignore` is the default; explicit `SnapshotOptions::ignore_file` replaces it entirely and never merges. CLI spelling/parsing for `--ignore-file` remains M7; M5 proves the core replacement behavior. Negation is explicitly deferred/literal. | `IgnoreRulesTest.ExplicitFileReplacesRootRulesRatherThanMerging`; `IgnoreRulesTest.WildcardsStayWithinOnePathComponent`; `IgnoreRulesTest.DirectoryMatchSignalsThatRecursionCanBePruned`; `FileScannerTest.IgnoreRulesPruneDirectoriesBeforeEnumeratingTheirChildren`; `SnapshotEngineTest.StreamsHiddenAndReplacementIgnoreOptionsIntoScanner` |
| FR-115 — progress callback | Worker-updated atomics; coordinator throttling; totals absent during scan and exact afterward; callback publication boundary follows the M4 amendment above. | `M5PipelineTest.ProgressIsBoundedMonotonicAndPublishesExactTotalsAfterScanning`; `M5PipelineTest.ProgressCallbackFailureStopsPipelineAndLeavesSnapshotIncomplete`; `M5PipelineTest.CompleteProgressCallbackFailureCannotRetractPublishedSnapshot`; `M5PipelineTest.UnstableWarningDoesNotIncrementProcessedOrChunkCounters` |
| FR-116 — Windows sharing warning | Filesystem/open failures retry once, then warn and skip while the snapshot completes as partial success. | `SnapshotEngineTest.PermissionAndSharingSourceErrorsWarnAndSkip` |
| FR-117 — mount/junction safety | Pure decision function captures junctions as link entries without recursion and warns/skips volume mounts; Win32 no-follow reparse classification is present. Exact-head native Windows execution remains part of the pending CI item. | `FileScannerDecisionTest.FakedPlatformAttributesChooseSafeActions`; Windows CI: `FileScannerTest.NativeWindowsJunctionIsCapturedAndNeverTraversed` |
| FR-118 — cloud placeholders | Win32 recall attributes are classified without hydration and cause warn/skip. | `FileScannerDecisionTest.FakedPlatformAttributesChooseSafeActions` |
| FR-119 — one filesystem/volume | Scanner compares POSIX device or Windows volume identity to the root and applies the faked boundary decision before recursion. | `FileScannerDecisionTest.HiddenAndFilesystemBoundaryOptionsAreAppliedToFakeFacts` |

## Completion checklist

M5 is complete only when **every** box is checked. Copy this checklist into the verification log and check items there with evidence.

**Implementation**

- [x] `BoundedQueue`: capacity (and byte budget where results are large), blocking push/pop with stop_token support, `close()` semantics, no busy-waiting (§24.3, §42.4).
  - **PASS:** `src/core/concurrency/bounded_queue.hpp` uses one mutex and
    `condition_variable_any` stop-token waits; processed results have item and 64 MiB byte bounds.
    All nine `BoundedQueueTest.*` tests passed in the full and focused gates.
- [x] Pipeline wired: one scanner producer → job queue → worker pool → result queue → one metadata writer that owns the sole write connection (§24.1, §24.5).
  - **PASS:** `SnapshotEngine::create_snapshot` constructs exactly that topology; workers receive
    filesystem-only `ObjectStore`s and the writer owns `MetadataStore`/`Transaction`.
    `M5PipelineTest.BoundedRequestedWorkerPipelineRoundTripsWithOneWriterThread` and
    `M5PipelineTest.ChunkReferencesSpoolOutsideTinyResultQueueByteBudget` passed.
- [x] **Ignore rules implemented here** (`.localvaultignore` per §27, `--ignore-file` replaces it entirely) — the plan assigns them to the scanner, so they land in this milestone; record this mapping in the log.
  - **PASS:** `IgnoreRules::load` selects either the root file or explicit replacement, never both;
    the FR-114 map above records assignment/replacement and the M7 CLI-parsing boundary.
    `IgnoreRulesTest.ExplicitFileReplacesRootRulesRatherThanMerging` and scanner integration passed.
- [x] Scanner platform rules: junctions/mount points never traversed (decision function unit-testable), `one_file_system` honored, hidden-file option wired, cloud placeholders skipped with warnings (§25.4, §25.11).
  - **PASS:** no-follow platform facts feed `decide_scan_entry` before recursion. Local
    `FileScannerDecisionTest.*`, `FileScannerTest.HiddenEntriesAreIncludedByDefaultAndPrunedWhenDisabled`,
    and ignore-pruning tests passed. Native Windows execution is correctly reserved for CI/human
    gates below.
- [x] Worker count default per §24.4 (the single normative definition), configurable and clamped.
  - **PASS:** `default_worker_count` implements fallback 4 and clamp 1..16;
    `M5PipelineTest.NormativeWorkerDefaultAndConfiguredClamp` passed.
- [x] Striped-mutex duplicate-object protection wrapped around the M3 publish sequence (§18.5).
  - **PASS:** `ObjectStoreSynchronization` has 256 mutexes; `ObjectStore::store` locks by first hash
    byte across recheck and durable publication.
    `SnapshotEngineTest.ForcedSixteenWorkerDuplicateRacePublishesOneObjectAndOneChunkRow` passed
    locally and under TSan (one object, one chunk row, 1 new/15 reused).
- [x] Stop-token checks at every §24.6 point; cancellation is cooperative only.
  - **PASS:** scanner checks before directory scans/enumeration; chunker checks before path open and
    between chunks; workers check before compression/store; writer checks batches/chunk records.
    `ChunkerTest.CancellationIsCheckedBeforeReadingAndBetweenChunks`,
    `ChunkerTest.PathOverloadChecksCancellationBeforeOpeningSource`, and both M5 scan/chunk
    cancellation tests passed; no force termination/detach exists.
- [x] Unstable-file handling: pre/post `{size, mtime, ctime, file_id}` comparison, one retry, then skip with warning (§16.4–16.5), via a test-controllable seam.
  - **PASS:** platform metadata supplies all four identity components; worker attempts are capped at
    two. Equal/change-once/change-twice/disappearance/permission-and-sharing tests passed.
- [x] First fatal error captured in a synchronized `exception_ptr`; stop requested, queues closed, all threads joined, original error rethrown (§24.7).
  - **PASS:** common `shutdown` stores only the first error under a mutex, requests stop, closes both
    queues, then the coordinator joins scanner/workers/writer/progress before rethrow.
    All three parameterized fatal stages, external cancellation, and non-standard worker failure
    passed in the 50-test focused run and 19-test TSan run.
- [x] Progress: atomic counters, coordinator emits ≤10 events/second, `total_entries`/`total_bytes` filled once scanning completes (§24.8, §12.3).
  - **PASS:** `ProgressAggregator` uses atomic counters and a coordinator thread; exact total and
    throttle behavior passed in
    `M5PipelineTest.ProgressIsBoundedMonotonicAndPublishesExactTotalsAfterScanning`.

**Tests (all green)**

- [x] Queue battery: close-while-waiting, cancel-while-waiting, multiple producers/consumers, byte-budget enforcement.
  - **PASS:** all nine `BoundedQueueTest.*` tests passed, including
    `CloseWakesBlockedProducerAndConsumer`, `CancellationWakesBlockedProducerAndConsumer`,
    `MultipleProducersAndConsumersDeliverEachItemOnce`, and
    `ByteBudgetBlocksUntilPopEvenWithItemCapacity`.
- [x] Shutdown from every role: scanner error, worker error, writer error, external cancel — no deadlock, no lost first-error.
  - **PASS:** the three instances of
    `M5PipelineFailureTest.FirstFailureClosesBothQueuesJoinsAndCleansPartialSnapshot` plus
    `M5PipelineTest.ExternalCancellationClosesQueuesJoinsAndCleansPartialSnapshot` passed in the
    first focused run; unknown worker exceptions are also structured by
    `NonStandardWorkerFailureIsStructuredAndCleansPartialSnapshot`.
- [x] Forced duplicate-write race (identical-content dataset, max workers): one object file, one `chunks` row, correct counters.
  - **PASS:** `SnapshotEngineTest.ForcedSixteenWorkerDuplicateRacePublishesOneObjectAndOneChunkRow`
    passed in development, focused, ASan/UBSan, and TSan runs.
- [x] Ignore-rule battery (§32.2): comments, exact names, wildcards, directory pruning (no recursion into ignored dirs), nested paths, hidden files, spaces, case behavior.
  - **PASS:** all ten `IgnoreRulesTest.*` tests and
    `FileScannerTest.IgnoreRulesPruneDirectoriesBeforeEnumeratingTheirChildren` passed.
- [x] Unstable-file scenarios (§32.3): changes once → retried and stored; changes twice → skipped with warning; disappears; permission denied; snapshot still completes as partial success.
  - **PASS:** `OneIdentityChangeRetriesAndStoresOnlySecondStableVersion`,
    `TwoIdentityChangesWarnAndSkipWithoutEntryMetadata`,
    `DisappearingFileWarnsAndSkipsWithoutMixedEntry`, and
    `PermissionAndSharingSourceErrorsWarnAndSkip` passed and assert complete partial-success state.
- [x] Cancellation during scan and during chunk processing: no `complete` partial snapshot; recovery clean (acceptance).
  - **PASS:** `DuringScanExternalCancellationAfterObservedHundredEntriesCleansAndPreservesPriorSnapshot`
    gates on 100 observed entries and verifies recovery/prior snapshot;
    `CancellationDuringChunkProcessingLeavesNoCompleteSnapshot` verifies chunk cancellation. Both
    passed in development, focused, ASan/UBSan, and TSan runs.
- [x] Memory bound: many-small-files dataset (§33.2 profile A) snapshot stays under a fixed RSS ceiling (acceptance).
  - **PASS:** inspected deterministic runner/generator and cited the fixed-seed 50,000-file Mac
    result: 89,473,024-byte peak RSS below the fixed 536,870,912-byte ceiling; runner also asserts
    exactly 50,000 snapshotted files.
- [x] Progress test: totals are empty during scan, present and correct after.
  - **PASS:** `M5PipelineTest.ProgressIsBoundedMonotonicAndPublishesExactTotalsAfterScanning`
    passed in development/focused/ASan/UBSan/TSan and explicitly asserts the transition and exact
    final counters.

**Platform & CI**

- [ ] ASan/UBSan preset green locally on the Mac **and** on the Linux sanitizer CI job (acceptance).
  - **PENDING CI:** local Mac ASan/UBSan is PASS via the required direct-executable workaround
    (177 passed/one opt-in skip; no diagnostics). The workflow defines `linux-sanitizers`, but no
    pushed exact-head run exists for this uncommitted M5 worktree, so the required Linux half cannot
    yet be certified.
- [x] TSan run over the concurrency-focused tests (locally on the Mac and/or the Linux job) — clean (acceptance).
  - **PASS:** local Mac TSan preset filter passed 19/19 in 32.83 s with no diagnostics; the
    disjunctive local-and/or-CI requirement is satisfied. The workflow also defines `linux-tsan`.
- [ ] All three CI jobs green; the junction/one-file-system fixture tests pass on the Windows CI job.
  - **PENDING CI:** `.github/workflows/build-test.yml` parses and defines Linux, Linux sanitizers,
    Linux TSan, macOS, and Windows jobs, with full CTest on Windows. The M5 worktree is not yet a
    pushed exact head, so no corresponding all-green run or Windows-native
    `FileScannerTest.NativeWindowsJunctionIsCapturedAndNeverTraversed` result exists.

**Process**

- [x] Implementation + verification logs under `docs/implementation-logs/M5/`, including this checklist's state.
  - **PASS:** `docs/implementation-logs/M5/implementation.md` and this
    `docs/implementation-logs/M5/verification.md` contain the decision record, independent gates,
    complete checklist, review closure, amendments, and pending external state.
- [x] Log records FR-112–FR-119 and FR-114 with proving test names; notes the ignore-rules mapping decision.
  - **PASS:** the complete FR table above names tests for every FR-112..FR-119 item and explicitly
    records that M5 owns scanner ignore rules while an explicit file replaces rather than merges
    the root rules.

## One-time Windows VM gate

**PENDING HUMAN — cannot be self-certified by this verifier.** In an Administrator `cmd.exe` on a
clean Windows VM: `mkdir C:\lv-m5-junction\source`, then
`echo payload>C:\lv-m5-junction\source\file.txt`, then
`mklink /J C:\lv-m5-junction\source\loop C:\lv-m5-junction\source`. Confirm with
`fsutil reparsepoint query C:\lv-m5-junction\source\loop`, build with
`cmake --preset windows-development -DLOCALVAULT_WARNINGS_AS_ERRORS=ON` and
`cmake --build --preset windows-development-debug --parallel`, then run
`build\windows-development\tests\Debug\localvault_tests.exe --gtest_filter=FileScannerTest.NativeWindowsJunctionIsCapturedAndNeverTraversed`.
It must pass without recursion. Remove only the junction with
`rmdir C:\lv-m5-junction\source\loop` (never `rmdir /S` through the loop), then remove the fixture.
