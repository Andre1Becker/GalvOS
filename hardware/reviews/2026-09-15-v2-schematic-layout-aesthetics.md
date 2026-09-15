# V2.0.10 schematic layout-aesthetics review

Date: 2026-09-15
Tool: KiCad 10.0.6
Scope: canonical schematic drawing only

## Result

The canonical V2.0.10 schematic was redrawn as a single-sheet engineering
drawing with five functional rows. Signal sources are on the left, processing
is central, outputs are on the right, and supply/return symbols are placed
above/below their loads. This is a drawing-only revision: the XML netlist still
contains exactly 122 components and 110 nets with identical component
properties, library data, pin membership and net names.

The result is still an **engineering draft, not approved for fabrication or
laser operation**. The separate SSR-only restart-interlock design was not
implemented or implied by this work.

## Artifact identity

| Artifact | Baseline SHA-256 | Final SHA-256 |
|---|---|---|
| Canonical schematic | `8c1796266cc1843ee5e76cb9ab3a1025354a36771a9495bd96aa1e50c6ad0cc5` | `4f36655a3db6b5c36ef65a4722ed1f9eb7d4c4fcdeb77973966a9bf6b506f086` |
| Schematic PDF | `bbfc5fb4335a76f11f08f506cf9931459e7073332e4c70010572443b0927156e` | `c4b863705803ee55f5cea0628ce073e603d91b3b57d3fccb38cf3b6bce4ad9d6` |
| Canonical PCB | `b8111d778abc5b57ec3adb1bbc94060598fb87cc0d16e584e53e28c0bce4f47a` | unchanged |
| KiCad project | `dc77f4155067018d81c509e667ad67642031a5cf69abe7181144c19c11bd3b1b` | unchanged |

The baseline is commit `d4cf341`. The final PDF was exported directly from the
canonical schematic, without an export-only formatting copy.

## Presentation changes

| Functional row | Left / input | Centre / processing | Right / output |
|---|---|---|---|
| Power | J2/J3/J4/J6 | D2, input filtering, U_BUCK1 and feedback | L1, output filtering and distributed rails |
| I/O | SD, DMX and five temperature inputs | U_DMXLV1 and U1 | two fan PWM/tach connectors |
| Analogue | four 3.3 V DAC controls | U_DACLV1, U2 and U12 | U13 galvo output |
| Laser | U1 RGB controls and pull-ups | three parallel U15/U16/U17 lanes | U4/U5/U3 outputs |
| Safety | E-stop and monitored inputs | watchdog, scan timer and status buffer | SSR and MCU-status boundaries |

Long cross-sheet conductors were replaced with exact-name labels and compact
power symbols. Internal generated `Net-(...)` labels remain exact and use a
reduced text size so they do not dominate the drawing. No bus objects were
introduced.

Source-level wire metrics changed from 329 wire objects, including 91 segments
longer than 20 mm and a 170.18 mm maximum, to 208 local stubs with a 3.81 mm
maximum and zero segments longer than 20 mm. The final sheet contains 151 local
labels and 71 global labels. Three embedded reference images, two decorative
rectangles and their redundant connector annotations were removed.

All component anchors, label anchors, no-connect markers and wire vertices are
on the 1.27 mm grid. Visible Reference and Value fields are horizontal and
mechanically standardized. The principal chains checked left-to-right are DMX,
DAC, all three laser channels and the buck path.

## Visual inspection

The complete PDF and 240 dpi crops were inspected across all five rows. Rotated
passive Reference/Value fields now render horizontally above/below the device,
and the U15/U16/U17 Reference/Value, pin and power annotations are separated.
No ambiguous long-wire crossing remains. The dense custom symbols `U_DACLV1`,
`U2` and `U12` retain library-generated internal pin text and nearby
power-annotation density; this review does not claim those library-owned pin
texts are collision-free because they cannot be moved independently without
changing the symbol libraries. Exact generated internal net labels are
deliberately rendered at 0.508 mm: they remain visible but only as a secondary
implementation detail.

## Verification evidence

| Gate | Result |
|---|---|
| XML electrical-preservation comparison | PASS — 122 components, 110 nets |
| Full presentation contract | PASS — all five blocks |
| Native schematic ERC, all severities | PASS — 0 violations |
| Fan-power, sensor/fan, DAC, scan, DMX, buck and trigger-diode interfaces | PASS — 7/7 |
| Hardware unit tests | PASS — 69 tests |
| Native PCB DRC with refill and schematic parity | 0 unconnected, 0 parity, 1 known warning |
| PCB draft-evidence guard | PASS |
| DAC source-layout guard | PASS — 2.796–6.352 mm source paths, no vias |
| Buck input-layout guard | PASS — 6.905 mm VIN and GND paths |
| PCB/project SHA-256 lock | PASS — byte-identical |

The one DRC warning is the existing `U_BUCK1` footprint-type mismatch: KiCad's
heuristic sees plated thermal holes in an SMD package. It remains a release
blocker requiring assembly/thermal construction qualification; no warning was
suppressed or severity weakened.

## Explicit non-claims

- No electrical component, value, footprint, pin net, PCB object, project rule
  or firmware file was changed.
- No SSR driver, restart latch, contact supply or independent safety interlock
  was added.
- The drawing cleanup does not qualify current capacity, thermal behavior,
  timing, EMC, analogue range, laser reset-off behavior or fault safety.
- No Gerbers or production assembly files were generated.
