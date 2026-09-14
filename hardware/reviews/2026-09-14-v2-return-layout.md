# V2.0.8 DAC return copper and ESP32 envelope

Based on hardware V2.0.7, commit
`b4385e3a3fff1313cbd545e9392028e87c34ad1a`.
**Engineering draft only — no fabrication or laser-operation approval.**

## User-supplied mechanics

The user reports an ESP32 module body of **28 x 57 mm**, with one 22-pin
male strip on each side. The carrier retains two female 1x22 socket strips.

The old footprint drew a 25.4 x 56 mm body. Both the project-local library
and the placed U1 footprint now draw a 28 x 57 mm F.Fab envelope, a
28.2 x 57.2 mm silkscreen outline and a 29 x 58.54 mm courtyard.
The old courtyard's longer Y allowance is retained. All 44 pads, pin
numbers, drills, nets and absolute contact positions are unchanged.

Body centering on the pin grid is still an assumption. The 2.54 mm pin pitch
and 22.86 mm row-center spacing remain provisional; the user has been asked
for measured row spacing. The outside dimensions alone do not establish
socket fit, USB access, component height or antenna clearance. No antenna
keepout qualification is claimed.

## Placement and local routing

- R26 (0 ohm) moves from (48, 58) to (92.8, 45) mm, toward the MCU/DAC
  boundary. Its pad nets remain AGND and power ground. Its reference label
  moves with it. No additional connection between those two nets is introduced.
- The enlarged module envelope exposed collisions with C_DMXLV1 and
  C_SCANLV1. Both move from x=125.5 to x=128.45 mm and rotate from 180 to
  0 degrees, retaining y=20.85 and y=31.5 mm respectively. Their +3V3 pad
  coordinates stay exactly fixed; only their ground pads change position.
- Ground connections use 0.4 mm F.Cu tracks to 0.60/0.30 mm ground vias:
  C_DMXLV1 has a 1.275 mm link to (131.2, 20.85); C_SCANLV1 has a 4.775 mm
  link to (134.7, 31.5). The existing shared ground route near the scan
  buffer is retained and anchored at (125, 20.85) with a 0.975 mm link.
- Two nearby +3V3 distribution segments are replaced by six short detour
  segments to clear the relocated ground pads. The capacitor-to-buffer VCC
  routing and all signal routes remain unchanged. Three reference labels
  are adjusted in addition to R26.
- Intermediate candidates had courtyard collisions, local shorts and an
  isolated ground branch. These were corrected, not excluded from DRC.

## Return copper

The V2.0.7 back-layer zone outlines left a large MCU-side gap: AGND ended
at x=94 mm; the power-ground zone began at x=124 mm. R26 was remote from
the crossing DAC input traces. Filled copper, not only zone outlines,
showed poor opposite-layer reference coverage along those routes.

Added two local F.Cu ground zones:

| Zone | Net | Outline bounds (mm) |
|---|---|---|
| DAC input return | AGND (from Galvo Board) | x=76–93.35, y=22–54 |
| MCU SPI return | 5V GND Buck | x=93.65–125, y=25–64 |

The zones retain 0.20 mm clearance, 0.25 mm minimum thickness and
0.30/0.40 mm thermal gap/spoke width, with isolated islands removed.
Three AGND stitching vias at (90.5, 26), (86, 30), and (90.5, 43) mm
connect local front/back copper. The three original zone outlines/settings
are preserved; their filled copper is recalculated. There are five ground
zones in total, with the two new zones explicitly named.

The [measurement script](../tests/measure_dac_return_layout.py) samples
trace centerlines at intervals no greater than 0.1 mm and tests for filled
ground polygons on the opposite layer. It includes bias branches; it is
not a point-to-point flight-time or impedance calculation.

| Input net | Total trace length, unchanged (mm) | V2.0.7 overlap | V2.0.8 overlap |
|---|---:|---:|---:|
| DAC_SCLK_3V3 | 37.434 | 1.799% | 75.602% |
| DAC_DIN_3V3 | 29.958 | 2.247% | 74.781% |
| DAC_SYNC_3V3 | 28.971 | 30.157% | 78.571% |
| DAC_CLR_3V3 | 32.983 | 31.869% | 90.677% |

These are geometric overlap estimates, **not proof of a continuous
low-inductance return path**. Split crossing, copper necks, pads, vias,
coplanar return, socket/module ground paths and coupled supply currents
require further review. Timing at the actual SPI rate and EMC remain open.
No arbitrary overlap percentage is used as a production pass threshold.

A 0.05 mm sampling run changes each result by less than 0.3 percentage
points. A query-only negative run with no selected reference-ground nets
reports zero overlap. An earlier in-memory native zone-deletion experiment
segfaulted and is not counted as a passing check; the separate read-only
measurement processes completed successfully. PCB files were not written
by those diagnostic runs.

## Verification and preservation

- All 122 component values/footprint assignments and all 110 electrical net
  memberships exactly match V2.0.7. The schematic changes only revision text.
- 119 component placements remain unchanged; only R26 and the two bypass
  capacitors move. Every pad retains its net, size, drill, shape, attribute
  and UUID. All U1 pad positions and both capacitor VCC positions are preserved.
- 1104 baseline copper objects retain exact geometry, net, width and ID.
  Ten obsolete objects are removed (seven tracks, three vias); fifteen are
  added (nine tracks, six vias). Final board: 1010 segments, 109 vias.
- Unrelated hidden-field font normalization from native save was restored
  before adoption. Project rules and the legacy PCB are unchanged.
- Fresh native ERC: zero violations. DRC with zone refill, all-track checks
  and schematic parity: zero errors/unconnected items, only the known buck
  footprint-type warning and two diode footprint-filter warnings.
- All six interface checkers, 33 unit tests, the draft-evidence guard and
  the existing buck-input/unique-copper-ID guard pass.
- Combined front/back copper, module outline and revised placements were
  rendered and inspected. The overlap measurements reproduce from the
  adopted PCB. Source hashes and detailed figures are in
  [return-checks.json](2026-09-14-return-checks.json).
- A relocated copy of the staged hardware/tests reproduces export/ERC, all
  six interface checkers, 33 unit tests, native DRC/parity/draft and buck
  guards, exact preservation and identical overlap figures. Source hashes
  match the working copy; untracked user files are absent.

Run the read-only geometric measurement with KiCad's Python bindings:

```sh
/usr/bin/python -B hardware/tests/measure_dac_return_layout.py \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_pcb'
```

Use the [current checkpoint](CURRENT-HARDWARE.md) for the full ERC/DRC and
interface-check commands. Firmware, shutdown behavior and KiBot are
unchanged. [Independent shutdown](2026-09-14-shutdown-boundary.md),
actual loads/thermal/EMC qualification, exact socket/module mechanics,
analog/DMX/fan interfaces, BOM/package/assembly and independent physical
review remain release gates.
