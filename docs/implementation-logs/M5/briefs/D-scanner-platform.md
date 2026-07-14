# M5 Packet D Brief — Scanner and Platform Rules

Class: standard. Run after Packet C; only the pure platform decision function is parallelizable.
Smallest complete implementation; state assumptions explicitly and report a summary.

## Verbatim requirements

From §25.4:

> Use `symlink_status`. Save `read_symlink` text. Do not recurse through symlink targets.
>
> Directory junctions are saved as symlink entries with their target text and are never traversed;
> volume mount points are skipped with a warning (FR-117).

From §25.11:

> **Cloud placeholders:** files carrying `FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS` or
> `FILE_ATTRIBUTE_RECALL_ON_OPEN` ... are skipped with a warning; reading them would silently
> download content (FR-118).
>
> **One-file-system option:** `one_file_system` (FR-119) stops the scanner at filesystem boundaries
> on POSIX (compare `st_dev`) and at volume boundaries on Windows. Junctions and mount points are
> never traversed on Windows regardless of this option.

Ignore mapping:

> When a directory is ignored, do not recurse into it. When `--ignore-file <path>` is supplied ...
> it replaces the source root's `.localvaultignore` entirely; the two are never merged.

Milestone invariant:

> Scanner platform rules: junctions/mount points never traversed (decision function unit-testable),
> `one_file_system` honored, hidden-file option wired, cloud placeholders skipped with warnings.

## File boundary

- Primary: `src/core/filesystem/file_scanner.hpp/.cpp` and platform metadata/classification files.
- Wire Packet B ignore rules.
- Add the agreed optional ignore-file field to `SnapshotOptions` and pass scanner options from
  `SnapshotEngine`; do not add CLI parsing.
- Add/extend scanner, ignore integration, and platform-decision tests. Update CMake as needed.
- Do not change pipeline shutdown, object publication, unstable-file retry, or progress policy.

## Required tests

- Hidden files included by default and pruned when `include_hidden == false`.
- Full ignore battery, including proof that ignored directories are never enumerated recursively.
- Explicit ignore file replaces root rules.
- Pure faked-attribute decision table: normal symlink, junction (record link/no traversal), volume
  mount point (warn/skip), cloud placeholder (warn/skip), ordinary entries.
- `one_file_system` boundary decision on POSIX and Windows-volume fake data.
- Native POSIX different-filesystem fixture where available; deterministic unit seam otherwise.
- Windows CI real fixture for junction decision and no recursion. Do not self-certify the separate
  one-time VM human gate.

## Watchpoints

- Never recurse before ignore, reparse-point, hidden, or filesystem-boundary decisions.
- Junction and mount-point detection must not follow the reparse target.
- Do not hydrate cloud placeholders to inspect them.
- Preserve UTF-8 warnings, special-file warnings, deterministic traversal, and existing symlink
  behavior.
- Case sensitivity follows source filesystem behavior; never force lowercase.
