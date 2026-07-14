# M5 implementation decision record

## Decisions and requirement map

- M5 owns scanner ignore rules: root `.localvaultignore` is used by default; an explicit ignore
  file **replaces, never merges with**, it. `--ignore-file` CLI parsing remains an M7 assumption;
  M5 exposes only the core option/loading behavior. Negation remains deferred.
- The M4 publication boundary is unchanged. `finalizing` callback failure is fatal before the
  publication transaction; the final `complete` callback runs after commit, is best-effort, and
  cannot retract a valid snapshot or execute user code inside SQLite publication.
- File jobs and processed entries use item-bounded queues; processed entries also have a 64 MiB
  byte bound. Chunk-reference lists spool to temporary files rather than inflating the result queue.
  Each worker's recent-object cache is fixed at 64 items and clears at its limit. Thus queues,
  spool memory, cache, chunk buffers, and the 1..16 worker clamp are independent of file count.

| Requirement | Proving tests |
|---|---|
| FR-112 unstable detection | `SnapshotEngineTest.OneIdentityChangeRetriesAndStoresOnlySecondStableVersion`; `SnapshotEngineTest.TwoIdentityChangesWarnAndSkipWithoutEntryMetadata`; `SnapshotEngineTest.DisappearingFileWarnsAndSkipsWithoutMixedEntry` |
| FR-113 cancellation | `M5PipelineTest.DuringScanExternalCancellationAfterObservedHundredEntriesCleansAndPreservesPriorSnapshot`; `SnapshotEngineTest.CancellationDuringChunkProcessingLeavesNoCompleteSnapshot`; `M5PipelineTest.CancellationFromFinalizingCallbackPreventsPublication` |
| FR-114 ignore rules | `IgnoreRulesTest.ExplicitFileReplacesRootRulesRatherThanMerging`; `IgnoreRulesTest.WildcardsStayWithinOnePathComponent`; `FileScannerTest.IgnoreRulesPruneDirectoriesBeforeEnumeratingTheirChildren` |
| FR-115 progress | `M5PipelineTest.ProgressIsBoundedMonotonicAndPublishesExactTotalsAfterScanning`; `M5PipelineTest.ProgressCallbackFailureStopsPipelineAndLeavesSnapshotIncomplete`; `M5PipelineTest.CompleteProgressCallbackFailureCannotRetractPublishedSnapshot` |
| FR-116 Windows sharing warning | `SnapshotEngineTest.PermissionAndSharingSourceErrorsWarnAndSkip` |
| FR-117 mount/junction safety | `FileScannerDecisionTest.FakedPlatformAttributesChooseSafeActions`; Windows CI: `FileScannerTest.NativeWindowsJunctionIsCapturedAndNeverTraversed` |
| FR-118 cloud placeholders | `FileScannerDecisionTest.FakedPlatformAttributesChooseSafeActions` |
| FR-119 one filesystem | `FileScannerDecisionTest.HiddenAndFilesystemBoundaryOptionsAreAppliedToFakeFacts` |

## Acceptance evidence (Mac host)

- Warning-strict: `cmake --preset development -DLOCALVAULT_WARNINGS_AS_ERRORS=ON`; `cmake
  --build --preset development --parallel`; `ctest --preset development` — 178/178 enabled tests
  were non-failing in 21.86 s (177 passed; the opt-in M3 dataset test skipped); the registered
  manual profile-A test was disabled. The required focused `ctest --test-dir build/development --output-on-failure -R
  '(BoundedQueueTest|FileScanner(Test|DecisionTest)|IgnoreRulesTest|ForcedSixteenWorkerDuplicateRace|EqualPreAndPostIdentity|OneIdentityChange|TwoIdentityChanges|DisappearingFile|PermissionAndSharing|ProgressIsBounded|ProgressCallbackFailure|CompleteProgress|UnstableWarning|FirstFailureCloses|NonStandardWorkerFailure|DuringScanExternalCancellation|CancellationDuringChunkProcessing|CancellationFromFinalizing)'`
  passed 46/46 in 6.73 s.
- Profile A: release build, seed 12345, exactly 50,000 mixed text/JSON/source/binary files from
  0..16 KiB in deep+wide paths, 16 workers. `/usr/bin/time -l
  /Users/f8fq/.local/bin/python3.11
  benchmarks/run_m5_profile_a.py --generator benchmarks/generate_dataset.py --benchmark
  build/release/benchmarks/localvault_m5_profile_a` reported 187,641,280 logical bytes, 24.9349 s
  snapshot wall, and 89,473,024-byte peak RSS versus the fixed 536,870,912-byte (512 MiB) ceiling;
  complete generator/run wall was 38.47 s. The ceiling is deliberately generous: it catches growth
  with entry count without presenting this single-host run as a performance claim.
- TSan is separate from ASan/UBSan: `VCPKG_ROOT=/Users/f8fq/tools/vcpkg cmake --preset thread-sanitizer
  -DLOCALVAULT_WARNINGS_AS_ERRORS=ON`; build; `ctest --preset thread-sanitizer` — its 19-test queue,
  duplicate, progress, cancellation, and role-error filter passed in 42.18 s with no diagnostics.
  Linux CI now has independent TSan and ASan/UBSan gates while retaining Linux/macOS/Windows jobs.
- Mac gotcha: `ctest --preset sanitizers` aborts before tests with `AddressSanitizer: detect_leaks is
  not supported on this platform.` The freshly built executable was therefore run exactly as
  `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
  build/sanitizers/tests/localvault_tests`: 177 passed, one opt-in dataset test skipped, no sanitizer
  diagnostics (178 total, 54.672 s).
- Quality gates passed: all tracked/untracked C++ via `clang-format --dry-run --Werror`, Ruff and
  Python AST parsing, workflow YAML parsing, preset parsing, `git diff --check`, and intentional
  diff audit. `clang-tidy`, `scan-build`, and `cppcheck` were not installed on this host.

## Windows VM human gate — pending, not self-certified

In an Administrator `cmd.exe` on a clean Windows VM: `mkdir C:\lv-m5-junction\source`, then
`echo payload>C:\lv-m5-junction\source\file.txt`, then `mklink /J
C:\lv-m5-junction\source\loop C:\lv-m5-junction\source`. Confirm with `fsutil reparsepoint query
C:\lv-m5-junction\source\loop`, build `cmake --preset windows-development
-DLOCALVAULT_WARNINGS_AS_ERRORS=ON` and `cmake --build --preset windows-development-debug
--parallel`, then set `set LOCALVAULT_M5_JUNCTION_LOOP_SOURCE=C:\lv-m5-junction\source`. Run the
named test with a 30-second watchdog:
`powershell -NoProfile -Command "$p=Start-Process -FilePath
'build\windows-development\tests\Debug\localvault_tests.exe' -ArgumentList
'--gtest_filter=FileScannerTest.NativeWindowsJunctionIsCapturedAndNeverTraversed' -NoNewWindow
-PassThru; if(-not $p.WaitForExit(30000)){$p.Kill(); throw 'junction-loop scan did not terminate'};
exit $p.ExitCode"`. It must pass and terminate without recursing into `loop\file.txt`. Remove only
the junction with `rmdir C:\lv-m5-junction\source\loop` (never `rmdir /S` through the loop), then
remove the fixture. **Human result: pending.**
