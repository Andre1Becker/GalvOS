# Current hardware checkpoint

Version/tag: **hw-v2.0.2-draft** (2026-09-14).

**PARTIALLY ROUTED — NOT FOR FABRICATION OR LASER OPERATION.**

## Artifacts

Paths are relative to `hardware/schematics/`.

| Artifact | State | SHA-256 |
|---|---|---|
| `Laser Controllerv2_only_for_pcb_test.kicad_sch` | V2.0.2; 116 components, 106 nets | `87fc469a1e351f4c436d61f8bb4dd374461f6ce6a11a6a050cf51f33e386ef28` |
| `Laser Controllerv2_only_for_pcb_test.kicad_pcb` | New v2 board; partial routing | `9f4674cdfd56fb373a079f2ce10e151416df3c596b0a46e7d271d550969b0ea3` |
| `Laser Controller.kicad_pcb` | Unchanged historical board; not the v2 layout | `9703ba1bf5343e8a77ab9dbb0781785384c4f238bedc52c1341898d9e9bbf6b2` |

## Changes and preservation

- J2 is identified as the external 12.6 V buck input; the buck supplies 5 V electronics.
- J4.1 connects only to J5.2 on `FAN1_12V`; J6.1 connects only to J7.2 on `FAN2_12V`. These external 12 V positive rails are separate from each other and the buck positive rails. Common power ground remains in the schematic; PCB ground routing is incomplete.
- Baseline comparison permits exactly three input-value changes and the four fan-positive pin departures from the old 5 V net. All other component values, footprints and net memberships are preserved.
- V2.0.1 sensor connector pinouts, tach GPIO2/GPIO9 assignments and DAC translator topology remain intact. Firmware, inverted RGB and safety circuitry are unchanged.
- New PCB: closed 175 x 115 mm rectangle, two copper layers, 1.6 mm nominal thickness, 116 footprints, 15 track segments, no added vias or zones. No legacy copper was imported.
- Functional placement, initial fan feed/return tracks and several buck input/bootstrap/output connections are present. Reference text was improved, duplicate silkscreen pin-1 marks removed, and draft/input/polarity markings added. Board rendering was inspected.
- Project rules: 0.20 mm minimum copper clearance/track width, 0.25 mm default signal width, 0.60/0.30 mm minimum via diameter/drill, 0.50 mm copper-to-edge and 0.10 mm silk clearance. Routed widths span 0.25–0.80 mm. These are provisional geometry rules, not a fabricator approval or load/temperature rating.

## Verification

KiCad 10.0.6, all-severity checks:

- Schematic ERC: **0 violations**.
- DAC, sensor/tach and fan-power connectivity checks: **PASS**. Fan baseline preservation: **PASS**.
- Negative fan tests rejected merged fan rails, connection to either buck positive rail, incorrect J2 marking and a changed fan ground connection.
- Export, baseline/connectivity checks, ERC and the same PCB finding counts were reproduced from a relocated copy of the staged project, without the user's untracked files.
- PCB DRC: **251 unconnected items**; **1 other warning**, no other geometric/type errors. No reported shorts, clearance/courtyard conflicts or silkscreen collisions.
- Native schematic-parity section: **2 footprint-filter warnings**, no other mismatches. This is not a completely clean parity result.
- No DRC exclusions were added. Missing-courtyard, footprint-filter/type and off-center-via checks are enabled. The default tuning-profile geometry check remains disabled; no tuning profiles are used.

Remaining native warnings:

1. `U_BUCK1`: SMD footprint contains plated thermal holes; KiCad's type check expects through-hole. Review actual DDA exposed-pad/thermal-hole construction and assembly; do not change the SMD designation merely to suppress the warning.
2. `D_TRIGCL_SCAN1`, `D_TRIGCL_WD1`: assigned SOD-123 footprints do not match the existing symbol's DO-35 filter. Qualify actual diode MPN/package/polarity before resolving metadata/BOM mismatch.

Reproduce from the repository root:

```sh
kicad-cli sch export netlist --format kicadxml -o /tmp/galvos-current.xml \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
PYTHONDONTWRITEBYTECODE=1 python hardware/tests/check_fan_power.py /tmp/galvos-current.xml
PYTHONDONTWRITEBYTECODE=1 python hardware/tests/check_sensor_fan_interfaces.py /tmp/galvos-current.xml
PYTHONDONTWRITEBYTECODE=1 python hardware/tests/check_dac_interface.py /tmp/galvos-current.xml
kicad-cli sch erc --format json --severity-all --exit-code-violations \
  -o /tmp/galvos-current-erc.json \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
kicad-cli pcb drc --schematic-parity --format json --severity-all \
  -o /tmp/galvos-current-drc.json \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_pcb'
```

PCB DRC omits `--exit-code-violations` for report collection: zero process exit code is **not** a passing board. Inspect all three finding arrays. The fan check's optional second argument is a netlist from `hw-v2.0.1-draft`, not older DAC/sensor baselines.

## Still required

Complete and review routing, common-ground distribution/R26 bridge, buck hot loop/feedback/thermal design, current sizing, mounting/connectors and MCU antenna/USB clearance. No antenna keepout or mounting locations are qualified. Resolve connectivity/package warnings, obtain independent layout/BOM review and perform physical qualification. No Gerbers were generated.

Existing electrical release blockers remain: independent Class 4 shutdown/rearm, E-stop/override behavior, RGB reset-off drive, unqualified 5 V GPIO interfaces and fan PWM contracts, analog output range, scan-fail limitations, DAC timing, buck ratings and input protection. V1 perfboard operation is user-reported functional evidence, not v2 or fault-safety qualification. See `codex-todos.md` and dated hardware reviews.
