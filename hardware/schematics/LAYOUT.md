# Laser Controller PCB — layout notes

Board: **118 × 90 mm, 2 layers**. Placement is done and DRC-clean; **routing is
not done** — 92 signal connections are still open. Both ground nets are already
complete via copper pours.

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

## Before this goes to a fab

1. **Route the remaining 92 connections.** KiCad has no autorouter; this is
   manual work.
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
