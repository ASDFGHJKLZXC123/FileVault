# Milestone 1 Implementation Log

Date: 2026-07-12
Milestone: Database and repository lifecycle

## Decisions

- Omit optional `operation_history` from migration 001. It is not required by M1 and can be
  introduced later by a new immutable numbered migration when the GUI needs it.
- Keep writable SQLite connections in WAL/FULL mode with persistent WAL/SHM artifacts. Ordinary
  read-only opens therefore create no paths and remain WAL-aware. When both sidecars are absent,
  immutable read-only is selected only after the platform positively proves storage is read-only;
  writable sidecar-free repositories are rejected without mutation.
- Pre-create the database and SQLite sidecar pathnames exclusively under the repository lock and
  track successful creations in a fixed ledger. Failure cleanup runs in two phases so visible
  database/layout content is removed while the writer lock is still held, then the lock file and
  newly created root ancestors are removed after unlock.
- Reject filesystem roots and symlinked repository/layout paths. Validate migration version,
  required tables/indexes, SQLite storage types, hash/path encoding, and format version on open.
- Migration execution owns its exclusive transaction. The initial `repository_info` insert uses a
  following exclusive transaction; caught initialization failures remove the invalid repository.
  Crash recovery remains M4 work.

## Plan amendment and deviations

- The original M1 process line incorrectly required FR-005 recovery proof while the same milestone
  explicitly leaves recovery empty until M4. Local M1 plan/orchestration wording was amended:
  M1 closes FR-001–FR-004; FR-005 is deferred to M4.
- POSIX directory flushing is checked and errors propagate. Windows relies on SQLite FULL
  synchronous mode because Windows has no generally usable directory `fsync` equivalent, as
  permitted by the "when practical" initialization step.
- The sanitizer CTest preset hardcodes `detect_leaks=1`, which Apple AddressSanitizer rejects on
  this host. The complete sanitizer binary was run with leak detection disabled; ASan and UBSan
  otherwise remained enabled and all tests passed.

## Functional requirements and proving tests

- **FR-001** — `RepositoryTest.CreateAndOpenRoundTripPreservesRepositoryInformation`,
  `ExistingNonEmptyDirectoryRequiresApproval`,
  `ApprovedNonEmptyDirectoryPreservesExistingContent`.
- **FR-002** — `RepositoryTest.RandomDirectoryIsNotARepository`,
  `MissingRequiredSchemaObjectIsRejected`, `OpenRejectsSymlinkLayoutEntry`.
- **FR-003** — `RepositoryTest.FutureFormatVersionIsRejectedClearly`,
  `RealFormatVersionIsRejectedWithoutIntegerTruncation`.
- **FR-004** — `RepositoryLockIntegrationTest.SecondProcessReceivesRepositoryBusy` (real child
  process).
- **FR-005** — deferred to M4; M1 intentionally contains no recovery implementation or false
  proving test.

## Gotchas for later milestones

- Do not remove persistent WAL/SHM while a connection is open. Sidecar-free read-only access must
  never silently fall back to immutable mode on writable storage.
- All object-reading or mutating operations added later must acquire `RepositoryLock`; pure SQLite
  metadata queries remain lock-free.
- Add schema changes only as new ordered migrations; never edit migration 001 after release.
- The lock file contents are diagnostics only. OS lock ownership is authoritative, and successful
  repositories never delete `repository.lock`.

## Local evidence

- Warning-strict development build: passed.
- Development CTest: 37/37 passed.
- ASan/UBSan test binary (`detect_leaks=0` on macOS): 37/37 passed.
- Critical invariant review: seven findings repaired and re-checked; no blocker remains.
