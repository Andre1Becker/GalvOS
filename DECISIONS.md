# Optimizer Refactor — Decisions

Records the outcome of every "analyze-first" prompt and every cross-session
design choice, so later sessions do not re-derive or contradict them. Append
only; if a decision is revised, add a new dated entry that supersedes the old
one (do not delete history).

## Pending (must be filled before the dependent session runs)

- **P12 min_segment_pts** — repair or remove?  → blocks K (P16 struct design)
- **P13 speedT** — re-target or remove?         → blocks K, R
- **P14 PathVertex::lift** — implement or document as vertex-0-only? → blocks K
- **SegmentPlan fields (P16)** — final field list, folding P12/P14 outcomes
- **P19 easeIn/easeOut** — decouple from cornerSeverity or not?
- **P21 Pillar-2 defaults** — per-profile values (index 0-7), with the math
- **P22 ILDA option B** — feasible? proposed gate + approach (no code)

## Decided

<!-- Template:

### <date> — P<n>: <short title>
Finding: <what the code actually showed>
Decision: <what was chosen and why>
Consequence for other prompts: <e.g. "SegmentPlan must carry field X">
-->
