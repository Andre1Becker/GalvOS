# Laser Controller PCB — layout notes

Board: **118 × 90 mm, 2 layers**, placed and **fully routed** — 257 tracks,
14 vias, **zero unconnected items and zero clearance violations**. Gerbers and
drill files are in [../gerbers/](../gerbers/). See
[Routing status](#routing-status) for the three remaining non-blocking items.

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

Routed with Freerouting 2.0.1 via the KiCAD MCP server. Design rules: 0.2 mm
clearance, 0.2 mm minimum track, 0.3 mm minimum drill. Track widths: signals
0.2 mm, supply rails 0.4 mm. Both ground nets are carried by pours.

DRC: **0 clearance violations, 0 unconnected items.** What remains is not
fab-blocking:

- **3 starved thermals** — three pads reach their pour through a single
  thermal spoke instead of two. Electrically connected; the rule is a
  robustness convention.
- **71 silkscreen warnings** — reference designators overlapping pads and each
  other. Cosmetic; tidy the silk if you care about the printing.

### If you re-run the router

Three things cost real time here; the recipe that works is: export DSN → patch
it → run Freerouting → import SES → refill pours → widen rails.

1. **The DSN carries `(clearance 50 (type smd_smd))`.** That tells Freerouting
   0.05 mm from an SMD pad is fine, and it will happily take it — the first
   run produced 31 violations down to 0.0017 mm, all against U2 and U12 pads.
   Delete that line and raise `(clearance …)` before routing.
2. **Freerouting ignores DSN net classes.** A `power` class at `(width 500)`
   came back with every path still at 200 µm, and the `Power` class in the
   `.kicad_pro` never reaches the exporter either (`assignments: 0`). So the
   board is routed at a deliberately generous **0.45 mm** clearance and the
   rails are widened afterwards into that slack.
3. **Widening after routing eats clearance.** Growing a 0.2 mm trace to 0.6 mm
   moves each edge out by 0.2 mm and re-broke the clearance that had just been
   fixed. The widening pass is therefore clearance-aware: each track only grows
   as far as its nearest foreign pad, via or track allows, leaving the 0.2 mm
   rule intact. Two traces near U2 stay narrow because of this.

Pours must be refilled after every SES import — otherwise DRC reports a flood
of bogus zero-clearance errors (283 in the first run) from tracks sitting on
unfilled zone copper.

## Before this goes to a fab

1. **Decide how the SD card attaches.** `Connector_Card:SD_Card_Device_16mm_SlotDepth`
   is a DIY pad field — its stock Edge.Cuts "slot" would cut away the very
   copper the card contacts, so those cuts were stripped and the board is a
   plain rectangle. Either print a card retainer, or swap J1 for a 1x06 header
   and plug in a micro-SD breakout (which is what the perfboard build uses).
2. **Mind the analog headroom.** With R7/R12 = 10k/22k the stage runs at
   ×2.2, so a full-scale DAC swing gives ±5.5 V into a ±5 V galvo input —
   that is what the firmware's ~95 % DAC clamp is for.
3. R8 is **DNP** on purpose. Populating it shifts the summing node's DC
   operating point and the output no longer centres at 0 V.

## Known non-issues

- ERC reports one error: `J1` pin 4 (CLK) is typed *power input* in KiCad's own
  `Connector:Micro_SD_Card` symbol. Library defect, not a wiring fault.
- 3 starved thermals and 71 silkscreen warnings, as listed above.
