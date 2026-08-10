# Optimizer Refactor — State

Single source of truth across Claude Code sessions. Each session reads this
first and appends its result at the bottom. Keep entries short (5-8 lines).

Base commit at start: 13d6b8f
Current FW version: 6.41.3

## Wave / prompt status

Legend: [ ] todo  [~] in progress  [x] merged  [!] blocked

- [ ] A  Contract tests (Governing 1)      branch: opt/contract
- [ ] B  P1  Telemetry                      branch: opt/01
- [ ] C  P2-P5  Blanking chain              branch: opt/02-05
- [ ] D  P6-P7  Call-site chain             branch: opt/06-07
- [ ] E  P8   Cross-field validation        branch: opt/08
- [ ] F  P9   applyTransform capacity/leak   branch: opt/09
- [ ] G  P10  applyPpsScaling                branch: opt/10
- [ ] H  P11  Accel clamp vectorial          branch: opt/11
- [ ] I  P12-P14  Semantic decisions         branch: opt/12-14
- [ ] J  P15  Severity cache                 branch: opt/15
- [ ] K  P16  Plan/emit SegmentPlan          branch: opt/16
- [ ] L  P17  Stage-2 bisection              branch: opt/17
- [ ] M  P18  configFromLive merge           branch: opt/18
- [ ] N  P19  Straight-line gradient         branch: opt/19
- [ ] O  P20  TSP jump order                 branch: opt/20
- [ ] P  P21  Pillar-2 defaults              branch: opt/21
- [ ] Q  P22  ILDA option B (analysis only)  branch: opt/22
- [ ] R  P23  Comment/doc cleanup            branch: opt/23

Merge order = wave order. E/F/G/H may run in parallel branches.
N/O/P/Q may run in parallel branches. Everything else is sequential.

## Invariant status (mirror of CONTRACT.md test names)

- [ ] budgetNeverExceeded
- [ ] blankJumpEndsAtTarget
- [ ] noSilentPointLoss
- [ ] velocityAccelLimitsHold
- [ ] dacRangeValid
- [ ] allocFreeSymmetric
- [ ] deterministicOutput
- [ ] statsConsistent

## Session log

<!-- append newest at the bottom. Template:

### Session <letter> — P<n> — <date>
Branch: opt/<n>
Files: <list>
Decision: <if any, else "none">
Invariants now green: <names, or "none">
Version: <old> -> <new>
Notes: <one line, gotchas for later sessions>

-->
