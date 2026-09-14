# Project-local KiCad libraries

These symbols and footprints were imported from the user's existing
`KiCad_Libs` directory on 2026-09-13. They preserve the existing v2 assignments
and pin/pad numbering. Library nicknames remain unchanged so older files can
still resolve the same identifiers. The standard KiCad symbol and footprint
libraries must also be installed.

Open `../Laser Controllerv2_only_for_pcb_test.kicad_pro` for v2. Its basename
matches the requested schematic, allowing KiCad CLI and the editor to resolve
the project-local library tables. The minimal project uses KiCad default rules;
it does not inherit old exclusions or claim that the legacy PCB matches v2.

| Library | Contents | Original location |
| --- | --- | --- |
| Symbols | OPA4134UA_2K5 | `KiCad_Libs/Symbols/OPA4134UA_2K5.kicad_sym` |
| Buffer | DAC8562SDGST | `KiCad_Libs/Symbols/DAC8562SDGST.kicad_sym` |
| ESP32 | ESP32-S3-N16R8-DevKit | `KiCad_Libs/Symbols/ESP32-S3-N16R8-DevKit.kicad_sym` |
| Footprint | D14, DGS10, XCVR_ESP32-S3-N16R8-DevKit | `KiCad_Libs/Footprint/` |

Existing source attribution and SnapEDA links are preserved. Importing these
files establishes reproducibility, not manufacturer approval or a new license.
The actual DevKit dimensions, connector orientation and production assembly
process still need confirmation.

## Reviewed corrections

`DAC8562SDGST`: LDAC (4), CLR (5) and SYNC (6) are inputs, not bidirectional
signals. VREFIN/VREFOUT (10) is bidirectional, not input-only. Both the local
library and the v2 schematic's embedded symbol were corrected together using
the TI DAC8562 data sheet, SLAS719E, section 6 (Pin Functions).
Pin numbering, geometry and nets did not change.

The v1 schematic's embedded symbols are historical and were not updated.

U1's existing Assembly field specifies two 1x22 female socket strips. The
DevKit footprint description was corrected from 21/20 pins to 22/22 pins
in v2.0.4; its 44 plated pads and geometry are unchanged. The retained
22.86 mm row spacing remains provisional until checked on the actual board.
See `hardware/reviews/2026-09-14-v2-scan-status.md` for the socket/fit boundary.

## Layout requirements for added capacitors

- C9: 100 nF, X7R, 50 V, 10%, at OPA4134 U12 pin 11, returning to its analog
  ground. C8 already bypasses the positive rail.
- C_WDVCC1 and C_SCANVCC1: 100 nF, X7R, 50 V, 10%, directly between pins 8 and 1
  of the corresponding NE555. They complement, rather than replace, CONT and
  timing capacitors.

These requirements are also stored in the schematic's `Layout` fields.
