# Laser Controller PCB — layout notes

Board: **118 × 90 mm, 2 layers**. Placement is done, and the board is now
**fully routed** (274 tracks, 10 vias, no unconnected items) via Freerouting.
**It is not fab-ready yet** — 31 clearance violations remain, all of them where
the autorouter squeezed traces between fine-pitch SMD pads. See
[Routing status](#routing-status).

## Floor plan

```text
 +--------------------------------------------------------------+
 | J3 +-15V        ANALOG ISLAND (AGND pour, F.Cu + B.Cu)        |
 |                                                               |
 | U13 --- R6/R16 --- U12 --- R7..R17 --- X/Y_Knoten --- U2 --- U1
 | galvo   100R      OPA4134   dividers    100R + 1nF    DAC   ESP32
 |                                                        R27/R28  |
 | ------------------------------- R26 (star) ------------------ |
 |         OPTO / LASER TTL          |  U1 (cont.)   | J1 SD pads |
 | J2 +5V  U15 U16 U17 + C5..C7      |               | R29        |
 | D1      R1/R2/R4, R18..R23        |               | U6..U10 R5 |
 |         U3 U4 U5 (RGB out)        |               |            |
 +--------------------------------------------------------------+
```

## Why things sit where they do

- **Signal flow is left-to-right in reverse**: U1 → U2 (SPI2) → R11/R13 + C3/C4
  → X/Y_Knoten → R7/R14 → U12 → R6/R16 → U13. The galvo output connector is on
  the far left edge, as far from the optocouplers as the board allows.
- **Split ground.** An AGND pour (both layers) covers the analog island up to
  y = 46 mm; digital GND pours over everything else. The two meet only at
  **R26 (0 R)**, which straddles the boundary — pad 1 in AGND, pad 2 in GND.
  Do not add a second bridge anywhere.
- **The optocouplers are the noisy block** (LED currents switch at the 50 kHz
  LEDC rate), so they sit below the split, next to their output headers.
- **J3 (±15 V / AGND) is inside the analog island** so the analog supply return
  never crosses digital ground. J2 (+5 V / GND) sits in the digital half.
- **SD card contacts (J1) sit beside U1's SPI3 pins** (GPIO5/6/1/42 are at the
  top of the module), not at the bottom.
- **U2's ground fanout is already routed** — a 0.5 mm-pitch VSSOP has inner
  ground pads no pour can reach, so pins 3/4 run to a via down to the AGND
  plane.

## Routing status

Routed with Freerouting 2.0.1 via the KiCAD MCP server. Track widths: signals
0.2 mm, +3V3 / grounds 0.5 mm, ±15 V and 5 V rails 0.6 mm (widened after import,
see below). Design rules: 0.2 mm clearance, 0.2 mm minimum track.

**31 clearance violations remain, 20 of them below 0.10 mm** — near-shorts, not
cosmetic. They cluster on exactly two parts:

| Location | Part | Violations |
| --- | --- | --- |
| ~(55, 30) | U2 — DAC8562, 0.5 mm-pitch VSSOP | 9 |
| ~(25–35, 20–30) | U12 — OPA4134, SOIC-14 | 7 |
| scattered | opto block, misc | 15 |

Freerouting routes between fine-pitch pads where there is no room. **Rip up and
hand-route the fanouts of U2 and U12 before fabricating.** Doing the analog
section by hand is worth it anyway: an autorouter has no notion of keeping the
op-amp feedback loops short or respecting the AGND/GND split.

Two quirks worth knowing if you re-run the autorouter:

- Freerouting exports at a fixed `(width 200) (clearance 200)` and drops every
  net into one `kicad_default` class — the `Power` net class in the
  `.kicad_pro` does **not** reach it (`assignments: 0`). Power widths here were
  applied *after* the SES import, not during routing.
- The SES import leaves the copper pours unfilled, so DRC reports a flood of
  bogus zero-clearance errors (283 in the first run). Refill zones first, then
  read the DRC.

## Before this goes to a fab

1. **Fix the 31 clearance violations** — see above; U2 and U12 need hand-routing.
2. **Decide how the SD card attaches.** `Connector_Card:SD_Card_Device_16mm_SlotDepth`
   is a DIY pad field — its stock Edge.Cuts "slot" would cut away the very
   copper the card contacts, so those cuts were stripped and the board is a
   plain rectangle. Either print a card retainer, or swap J1 for a 1x06 header
   and plug in a micro-SD breakout (which is what the perfboard build uses).
3. **Mind the analog headroom.** With R7/R12 = 10k/22k the stage runs at
   ×2.2, so a full-scale DAC swing gives ±5.5 V into a ±5 V galvo input —
   that is what the firmware's ~95 % DAC clamp is for.
4. R8 is **DNP** on purpose. Populating it shifts the summing node's DC
   operating point and the output no longer centres at 0 V.

## Known non-issues

- ERC reports one error: `J1` pin 4 (CLK) is typed *power input* in KiCad's own
  `Connector:Micro_SD_Card` symbol. Library defect, not a wiring fault.
- KiCad will create `Laser Controller.kicad_pro` on first open; the board was
  generated headlessly and ships without one.
