# M7 narrow QueryService additions

User decision: add two narrow queries for `show --warnings` and `stats --snapshot`.
Own only `include/localvault/query_service.hpp`, `src/core/query_service.cpp`,
and `tests/integration/query_service_test.cpp`. No build/CMake/log edits.
Contracts: add SnapshotWarning { relative_path, code, message }; add
`Page<SnapshotWarning> list_warnings(SnapshotId, PageRequest = {}) const` and
`RepositoryStats snapshot_stats(SnapshotId) const`. Require complete snapshots;
use existing page bounds, deterministic warning ordering and one read transaction.
Snapshot stats count that snapshot's regular-file logical bytes and distinct referenced
chunks only, checked unsigned arithmetic, same three savings formulas and zero behavior.
Keep global stats semantics unchanged. Test isolation between snapshots, shared/duplicate
chunks, empty files, warning paging and missing/incomplete snapshots. Smallest complete
implementation; reuse existing helpers, avoid new broad query surfaces. Root owns builds.
Report assumptions/decisions and tests. Inherited model route as documented in M6.
