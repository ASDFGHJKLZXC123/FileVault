# M2 Orchestrator — Minimal whole-file snapshot/restore

Spec: [../milestones/M2-whole-file-snapshot-restore.md](../milestones/M2-whole-file-snapshot-restore.md) · Loop: [00-COMMON.md](00-COMMON.md)
Reading set: Part 03 (§11.3) · 04 (§14.4) · 05 (§16) · 06 (§19) · 07 (§25.1–25.6) · 09 (§32.1/32.3) · 11 (§42.8)

## Fix first (orchestrator)

- Interfaces shaped per §11.3 (`Chunker`, `ObjectStore`) even though internals are dumb — M3 swaps implementations, not architecture.
- Keep everything single-threaded. No compression required.

## Packets

| # | Packet | Scope | Order | Class |
|---|---|---|---|---|
| A | Test support | `TemporaryDirectory`, `DatasetBuilder`, `expect_tree_equal` (§32.1) + §32.3 fixture tree | first, parallel with B | standard |
| B | Scanner + entries | `FileScanner` (`symlink_status`, normalized paths, root row §14.4), entry insertion (§25.1–25.6) | parallel with A | standard |
| C | Store path | one object per file, write-temp-then-rename, snapshot row `complete`-last, list | after B | standard |
| D | Restore path | §19.2 order: dirs shallow→deep, files temp+rename, symlinks, dir metadata deepest-first; alternate destination | after B, parallel with C | tricky |
| E | Acceptance | snapshot → delete source → restore → `expect_tree_equal` | last | mechanical |

## Watchpoints (put in briefs)

- Empty file: entry row, `logical_size=0`, zero chunk rows; hasher finalizes with no `update()`. Fixture on day one.
- Root row `''` — children query needs `relative_path <> ''` exclusion (the milestone's most predictable bug).
- Directory mtimes deepest-first, after all content.
- Nanosecond mtimes end to end; never round-trip through `time_t`.
- Symlinks: recreate link text; never `stat` the target.
- NFC/NFD: byte-compare stored path strings; keep one fixture of each.
- `file_hash` may stay NULL until M3 — record in log.

## Verify

- Fresh agent: checklist → `docs/implementation-logs/M2/`; FR-100–104/200/300–303/309 mapped to tests, with FR-103 partial until M5 completes Windows junction/mount-point scanner rules and the existing FR-104/309 partials called out.
- No VM session needed; CI covers Linux/Windows.
