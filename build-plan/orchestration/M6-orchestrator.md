# M6 Orchestrator — Diff, verify, delete, and GC

Spec: [../milestones/M6-diff-verify-delete-gc.md](../milestones/M6-diff-verify-delete-gc.md) · Loop: [00-COMMON.md](00-COMMON.md)
Reading set: Part 04 (§14.5) · 05 (§18.8) · 06 (§20–22) · 07 (§23.4/23.6) · 09 (§32.1/32.3) · 11 (§42.9)

## Fix first (orchestrator)

- Four disjoint packets — good parallelism. But **GC is the only code that deletes user data**: packet C gets a dedicated review-agent pass and the strictest brief.
- No chunk refcounts — explicitly forbidden (§14.5); truth comes from the LEFT JOIN.

## Packets

| # | Packet | Scope | Order | Class |
|---|---|---|---|---|
| A | DiffEngine | two sorted cursors, streaming merge, §20.2 classification, dir-mtime suppression + battery | parallel | standard |
| B | Verification | quick (all eleven §21.1 checks) + full (§21.2); §32.1 corruption helpers; severity/exit map §21.3 + corruption matrix | parallel | tricky |
| C | Delete + GC | delete via M4 `deleting`; GC order: exclusive lock → recovery → §22.2 join → recheck → object-then-row batches; `--dry-run` zero writes | parallel, reviewed | critical |
| D | Statistics | §18.8 formulas, §42.9 SQL, checked arithmetic (§42.5) + battery | parallel | mechanical |

## Watchpoints (put in briefs)

- The project's most important test: snapshot A+B share chunks → delete B → GC → **fully restore A byte-identically**.
- GC "unreferenced" computed only after recovery, inside the lock, rechecked — never from a cached set.
- Orphan objects = GC candidates, not corruption; missing/corrupt **referenced** objects = fatal. Exit-code mapping matters (5 vs clean-with-notes).
- Interrupted GC (row pointing at deleted object) is stale metadata by design — encode as the recoverability test, don't "fix" the ordering.
- Diff needs dir-mtime suppression now or every real diff is noise.
- Statistics: zero denominators → 0.0.

## Verify

- Fresh agent: checklist → `docs/implementation-logs/M6/`; FR-201–204, FR-400–408 mapped.
- Dry-run assertion: repo tree + DB byte-identical before/after.
- No VM needed; optionally full-verify the M1 VM-created repo for a free portability point.
