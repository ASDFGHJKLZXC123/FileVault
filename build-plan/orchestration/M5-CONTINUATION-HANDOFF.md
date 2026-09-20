# M5 continuation handoff

Saved: 2026-07-18 (America/Los_Angeles)

## Current repository state

- Repository: `/Users/f8fq/dev/LocalVault`
- Branch: `main`
- `HEAD`: `e114307f837e43face77e102baadfea6a35311ba`
- `origin/main`: `e114307f837e43face77e102baadfea6a35311ba`
- Working tree was clean when this handoff was written. At that time, this file was under
  the gitignored `build-plan/` tree. The plan is now tracked on `Latest-Dev`; the repository
  state and verification results below remain a historical snapshot from the saved date.
- Exact-head GitHub Actions run:
  <https://github.com/ASDFGHJKLZXC123/FileVault/actions/runs/29376613145>
  - Linux: PASS
  - macOS: PASS
  - Windows: PASS
  - Linux ASan/UBSan: PASS
  - Linux TSan: PASS

## M5 status

M5 implementation, integration, critical shutdown-invariant review, repair, and fresh independent
verification are complete. Do not redo M4 or the completed M5 implementation packets.

Implemented and verified:

- One scanner producer, bounded job queue, worker pool, bounded result queue, and one metadata
  writer that owns the sole SQLite write connection.
- Item and byte queue bounds; stop-token-aware blocking operations; close wakeups; no busy-waiting.
- Cooperative cancellation and first-error shutdown ordering: capture first exception, request stop,
  close queues, join every thread, and rethrow the original error.
- Normative configurable/clamped worker count.
- M3/M4 object and crash-publication semantics retained, with 256 striped object-publish mutexes.
- Duplicate-content races produce one immutable object and one `chunks` row.
- Root `.localvaultignore` behavior and explicit ignore-file replacement behavior. M7 still owns CLI
  spelling/parsing for `--ignore-file`.
- Hidden-file, junction, mount-point, one-file-system, and cloud-placeholder scanner policies.
- Stable pre/post file identity, one retry, then warning/skip via a controllable test seam.
- Concurrent progress counters, at most 10 events/second, totals only after scanning completes.
- Bounded memory, including fixed-record chunk-reference spooling and capped caches.
- No M6 diff/verify/delete/GC functionality was added.

Key automated evidence:

- Warning-strict full local suite: all 178 enabled tests non-failing; one opt-in M3 external dataset
  test skipped; the manual 50,000-file test is intentionally disabled in routine CTest.
- Fresh focused independent verification: 50/50 M5 tests passed on the first run.
- Local TSan filter: 19/19 passed with no diagnostics.
- Mac ASan/UBSan: CTest reproduced unsupported leak detection; direct executable with
  `ASAN_OPTIONS=detect_leaks=0` passed 177 tests with one opt-in skip and no diagnostics.
- Profile A: exactly 50,000 files, seed 12345, 187,641,280 logical bytes, 16 workers, peak RSS
  89,473,024 bytes against the 536,870,912-byte ceiling.
- Critical review closed all nine findings; fresh independent verification has no pending automated
  gate.
- Exact-head CI at `e114307` is green for Linux, macOS, Windows, ASan/UBSan, and TSan.

Logs and briefs:

- `docs/implementation-logs/M5/implementation.md`
- `docs/implementation-logs/M5/verification.md`
- `docs/implementation-logs/M5/briefs/A-bounded-queue.md` through `G-acceptance.md`

The verification log still reflects its pre-push state (`21 PASS / 0 FAIL / 2 PENDING`) and says CI
is pending. That text must be updated after the human gate. The two automated checklist items are in
fact satisfied by run 29376613145; only the separate human gate remains unresolved.

## Sole remaining gate: human Windows junction-loop VM test

This gate must not be self-certified by an agent. No human PASS or FAIL has been reported yet.

The user began preparing a Windows VM but reported:

```text
cmake is not a recognized an internal or external command
```

The required machine is Windows 10 or 11 x64. `echo %PROCESSOR_ARCHITECTURE%` should print `AMD64`.
Use x64 CMake, not the x86/i386 or ARM64 build. The repository requires CMake 3.24 or newer, Visual
Studio 2022 with the Desktop development with C++ workload/MSVC v143, Git, and vcpkg.

Install CMake from an Administrator `cmd.exe` when WinGet is available:

```bat
winget install --id Kitware.CMake --exact --source winget --accept-package-agreements --accept-source-agreements
```

Then close and reopen Administrator `cmd.exe` and verify:

```bat
where cmake
cmake --version
```

If WinGet is unavailable, use the official Windows x64 MSI from <https://cmake.org/download/> and
select the option to add CMake to the system PATH.

The CI-pinned vcpkg tool commit is `03e366fb91e38b9432ebd5f8cc79f7c8f55e96ab`. If the VM does not
already have `VCPKG_ROOT`, prepare it outside the repository:

