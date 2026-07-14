# M5 Packet B Brief — Ignore Rules

Class: standard. This packet may run in parallel with Packet A. Smallest complete implementation;
state assumptions explicitly and report a summary.

## Verbatim requirements

From §27.1:

> Support:
> - Blank lines.
> - Comments beginning with `#`.
> - Exact relative paths.
> - Directory patterns ending in `/`.
> - `*` within one path component.
> - `?` for one character within a component.
> - Patterns without `/` matching a name at any depth.
> - `!` negation may be deferred; if implemented, define ordering clearly.

From §27.2:

> - Normalize separators to `/`.
> - Match against repository-relative paths.
> - Decide case sensitivity according to source filesystem behavior; do not force lowercase.
> - When a directory is ignored, do not recurse into it.
> - The ignore file itself is included unless explicitly ignored.
> - When `--ignore-file <path>` is supplied on the CLI, it replaces the source root's
>   `.localvaultignore` entirely; the two are never merged.

Milestone mapping:

> **Ignore rules implemented here** (`.localvaultignore` per §27, `--ignore-file` replaces it
> entirely) — the plan assigns them to the scanner, so they land in this milestone; record this
> mapping in the log.

## File boundary

- Add `src/core/filesystem/ignore_rules.hpp` and `.cpp`.
- Add `tests/unit/ignore_rules_test.cpp`.
- Update `src/core/CMakeLists.txt` and `tests/CMakeLists.txt` only as needed.
- Do not edit `FileScanner`, `SnapshotEngine`, `ObjectStore`, database files, CLI, or public API;
  Packet D wires this component later.

## Required tests

- Blank lines and comments.
- Exact names and exact nested paths.
- `*` and `?` limited to one path component.
- Directory patterns and a match result that lets the scanner prune recursion.
- Patterns without `/` match names at any depth.
- Nested paths, spaces, hidden names, separator normalization, and filesystem case behavior.
- `.localvaultignore` remains included unless matched.
- Explicit replacement-file loading does not merge root rules.
- Missing default file means no rules; unreadable explicit file is an error.

## Watchpoints

- Defer `!` negation rather than adding ordering complexity; treat it as unsupported/literal only if
  that choice is explicit and tested.
- Do not silently lowercase. Use a small platform/filesystem case-policy seam if necessary.
- Do not implement CLI parsing (M7); expose loading/matching needed by Packet D.
- Avoid regex backtracking or an unbounded cache.
