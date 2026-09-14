# V2.0.7 local buck input capacitor

Based on V2.0.6 commit `5cde493a24aa4732153044ad6434225d27abd20a`.
**Engineering draft only; no fabrication or laser-operation approval.**

## Input-loop change

TI LMR33630 SNVSAN3F section 10.1 identifies the input capacitor/regulator
power-ground loop as the critical high-transient-current loop and calls for
short, wide routing with nearby input capacitors. The inspected reference is
<https://www.ti.com/lit/ds/symlink/lmr33630.pdf>, SHA-256
`3b0920a4e56a0b3f5f214a6317d3c06cff6e541e5054024adc31c30acc50c04a`.

C_IN2 was at (146, 31) mm, with its input connection reaching the IC through
the back-layer distribution and vias. It now sits at (136.5, 38.73) mm,
rotated 90 degrees, next to the existing C_INHF1. Its 10 uF value and 1210
footprint are unchanged. C_IN1 remains the other input-bank capacitor.

The new C_IN2-to-C_INHF1 VIN and ground links are 3.5 mm long, 0.8 mm wide,
and on F.Cu. They join the existing 0.6 mm direct C_INHF1-to-regulator paths.
Each complete capacitor-pad-to-IC-pad path is 6.905 mm long, without a via.
These numbers describe explicit trace centerlines, not measured inductance,
current distribution, loop area or an EMI result. The source-distribution
traces upstream of the local input capacitor are retained.

Four obsolete top-layer capacitor stubs and their two now-unused vias were
removed. Two reference labels were adjusted. No electrical net, component
value, footprint type, firmware behavior or external connector changed.

## Duplicate-object correction

The stricter native object audit found two exact +3V3 trace pairs sharing IDs
in the published V2.0.5 and V2.0.6 PCBs. V2.0.4 had distinct IDs; the V2.0.5
restoration mapped pre-existing coincident geometry to the same IDs. Earlier
geometry-count comparisons did not check UUID uniqueness, and native DRC did
not report this condition. This is a correction to that verification gap.

The pairs were at (85.5913, 39.0662) to (87.5251, 41) mm, and the adjacent
(85.5912, 39.0662) to (85.5913, 39.0662) mm stub. One identical copy of each
is removed; one remains with the same geometry, width, net and ID. The
occupied +3V3 copper and its connectivity are unchanged. The local input
layout checker now rejects duplicate IDs across all tracks and vias.

## Proof

- All 122 component values/packages and all 110 electrical net memberships
  exactly match V2.0.6; only the schematic revision text changes.
- Native comparison confirms 121 unchanged placements and every pad's net,
  size, drill, shape and attribute. Only C_IN2 is moved/rotated.
- Of 1120 old copper objects, six obsolete input-capacitor objects and two
  exact duplicate copies are removed; 1112 distinct objects retain their
  original geometry, width, net and ID. Two new local tracks are added.
- Final board: 1008 segments, 106 vias, three filled/named ground zones.
  The front-layer detail was rendered and inspected. No autorouter used.
- Fresh ERC: zero findings. Native all-track DRC/refill/parity: zero
  unconnected items, no errors, only the known buck-type warning and the two
  known diode-filter warnings. No rules/severities/exclusions changed.
- All six interface checkers and 33 unit tests pass. A new native layout
  guard checks both direct input paths, widths, lengths and copper-ID
  uniqueness. Negative runs reject the duplicate-ID baseline and a candidate
  missing its local VIN link. The guard's 8 mm/0.60 mm bounds are local
  regression constraints, not TI-specified limits or current ratings.
- A relocated staged copy reproduces export/ERC, all six interface checkers,
  33 tests, native layout/ID and DRC/parity guards, and exact preservation.
  The KiCad source/project files match byte-for-byte. Publication details are
  recorded in `codex-todos.md`; source hashes and commands are in
  [CURRENT-HARDWARE.md](CURRENT-HARDWARE.md).

The earlier V2.0.6 feedback/bypass change remains intact. Laser safety,
exact DevKit/socket/antenna mechanics, actual load currents, component MPNs
and capacitor effective capacitance, source protection, copper/thermal
capacity, load-step/loop-stability behavior and EMC remain unqualified.
Moving a nominal 10 uF capacitor does not establish its capacitance at bias
or qualify a 3 A supply. KiBot inputs remain unchanged pending approval.
