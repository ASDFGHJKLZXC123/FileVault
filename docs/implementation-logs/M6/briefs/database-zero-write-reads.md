# Zero-write metadata reads for maintenance

`DatabaseAccess::locked_read_only` is an internal WAL-aware view for verification
and GC preview. Its caller must hold the exclusive repository OS lock and retain
the original normal SQLite connection until the view is destroyed. The OS lock
excludes repository writers; the normal connection's retained shared database lock
prevents other normal connections from checkpointing on close.

The view uses a narrow delegating VFS with READONLY database and WAL handles, sets
`SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE`, and selects EXCLUSIVE locking before the first
read. The wrapper delegates real SHARED locking and native close accounting, but
reports a successful EXCLUSIVE upgrade while retaining the native SHARED lock.
SQLite then reconstructs its WAL index in private heap pages instead of mapping or
updating repository SHM. Writes, truncation, named auxiliary-file opens, and VFS
deletion are rejected. Only anonymous DELETEONCLOSE SQL scratch files use ordinary
native writable access. This uses ordinary SQLite WAL-index memory (roughly 32 KiB
per 4096 frames), not an in-memory database or WAL copy.
The constructor rejects missing/nonregular/indirect database or WAL files and main
files shorter than the 100-byte SQLite header before invoking SQLite.

Source evidence from pinned SQLite 3.53.3 amalgamation under
`build/m5-junction/vcpkg/buildtrees/sqlite3/src/nf-3530300-2586d521e3.clean/sqlite3.c`:

- `pagerOpenWal` (67257–67285) requests a heap WAL index in EXCLUSIVE mode.
- `walIndexPage` (68299–68304) allocates private heap pages in this mode;
  `walIndexClose` (69139–69147) frees them without calling SHM unmap.
- Built-in `unix-none` is unsafe for this purpose: closing its additional database
  descriptor releases the process's other POSIX fcntl locks. The wrapper instead
  preserves the native `unixClose` inode/deferred-descriptor accounting and leaves
  the underlying file's method table untouched.
- URI `nolock=1` is unsuitable: `sqlite3PagerWalSupported` (67233) rejects it.
  `immutable=1` is unsuitable because it bypasses reading live WAL frames.
- `pager_unlock` (61499–61511) retains the normal connection's database lock;
  `sqlite3WalClose` (70047–70055) checkpoints only when its buffer is supplied;
  pager close (63871–63876) with NO_CKPT_ON_CLOSE supplies no buffer.
- `pagerOpenWalIfPresent` (63035–63038) can delete a WAL for an empty main database;
  `sqlite3WalOpen` (69243–69245) opens WAL with CREATE. Preflight rejects both cases.

Regression tests cover uncheckpointed WAL data, unchanged main/WAL/SHM bytes and
directory entries before/during/after the view, rejected SQL writes, missing or
nonregular WAL rejection, and preservation of WAL beside an empty main database.
The POSIX cross-process lock regression probes the SQLite shared byte range with a
forked child's exclusive fcntl request before, during, and after normal/exceptional
view closure. It avoids byte-capture helpers, whose own main-file close would drop
POSIX process locks and obscure this test's baseline.