```bat
mkdir C:\tools
git clone https://github.com/microsoft/vcpkg.git C:\tools\vcpkg
git -C C:\tools\vcpkg checkout 03e366fb91e38b9432ebd5f8cc79f7c8f55e96ab
C:\tools\vcpkg\bootstrap-vcpkg.bat -disableMetrics
set VCPKG_ROOT=C:\tools\vcpkg
```

From an Administrator `cmd.exe` at repository commit `e114307`, create or confirm the real loop:

```bat
mkdir C:\lv-m5-junction\source
echo payload>C:\lv-m5-junction\source\file.txt
mklink /J C:\lv-m5-junction\source\loop C:\lv-m5-junction\source
fsutil reparsepoint query C:\lv-m5-junction\source\loop
```

If the fixture was already created, do not recreate it; confirm it with `fsutil` instead.

Configure and build:

```bat
cmake --preset windows-development -DLOCALVAULT_WARNINGS_AS_ERRORS=ON
cmake --build --preset windows-development-debug --parallel
```

Make the existing native Windows test scan that exact manually created loop, with a 30-second
watchdog:

```bat
set LOCALVAULT_M5_JUNCTION_LOOP_SOURCE=C:\lv-m5-junction\source
powershell -NoProfile -Command "$p=Start-Process -FilePath 'build\windows-development\tests\Debug\localvault_tests.exe' -ArgumentList '--gtest_filter=FileScannerTest.NativeWindowsJunctionIsCapturedAndNeverTraversed' -NoNewWindow -PassThru; if(-not $p.WaitForExit(30000)){$p.Kill(); throw 'junction-loop scan did not terminate'}; exit $p.ExitCode"
```

Expected result: exactly one test passes within 30 seconds and the scanner never reaches
`loop\file.txt`.

Safe cleanup (never use `rmdir /S` through the loop):

```bat
rmdir C:\lv-m5-junction\source\loop
del C:\lv-m5-junction\source\file.txt
rmdir C:\lv-m5-junction\source
rmdir C:\lv-m5-junction
```

Ask the user to report PASS or FAIL and paste the test output. Record it explicitly as
user-reported human evidence; do not infer or self-certify the result.

## Work after the human result

If the human gate passes:

1. Update `docs/implementation-logs/M5/implementation.md` and `verification.md`:
   - record the human-reported junction-loop evidence;
   - mark the Windows VM human gate PASS;
   - mark the Linux sanitizer CI and all-platform CI checklist boxes PASS using run 29376613145;
   - replace the stale `21 PASS / 0 FAIL / 2 PENDING` and blocker text;
   - update FR-117 text that still says native Windows CI is pending;
   - name the proving test and preserve the ignore-rules mapping decision.
2. Recheck the full M5 checklist and ensure no automated or human item remains pending.
3. Run formatting/static/diff checks appropriate to the documentation-only change.
4. Commit and push the intentional log finalization to `main` using the established milestone
   workflow.
5. Inspect GitHub Actions after the push. Treat any failure as a defect and continue the focused
   fix-push-rerun loop. Do not edit files after the final green run.
6. Confirm the exact final `main` commit passes Linux, macOS, Windows, Linux ASan/UBSan, and Linux
   TSan.
7. Confirm `git status` is clean and `HEAD == origin/main`.
8. Only then declare M5 complete.

If the human gate fails, preserve the output, diagnose the concrete scanner/test/setup defect, make
the smallest complete repair, rerun proportional local gates, commit/push, inspect exact-head CI,
and ask the human to repeat the real VM test. Do not rerun until it happens to pass.

## Ready-to-paste continuation prompt

```text
Continue M5 in /Users/f8fq/dev/LocalVault using the repository AGENTS.md and the M5 orchestration
rules. First read build-plan/orchestration/M5-CONTINUATION-HANDOFF.md completely, then read the M5
implementation and verification logs it references before changing anything.

Do not redo M4 or the completed M5 implementation. Current synchronized main/origin/main is
e114307f837e43face77e102baadfea6a35311ba. Exact-head GitHub Actions run 29376613145 is green on
Linux, macOS, Windows, Linux ASan/UBSan, and Linux TSan. All local automated gates, the critical
shutdown-invariant review, repairs, and fresh independent verification are complete.

The sole remaining gate is the human-only real Windows junction-loop VM test; an agent must not
self-certify it. The Windows VM previously reported that cmake was not recognized. Help me finish
the x64 Windows prerequisites and run the exact guarded test commands from the handoff. Ask me to
report PASS or FAIL with output.

After I report a human PASS, update both M5 logs to close the stale CI and human pending items,
commit and push the intentional finalization, inspect every GitHub Actions job, and continue the
focused fix-push-rerun loop until the exact final main commit is green on every required job. Then
verify main is clean and synchronized with origin/main before declaring M5 complete. Preserve
unrelated changes and use the smallest complete implementation.
```
