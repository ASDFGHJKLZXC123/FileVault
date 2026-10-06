# M7 implementation decision record

Status: implementation, critical review, native acceptance, and human check complete; CI pending.

- User decisions (2026-10-04): restore prompts accept terminal and scripted input; add whole-file
  verification in core; add two narrow QueryService queries; native Windows development with
  cross-platform acceptance retained. EOF is a clear input error; prompt diagnostics use stderr.
- `verify --files` implies full verification unless explicitly combined with `--quick` (usage
  error). Whole files stream bounded verified chunks without creating destination files.
- QueryService adds paged warnings and snapshot-specific checked statistics. Interfaces never
  access SQLite directly. `files --path` and `--search` are mutually exclusive.
- JSON schema v1: one stdout document `{schema_version, command, result}` on results, or
  `{schema_version, command, error: {code, message, path}}` on fatal errors. Human output is
  formatted result data. Progress/errors/prompts/warnings use stderr; no terminal colors.
- JSON-mode progress uses structured stderr records. Diff output is staged in a temporary
  presentation spool and copied only after success, keeping diff memory bounded and stdout
  free of partial documents on core errors. Interrupt ownership survives fatal diagnostics.
- Verify/GC preview use a retained-lock maintenance view: private live-WAL indexing or immutable
  reads without sidecars. Ordinary queries retain concurrent WAL readers; a closed sidecar-free
  repository uses the locked immutable fallback. Maintenance opens never create sidecars.
- CLI GC executes unless `--dry-run`; core GC remains preview by default. Delete confirmation
  accepts stdin or `--yes`; full restore requires `--all`. First release accepts only 4MiB chunks.
- Restore now honors explicit `--no-final-hash`; default whole-file verification and mandatory
  chunk integrity, file-hash metadata shape, chunk layout, size and path validation remain.
- Historical model routes in the orchestration guide are unavailable; available inherited
  agents implement disjoint packets, with separate critical review and fresh verification,
  following the recorded M6 precedent. Root owns all build/test execution.
- Windows is the actual native host. Linux/macOS/Windows CI and sanitizer jobs remain gates.
  Human Windows acceptance passed with saved physical Ctrl+C / PowerShell JSON evidence;
  the user performed the action, and the saved record was inspected independently.
- Native warning-strict build passes; 254 tests pass with 11 documented platform/dataset skips,
  including all 14 CLI cases and three Windows console-interrupt cases. On 2026-10-05 the user
  explicitly authorized committing and pushing M7 to `Latest-Dev`, running all platform and
  sanitizer CI jobs, fixing failures, and recording results.

- CLI requirements exposed: FR-500 → `test_full_happy_path_bytes_queries_and_history_retention`;
  FR-503 (CLI) → strict envelopes/exit codes in the usage, repository/filesystem, corrupted-object,
  denied-source, busy, and interrupt cases; FR-303 → alternate roots and byte comparisons in the
  happy path; FR-307 → `test_scripted_overwrite_decisions_and_eof` (all policies/apply-to-all).
- Later-interface gotcha: ordinary WAL queries may update SQLite coordination sidecars;
  maintenance verification/preview must preserve every repository byte, including sidecars.
  Keep OS lock ownership through DB destruction and interrupt ownership through diagnostics.
- CI portability: canonicalize the CLI fixture's existing temporary root before deriving
  restore destinations, avoiding macOS `/var` alias rejection while preserving no-follow
  production checks. Verbose CTest output exposes nested e2e skips in the recorded job logs.
- Cold CI downloads failed for GoogleTest and GNU gperf before project compilation.
  Prefetch their exact pinned archives from alternate endpoints with SHA512 verification;
  network failures retain vcpkg's original fallback, checksum mismatches fail immediately.
  Keep the prefetch versions/hashes aligned when changing the vcpkg baseline.
