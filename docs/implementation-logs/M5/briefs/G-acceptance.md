# M5 Packet G Brief — Acceptance Runs and Evidence

Class: standard. Run last, after integration and critical invariant review findings are closed.
This packet verifies and records; implementation fixes return through the owning packet/repair
route. Smallest complete test/support additions; state assumptions explicitly and report a summary.

## Verbatim requirements

Milestone acceptance:

> - Cancellation leaves no complete partial snapshot.
> - ASan/UBSan tests pass.
> - TSan-focused tests pass when enabled.
> - Memory remains bounded on a large generated dataset.

From §33.2 profile A:

> 50,000 to 100,000 files. Sizes from 0 to 16 KiB. Text, JSON, source, and random binary content.
> Deep and wide directories.

From §34.5:

> Do not combine ThreadSanitizer with AddressSanitizer in one binary.
>
> Run concurrency-focused tests under TSan: Bounded queue; duplicate object storage; progress
> aggregation; cancellation; error propagation.

## File boundary

- Add only test/benchmark/CMake/CI support needed for repeatable acceptance gates.
- Produce the concise implementation decision log under `docs/implementation-logs/M5/`.
- Do not write the independent verification log; a fresh verification agent owns it.
- Do not implement M6 diff/verify/delete/GC behavior.

## Required gates and evidence

- Warning-strict development configure/build and complete CTest.
- Focused queue, scanner, ignore, unstable-file, progress, shutdown-role, duplicate-race, and both
  cancellation-point batteries.
- Deterministic profile-A 50,000-file run with fixed seed and a generous fixed peak-RSS ceiling;
  memory bound must be independent of file count.
- ASan/UBSan. On macOS, if CTest forces unsupported leak detection, run the freshly built sanitizer
  executable with `ASAN_OPTIONS=detect_leaks=0` and record that exact evidence.
- Separate TSan configuration/executable over concurrency-focused tests; no sanitizer diagnostics.
- Formatting, clang-tidy/static checks, Python checks, `git diff --check`, and intentional-diff audit.
- Precise `mklink /J` human VM instructions; leave the human result pending.

## Watchpoints

- Never rerun a flaky concurrency test until green. Diagnose and fix timing or shutdown defects.
- Peak RSS uses a generous ceiling, not a precise performance claim; Mac host numbers only.
- Fresh critical shutdown review precedes this packet; fresh independent verification follows it.
- Every FR-112 through FR-119 and FR-114 must name proving tests; explicitly record that ignore rules
  were assigned to M5 scanner work.
