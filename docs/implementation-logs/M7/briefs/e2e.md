# M7 black-box acceptance suite

Own only new `tests/cli/cli_e2e_test.py`. Root owns CMake registration and all executions.
Implement unittest script accepting built CLI binary as first argument; no third-party deps.
Read Parts08 §28/31 and09 §32.7; full CLI happy path and every exit code 0/2/3/4/5/6/7/130.
Context JSON envelope: {schema_version:1,command,result} or error{code,message,path}.
All supported JSON success/error/version/help must parse strictly as one document, stderr
only progress/prompts/errors/warnings; check global flags before/after subcommand.
Command contracts are in basic-commands.md and maintenance-commands.md beside this brief.
Core narrow queries and whole-file verification are already implemented; CLI agents are
working in commands_basic.cpp/commands_maintenance.cpp. Coordinate by messaging them.

Required cases: init/snapshot/list/show/files/diff/restore/verify/stats/delete/gc; restored
bytes incl empty/multi-chunk/unicode/nested files; --all required; overwrite never/always/
prompt skip/replace/apply-all via piped stdin (s/r/sa/ra/c), EOF failure and CRLF; explicit
--quick --files rejects, files impliesfull; forge stored file_hash with Python sqlite3 to
prove --full clean chunk verification vs --files detecting whole-file metadata corruption.
Duplicate/shared chunk retention through delete+gc and older snapshot exact restore;
GC preview repository tree byteidentical; read-only query outputs/warning paging/scoped stats.
Exercise documented flags without mirroring internals. Capture expected warning6 using a
denied file (POSIX chmod000, native Windows sharing/ACL denial via ctypes only temporary
fixture); exit4 reproducible FS failure; corruption5; lockbusy7 (flock/MSVCRT file locking
against repository.lock) must fail promptly. Never mutate user fixtures outside tempdir.
Cancellation: mid-snapshot observes stderr readiness before signal, expects130/recovery,
prior snapshot restored. Prompt with silent open stdin must cancel. Second interrupt must
force130 even if graceful input wait/cleanup delayed. POSIX SIGINT. Windows Ctrl+Break
with process group; if no console makes automation unavailable/flaky, record explicit skip
and reason for required human native-Windows gate (don't rerun-until-green).
No timing-only sleep assertions; bounded timeouts and readiness coordination. Keep suite
small and effective, avoid huge datasets (64-128MiB streamed fixture max). Per-case isolation.
Report tests/assumptions/skips; root runs it across native Windows and CI.
