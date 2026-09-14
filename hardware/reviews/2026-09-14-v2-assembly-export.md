# V2.0.10 assembly-export and buck-pad review — 2026-09-14

Baseline: `3468b7b13f6fd5958f94450dff0705a5b9c341e0`.
**DRAFT — NOT FOR FABRICATION OR LASER OPERATION.**

## Confirmed export hazard and guard

Native KiCad 10.0.6 position exports from the current PCB give:

| Export options (CSV, mm, both sides) | Rows | U_BUCK1 |
|---|---:|---|
| `--smd-only --exclude-dnp` | 97 | Included |
| Above plus `--exclude-fp-th` | 96 | Missing |

The latter flag excludes any footprint containing a plated through-hole
pad, not just through-hole components. U_BUCK1 is correctly an SMD assembly
component despite its eight plated pad-9 holes. Changing its footprint
attribute to THT to suppress the DRC warning would misstate assembly intent.

Added `hardware/tests/check_smd_placement_coverage.py` in the existing
hardware verification layer. It derives the expected populated SMD inventory
from the actual PCB attributes, respects DNP/position-exclusion flags and
compares CSV reference uniqueness, full reference coverage, values, footprint
item names and assembly sides. It does not infer assembly type from holes.

The 97-row native export passes. The actual 96-row negative export is
rejected specifically for missing U_BUCK1. Eight unit-test methods cover
complete/empty inventories, missing buck, duplicates, extra parts, malformed
rows and identity/side mismatch. All 49 hardware unit tests pass.

This is coverage/identity proof only. Coordinates, rotation, origin, units,
supplier-specific package rotations, board-side conventions, stencil, BOM
procurement and solder-joint quality still require manufacturing review.
No KiBot configuration/workflow changes were made; the observed failure is
a native CLI option hazard, not a claim that KiBot currently uses that flag.

## Buck land-pattern evidence: no automatic geometry change

The actual placed U_BUCK1 footprint has:

- Eight signal pads at 1.27 mm pitch, 5.4 mm row-center spacing,
  each 1.55 x 0.60 mm.
- Pad-9 front/back copper regions of 2.95 x 4.90 mm, connected to
  `5V GND Buck`.
- Separate 2.60 x 3.10 mm central F.Mask and F.Paste rectangles.
- Eight 0.30 mm plated holes on pad 9, at X = +/-0.65 mm and
  Y = +/-0.65 / +/-1.95 mm, with 0.60 mm copper lands.
- No B.Mask opening in the footprint. Four central holes fall inside the
  explicit front mask/paste opening; a generic tenting setting alone does
  not remove that opening or establish hole filling.

Source: TI [LMR33630 datasheet](https://www.ti.com/lit/ds/symlink/lmr33630.pdf),
DDA0008J board/stencil examples, drawing 4221637/B, 03/2016. Examined PDF
SHA-256: `3b0920a4e56a0b3f5f214a6317d3c06cff6e541e5054024adc31c30acc50c04a`.

The example explicitly shows the same signal-pad geometry, extended copper
and 2.60 x 3.10 mm mask/paste opening. Its stencil table associates that
opening with 0.125 mm stencil thickness and explicitly describes 100% printed
coverage. Therefore neither the extended copper nor the single full opening
is by itself evidence of an error. Do not substitute an arbitrary segmented
stencil merely to follow a generic rule.

The TI board example labels its vias 0.20 mm typical, whereas the placed
footprint drills are 0.30 mm. The assembly shop must review the actual via,
mask, stencil and solder process together, including possible paste loss
into unfilled holes. TI also states that the exposed pad must be soldered
to the board and that the assembly site may recommend a different stencil.
Exact purchased package variant, process and inspection remain unqualified.
No thermal or current-capacity conclusion is made.

## BOM evidence and required user choice

A fresh ungrouped native BOM export contains 122 rows. Only eight rows have
an explicit MPN or legacy MP field. Some other Value fields already identify
specific ICs; the field count is not proof that 114 parts are unidentified
or that the eight populated fields are procurement-qualified.

The user was asked whether assembly will use a reflow service, self-applied
paste/hot air, or mainly a soldering iron. Do not choose a different buck,
stencil or via construction before this process choice is known. The
exposed-pad joint is a required connection, not optional assembly detail.

## Reproduce without manufacturing release

Run from the repository root, using a fresh temporary directory:

```sh
assembly_check_dir=$(mktemp -d /tmp/galvos-assembly-check.XXXXXX)
kicad-cli pcb export pos --format csv --units mm --side both \
  --smd-only --exclude-dnp --output "$assembly_check_dir/smd.csv" \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_pcb'
/usr/bin/python -B hardware/tests/check_smd_placement_coverage.py \
  "$assembly_check_dir/smd.csv" \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_pcb'
python -B -m unittest discover -s hardware/tests -p 'test_*.py'
```

For the negative integration test, repeat the export with
`--exclude-fp-th` and a different output filename. The guard must fail
with `missing=['U_BUCK1']`. Do not send that export for assembly.

SCH/PCB/PRO hashes remain respectively
`adb9bf0af905d7a063de0bc2b287c3bc65b554794207a8d884ebd85c7f033f29`,
`267c312d4451f386fe3bcb2e500c779456d1787064c24c7be033837a4de578ee`,
`dc77f4155067018d81c509e667ad67642031a5cf69abe7181144c19c11bd3b1b`.
PDF/PCB images remain current and unchanged. Prior ERC/DRC evidence applies
to these identical design files; no new ERC/DRC run is claimed here.

No production BOM, Gerbers or assembly release was generated. Probe CSVs
remain temporary. Mechanical fit, independent safety and other release
gates remain open. Thermal, load and timing remain excluded and unverified.
