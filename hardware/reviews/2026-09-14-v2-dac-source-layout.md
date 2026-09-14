# DAC source-termination layout — V2.0.9 draft

Date: 2026-09-14. Baseline: `f29ffd0494d83855157dfe3d3dfa84a241f2b0d5`
(`hw-v2.0.8-draft`). **Not for fabrication or laser operation.**

## Evidence and change

The [DAC interface review](2026-09-14-dac-interface.md) already requires the
22 ohm resistors immediately beside U_DACLV1's outputs. Native measurement
found long un-terminated source links, including two vias on the clock.
Moved only R_DACCLR1, R_DACSYNC1, R_DACDIN1 and R_DACSCLK1, retained their
1206 footprints, values, orientation and electrical nets, and rerouted their
eight source/DAC-side nets locally. All four source links now use F.Cu only.

| Signal | Source length before (mm) | After (mm) | Source vias before → after | New resistor center (mm) |
|---|---:|---:|---|---|
| SCLK | 11.629 | 2.962 | 2 → 0 | 73.2, 33.8 |
| DIN | 7.238 | 2.796 | 0 → 0 | 73.2, 31.2 |
| SYNC | 6.783 | 4.563 | 2 → 0 | 73.2, 28.6 |
| CLR | 16.095 | 6.352 | 2 → 0 | 73.2, 26.0 |

These are the complete trace lengths on the buffer-to-resistor nets, not
buffer-to-DAC delay or total signal length. The DAC-side routes change and
one DAC-side CLR via is added. The four existing 1206 packages constrain
fanout spacing. This improves source-resistor locality; it does not prove
termination value, rise time, ringing, impedance, timing or EMC compliance.

The isolated offline Freerouting 2.4.1 run supplied only the four DAC-side
routes. No complete router import was adopted: it normalized unrelated
copper and attempted unrelated connections. Only the selected local paths
were recreated on the baseline; a 0.1874 mm neckdown was raised to the
existing 0.20 mm minimum. Five obsolete DAC-side tails were removed.
References retain 0.8 mm text and 0.12 mm stroke, relocated clear of pads/silk.

## Preservation and checks

- 122 component values/footprint assignments and all 110 net memberships
  are identical to V2.0.8. Schematic change is draft-revision text only.
- 118 placements unchanged. All pad geometry, relative positions, nets and
  UUIDs unchanged; all footprint rotations unchanged.
- 1084 retained copper objects preserve exact UUIDs, nets, coordinates,
  layers, widths and drills. Removed 35 objects (30 old source objects and
  five obsolete DAC-side tails), added 31 local objects.
- Final board: 1011 segments, 104 vias, 122 footprints, five filled ground
  zones. Outline/layer stack and project-rule file unchanged.
- Native KiCad 10.0.6 ERC: zero findings. Refilled, all-track DRC with
  schematic parity: zero errors and zero unconnected items. Only the
  previous buck footprint-type warning and two diode-filter warnings remain.
- Six interface checkers, 33 existing unit tests, draft-evidence guard and
  buck-input/unique-copper-ID guard pass.
- New native `check_dac_source_layout.py` verifies each 22 ohm source
  link as one connected, branch-free F.Cu chain, without vias, at least
  0.20 mm wide. Its 3.2/3.2/4.8/6.6 mm bounds are project-local regression
  limits, not manufacturer ratings.
- V2.0.8 fails the new source guard. Six independent text-mutated PCB
  fixtures are rejected: missing segment, added via, wrong layer, branch,
  excessive length and wrong resistor value. An initial native in-memory
  mutation harness failed during object teardown/reloading; it is not
  counted as successful test evidence.
- The four MCU-side geometric ground-overlap measurements remain identical
  to V2.0.8 (75.602%, 74.781%, 78.571%, 90.677%). This remains geometric
  evidence only. Native front-copper/silk and combined-layer views inspected.
- A relocated staged hardware copy repeats netlist export, all six interface
  checks, 33 unit tests, ERC, native DRC/parity and both native layout guards.
  The artifact hashes, preservation proof and overlap measurements match.
  User-owned untracked files are not included.

## Reproduce

Use the current checkpoint's export, six interface checks, unit tests,
ERC, all-track DRC/parity and draft-evidence commands, then:

```sh
/usr/bin/python -B hardware/tests/check_dac_source_layout.py \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_pcb'
/usr/bin/python -B hardware/tests/check_buck_input_layout.py \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_pcb'
/usr/bin/python -B hardware/tests/measure_dac_return_layout.py \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_pcb'
```

Hashes and measurements: [source-layout evidence](2026-09-14-dac-source-checks.json).
Current artifacts: [hardware checkpoint](CURRENT-HARDWARE.md).

## Remaining release gates

The existing 40 MHz timing budget has only 0.6 ns ideal SCLK pulse-width
reserve before source/PCB effects. This revision changes neither firmware
clock nor manual CS behavior; qualify SCLK/DIN/SYNC/CLR at the DAC pins
under actual rails, temperature and load. Source locality alone cannot
close this gate.

Independent Class 4 shutdown/rearm, RGB reset-off, actual fan/5 V loads,
buck thermal/stability/protection, module pin-grid and antenna/USB mechanics,
BOM/package/assembly review and physical qualification remain open.
KiBot configuration still awaits user direction. No Gerbers generated.
