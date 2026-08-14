Triage


P4 — Design questions, resolved via live camera capture (172.18.30.30 + optimizeGalvo.py analyze-live)
13. Chaos Bouncer — confirmed working as designed. Woven bowtie/X from two incommensurate
    triangle-wave frequencies (fx=4.3, fy=2.9), matches the code comment's intent. No action.
14. Starburst Party — NOT a design question, it's a bug. `p88` (backing fn for StarburstParty)
    predates the optimizer-batch migration `p59` (plain Starburst) already got: 24 separate
    line()->optimize() calls each draw against the frame's point budget individually; budget
    runs out ~12 spokes in and frameContext() silently drops the rest. Live A/B: Starburst
    (p59) = full 360° burst, Starburst Party (p88) = top ~180° only, reproduced twice (bbox
    ratio ~2:1 both times). Fix: batch all 24 spokes into one PathSegment[] -> optimize() call
    like p59 (preset_patterns.cpp:1229-1249). Also: line()'s dwell param is dead code (name
    commented out) — p88's trailing `,8)` arg does nothing, unrelated minor cleanup.

→ #13 closed, no dev time needed. #14 moves out of P4 into the normal correctness-bug queue
  (P2/P3 tier) — root cause is known, not a taste call.

P5 — Tooling (optimizegalvo.py, non-realtime, can run in parallel with firmware work)
15. calibrate-warp: no in-tool explanation of what to do, missing from docs.
16. measure-resonance: fails to resolve −3dB bandwidth near peak — should auto-retry with a wider resonanceFineSpanFraction / broader line instead of giving up.
17. searchSpace.json not regenerated when missing.

→ Bundle all three as one "optimizegalvo maintenance pass" — no hardware risk, no galvo real-time constraints, safe to hand to a subagent independently of the firmware work above.

Suggested order
P0 (off-state safety bug + Paint-by-Finger) — ship first, always.
P1 (ILDA UI hang).
P2 (Autoscale, 3D projection, Wave loop, H-Line speed).
P3 (UI bundle: Mirror H/V + rotation buttons + auto-rotation range + Text clamp; H-Line direction feature riding with its speed fix).
P4 — resolved (#13 as-designed, #14 reclassified as a bug — see fix above, fold into P2/P3).
P5 — independent track, can run in parallel with anything above since it touches Python tooling only, not firmware/UI.