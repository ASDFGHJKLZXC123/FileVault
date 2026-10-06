# M7 core whole-file verification

User decision: implement `verify --files` in the core during M7. Part 06 §21.2:
"Optionally verify full file hashes by reconstructing streams from chunks without writing destination files."

Own only `include/localvault/integrity_verifier.hpp`, `src/core/integrity_verifier.cpp`,
and `tests/integration/integrity_verifier_test.cpp`. No build/CMake/log edits.
Preserve existing verify calls; add a trailing `bool verify_files = false` argument to
`verify(mode, stop_token, progress, verify_files)`. Reject files with quick mode.
Add checked_files / checked_file_bytes result counters and a file-hash issue kind.
Stream one verified bounded chunk at a time into a file BLAKE3 hash, validate sequence,
offsets, total size and stored whole-file hash, including empty files. Complete snapshots
only; independent bad files remain reportable. Preserve zero repository writes, locking,
no-follow object reads, cancellation and progress throttling. No destination files.
Test valid multi-chunk and empty files, corrupted file_hash with intact chunks, malformed
relationships, cancellation, independent failures, and unchanged repository bytes.
Smallest complete implementation; report decisions/assumptions and proving tests.
Root owns builds. Named historical model routes are unavailable; available inherited agents
are used with separate invariant review and fresh independent verification, as in M6.
