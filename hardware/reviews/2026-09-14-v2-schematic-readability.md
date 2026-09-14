# V2.0.10 schematic readability update — 2026-09-14

## Scope and evidence

Baseline: export commit `995eb85872eb54f38ede0726ccb4051b6725de39`;
electrical/PCB source tag `hw-v2.0.10-draft` remains unchanged.

The exported drawing exposed visible footprint strings, reference/value
fields inside IC bodies, inward-pointing local labels and an undersized
canonical A2 page. Native before/after PDF crops confirmed these causes.

This update changes presentation only:

- Canonical page: 620 x 440 mm, short V2.0.10 DRAFT revision, separate
  NOT FOR FABRICATION and NOT FOR LASER OPERATION warnings.
- Reference/value fields moved outside U1, U2, U12, U_BUCK1, U_WD1 and
  U_SCAN1. U1 fields also avoid the existing wires above its body.
- U2/U12 Footprint fields hidden, with their assigned values preserved.
- 22 labels at buck/watchdog/scan IC pins oriented outward; label text,
  UUIDs and XY electrical anchors remain unchanged.
- PDF refreshed directly from the canonical schematic. No export-only
  source transformation remains. PCB images stay unchanged/current.

## Preservation proof

An independent root-block comparison allows only page/title changes,
selected field presentation and label angle/justification. It found
797 exact unchanged blocks, six changed symbol display blocks, 22 changed
label display blocks and two page/title blocks. No symbols, wires, junctions,
pins, images or electrical anchors were added, removed or relocated.

Fresh KiCad XML exports before and after adoption have **byte-identical
components, libparts, libraries and nets subtrees**, including every
component property, pin definition and net class. No normalization was used
for the final canonical comparison: 122 components, 110 named nets.

The isolated candidate initially omitted the local libraries/project and
produced six missing-library ERC warnings and default net classes.
The unchanged project configuration, tables and libraries were copied into
the candidate directory before repeating validation; warnings disappeared
and the full net classes matched. These first incomplete-environment results
are not counted as passes.

## Verification

KiCad 10.0.6; canonical final files:

- ERC, all severities: zero findings.
- Native DRC with refill, all-track errors and schematic parity: zero
  errors, zero unconnected items, zero parity findings.
- One existing U_BUCK1 footprint-type warning remains; draft guard passes.
- Seven interface checkers pass: fan power, sensors/tach, DAC, scan status,
  DMX, buck and trigger-diode identity/polarity.
- All 41 hardware unit tests pass.
- Native PDF full-page and changed-region crops visually inspected.
  Page/title fit is corrected. Existing overlaps elsewhere remain.

Reproduction commands for native export, ERC, DRC, interfaces and unit tests
are in [CURRENT-HARDWARE.md](CURRENT-HARDWARE.md#reproduce).
For electrical preservation, export baseline and current schematics with
their matching project filenames/configuration and compare the XML
`components`, `libparts`, `libraries` and `nets` subtrees; only the
`design` metadata may differ. Git diff establishes the scoped drawing edits.

| Artifact | SHA-256 |
|---|---|
| Canonical SCH | `adb9bf0af905d7a063de0bc2b287c3bc65b554794207a8d884ebd85c7f033f29` |
| PCB, unchanged | `267c312d4451f386fe3bcb2e500c779456d1787064c24c7be033837a4de578ee` |
| PRO, unchanged | `dc77f4155067018d81c509e667ad67642031a5cf69abe7181144c19c11bd3b1b` |
| Refreshed PDF | `55ec864fb20cc925e97bd9cdc02804c4251efaf5aa8d7ffaef8a58df70f5041c` |

## Remaining gates

This is a scoped readability improvement, not a complete drawing sign-off.
Legacy passive-component labels and MCU pin labels still overlap. Actual
ESP32 row spacing/grid alignment, USB/antenna/enclosure fit, BOM/assembly
qualification, manufacturing-data review and independent shutdown/rearm
remain open. No firmware or KiBot changes were made.

Thermal, load and timing remain excluded from further work at the user's
request and unverified, not passed. No Gerbers, fabrication release or
laser-operation approval.
