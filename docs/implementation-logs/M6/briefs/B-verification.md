# M6 packet B — Integrity verification

Implement the §12.7 public API and all eleven §21.1 quick checks; full mode also
reads, decompresses, and BLAKE3-verifies each distinct referenced chunk through
the existing bounded `ObjectStore::read_verified` path. Verification collects
independent issues and never repairs or deletes data.

`VerificationIssue::Severity` distinguishes fatal required-data errors from
maintenance notes. `VerificationResult::ok()` ignores notes, allowing the future
M7 CLI to map errors to exit 5 and a healthy repository with notes to exit 0.
Missing unreferenced objects, orphan files, incomplete snapshots, and temporary
files are maintenance notes. Invalid foreign keys and required objects are errors.

Verification requires a read-write `Repository` and an exclusive repository lock
per §12.4. It deliberately does **not** invoke writer recovery: doing so would
delete the stale metadata and temporary files that verification must diagnose.
Only hash-derived object paths are inspected, with each directory component
checked for symlinks/reparse points; filesystem walks never follow indirection.
Cancellation throws `ErrorCode::cancelled`, matching existing engine behavior.

Optional full-file reconstruction (`--files`) is deferred. Full object validation
plus structural chunk relationships is the required M6 scope; M7 adds CLI wiring.

Validation will cover healthy quick/full results, distinct shared objects,
missing/truncated/modified objects, wrong raw sizes, forged object paths, foreign
key failures, bad repository metadata/counters, stale metadata and temporary files,
orphans, multiple simultaneous errors, no mutation, lock contention, read-only
rejection, cancellation, and indirection safety. The root integrator owns CMake,
shared-file changes, the final build, and the milestone logs.

Integration finding: SQLite omits schema CHECK expressions on read-only
connections (`sqlite3AddCheckConstraint` tests `!sqlite3BtreeIsReadonly(...)`).
Reproduced using the pinned SQLite 3.53.3 DLL and Python's SQLite 3.43.1: a
violated hash-algorithm CHECK returns `ok` from read-only `integrity_check`, while
a writable connection reports the violation. The verifier still executes the
required integrity PRAGMA; explicit format/algorithm, counter, size, and
relationship validation supplies the necessary semantic checks. The malformed
format fixture therefore requires `invalid_repository_info`, rather than an
additional `invalid_database` issue that read-only SQLite cannot supply for a
CHECK violation.
