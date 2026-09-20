# Orchestrator Loop — Common Rules

Applies to every `Mx-orchestrator.md` in this folder. The milestone spec (`../milestones/`) and its **Completion checklist** are the only definition of done. Coding rules: `../../AGENTS.md`.

## The loop

1. **Brief** — Orchestrator reads the milestone spec + its reading set. Writes one brief per packet: verbatim spec excerpts, file boundaries, required tests, relevant watchpoints. Agents get briefs, never "go read the plan."
2. **Delegate** — One implementation subagent per packet. Run packets in parallel only if file-disjoint; otherwise sequential. Every brief includes: smallest complete implementation, state assumptions explicitly, report a summary back.
3. **Integrate** — Orchestrator builds, runs tests, resolves interface mismatches, trims over-engineering.
4. **Review** — Critical packets and cross-cutting invariants get a fresh review agent, separate from implementation and verification. Findings are resolved before verification.
5. **Verify** — A fresh agent (no implementation context) walks the Completion checklist against code and test output; writes the independent verification log.
6. **Gate** — Logs committed to `docs/implementation-logs/<Mx>/`; push; all three CI platforms green. VM/manual items go to Richard — never self-certified by an agent.

## Models and effort

The parent session is the orchestrator. The user selects its model and effort when starting the session. Every delegated implementation, review, and verification invocation explicitly specifies its model and effort. Effort is model-relative reasoning intensity, not a fixed token budget shared across models.

Each packet table classifies implementation risk as `standard`, `tricky`, `critical`, or `mechanical`; model routing lives only here. Assignment policy:

| Role | Model · effort |
|---|---|
| `standard`, `tricky`, or `critical` implementation pass | GPT-5.3-Codex-Spark · xhigh |
| `mechanical` implementation pass | GPT-5.6-luna · high |
| Critical invariant review | opus · high |
| Independent verification (fresh context) | sonnet · high |
| Repair after two failed integrations | opus · high |
| Exceptional architecture second opinion | fable · high |

Spark owns the first implementation attempt, including `critical` packets. Critical and cross-cutting work (crash-safety, concurrency and pipeline shutdown, GC, signal safety, GUI threading) receives a fresh opus invariant review before independent verification. After two failed integration attempts, reassign the repair to opus · high. Never downgrade a critical packet to save cost.

**External models.** Spark and luna are not dispatchable from a Claude session — those passes run outside it (e.g., Codex CLI): the orchestrator hands over the brief, receives the diff, and integrates as normal. In-session fallback when the external model is unavailable: `mechanical` → haiku · low · `standard` → sonnet · medium · `tricky` → sonnet · high · `critical` → opus · high.

## Logs

- **Verification log** — the Completion checklist copied in and checked with evidence. Carries the entire burden of proof.
- **Implementation log** — a decision record, not a diary (≤1 page): decisions made with a one-line why (including those the plan requires recording), deviations/plan amendments, FR → proving-test map, gotchas for later milestones. Three lines is fine if that's all there was; what was done lives in git.

## Failure handling

- **Build/test failure at integration** — orchestrator triages first. Interface mismatch between packets → orchestrator fixes directly. Defect inside one packet → back to the implementing route with the failing test + diagnosis (attempt 2). Two failed attempts → repair reassigned per the routing table.
- **Review finding** — blocking. Fix via the implementing/repair route; the reviewer re-checks the finding. Never proceed to verification with an open finding.
- **Verification finds an unchecked box** — the verification agent reports only, never fixes. Orchestrator routes the fix like an integration failure, then re-verifies the affected items (full re-walk if the fix touched shared code).
- **CI failure on macOS/Windows** — a defect like any other: fix-push-rerun (this loop is expected for Win32 code). Platform debt is never carried; the milestone cannot close around it.
- **Flaky test** — never rerun-until-green. A flake is a defect, usually a timing assumption (see the M5 cancellation-test rule); fix the test or the code.
- **Spec conflict or ambiguity mid-packet** — stop the packet; orchestrator resolves against the plan. If the plan itself is wrong, record a plan amendment (implementation log + plan file). If it invalidates a recorded decision or needs human/VM judgment, escalate to Richard — don't guess.
- Every failure whose lesson outlives the fix goes in the implementation log's gotchas.

## Constants

- Milestones strictly in order; no future-milestone work lands early.
- Orchestrator owns: spec interpretation, public interfaces, decomposition, integration, the done call.
- Subagents own: bounded implementation; critical invariant review; independent verification.
- Sandbox = Linux leg of the tri-platform rule; CI compiles macOS/Windows. Push early and often.
- Benchmarks: Mac host only. Never publish VM/sandbox numbers.
- Cross-cutting invariants (crash-safety, concurrency and pipeline shutdown, GC, signal safety, GUI threading) get a dedicated review-agent pass before verification.
