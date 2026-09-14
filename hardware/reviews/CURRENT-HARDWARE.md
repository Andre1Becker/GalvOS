# Current hardware checkpoint

Version/tag: **hw-v2.0.3-draft** (2026-09-14).

**FULLY CONNECTED ROUTING DRAFT — NOT FOR FABRICATION OR LASER OPERATION.**

## Artifacts

Paths are relative to `hardware/schematics/`.

| Artifact | State | SHA-256 |
|---|---|---|
| `Laser Controllerv2_only_for_pcb_test.kicad_sch` | V2.0.3; 116 components, 106 nets | `039f7f458c7276ae266f34b7865dba6b08804d1335d9a71d4755312799bb3826` |
| `Laser Controllerv2_only_for_pcb_test.kicad_pcb` | Routed v2 engineering draft with three ground zones | `b3d9add35c7acfa7a4094f1a811fa3058481ea25f2427d7640802e7c9ca48372` |
| `Laser Controller.kicad_pcb` | Unchanged historical board; not the v2 layout | `9703ba1bf5343e8a77ab9dbb0781785384c4f238bedc52c1341898d9e9bbf6b2` |

## Implemented

- Preserve the approved external 12 V fan rails: J4.1–J5.2 and J6.1–J7.2, separate from each other and the buck positive rails. J2 supplies 12.6 V to the 5 V buck.
- Schematic electrical netlist is exactly unchanged from v2.0.2: component values, footprints and pin memberships match. Only its revision text changed. Firmware and safety circuitry are unchanged.
- PCB: 175 x 115 mm closed outline, two copper layers, nominal 1.6 mm thickness, 116 footprints, 964 trace segments, 103 vias and three filled, named ground zones.
- Repositioned seven analog parts for local feedback/supply routing; routed analog feedback, output and bypass connections manually. All 30 pre-autoroute segments survived the routing import exactly, along with every footprint position and pad net.
- Completed remaining routing in an isolated offline candidate, corrected sub-minimum neckdowns, added an R9 escape via and verified the candidate before adopting it.
- Added B.Cu AGND and power-ground planes, plus local F.Cu buck thermal copper. U_BUCK1 exposed-pad/thermal-hole pad 9 uses solid zone connection. Isolated copper removal is enabled. These first planes do not qualify the MCU/DAC return boundary or system EMC.
- `PowerDraft` retains provisional 0.8 mm routing for both fan-positive rails, VIN_RAW, VIN_BUCK and Buck +5V. Default signal routing is 0.25 mm, minimum 0.20 mm; vias are 0.60/0.30 mm. Minima include 0.20 mm copper clearance, 0.50 mm copper-to-edge and 0.10 mm silk clearance. No current rating or copper-weight/temperature qualification is claimed.

## Verification

KiCad 10.0.6, native all-severity DRC with schematic parity and freshly refilled zones:

- ERC: **0 violations**.
- PCB: **0 unconnected items**, **0 non-routing errors**, **1 footprint-type warning**.
- Native schematic-parity section: **2 footprint-filter warnings**, no other mismatches.
- No reported shorts, clearance/courtyard conflicts, undersized tracks, silk collisions, isolated copper or starved thermals.
- DAC, sensor/tach and fan-power checks pass; exact electrical preservation against the v2.0.2 export passes.
- New draft-evidence guard passes; nine unit tests cover disabled/stale checks, relaxed minima, filtered/incomplete reports, errors, exclusions and unexpected warnings. A passing draft guard is not a production approval.
- Front and back routing and filled back copper were rendered and inspected. Native inspection confirms three zones are filled and assigned to the intended nets.
- Export, interface tests, nine unit tests, ERC and the same native DRC/parity result were reproduced from a relocated copy of the staged project without untracked user files.

Known warnings remain visible:

1. `U_BUCK1`: KiCad's type heuristic expects through-hole because the SMD footprint contains plated thermal holes. Qualify the exposed-pad, solder/paste, thermal-hole and assembly construction.
2. `D_TRIGCL_SCAN1` and `D_TRIGCL_WD1`: SOD-123 footprints conflict with the existing symbols' DO-35 filters. Qualify actual diode MPN/package/polarity before resolving this metadata/BOM mismatch.

Correction to the v2.0.2 report: its committed project actually retained **0.00 mm** minimum silk clearance, despite the documented 0.10 mm intent. Native save/export operations reset this setting. V2.0.3 explicitly stores and verifies 0.10 mm; the new guard rejects the old/reset value and skipped checks. No findings were excluded.

## Reproduce

```sh
kicad-cli sch export netlist --format kicadxml -o /tmp/galvos-current.xml \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
PYTHONDONTWRITEBYTECODE=1 python hardware/tests/check_fan_power.py /tmp/galvos-current.xml
PYTHONDONTWRITEBYTECODE=1 python hardware/tests/check_sensor_fan_interfaces.py /tmp/galvos-current.xml
PYTHONDONTWRITEBYTECODE=1 python hardware/tests/check_dac_interface.py /tmp/galvos-current.xml
PYTHONDONTWRITEBYTECODE=1 python -m unittest discover -s hardware/tests -p 'test_pcb_draft.py'
kicad-cli sch erc --format json --severity-all --exit-code-violations \
  -o /tmp/galvos-current-erc.json \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
kicad-cli pcb drc --refill-zones --schematic-parity --all-track-errors \
  --format json --severity-all -o /tmp/galvos-current-drc.json \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_pcb'
PYTHONDONTWRITEBYTECODE=1 python hardware/tests/check_pcb_draft.py \
  /tmp/galvos-current-drc.json \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_pro'
```

PCB DRC deliberately collects the remaining warnings. Its process exit code alone is not proof. Read the report and run the draft guard; do not disable package warnings to obtain an apparent clean result.

## Release remains blocked by engineering work

Routing connectivity is complete, not production qualification. Review/refine the MCU/DAC return boundary and R26 placement, high-speed paths and timing, buck hot loop/feedback/thermal layout, actual load currents/copper weight, and external interfaces. Antenna/USB keepouts, enclosure/mounting/connectors, BOM/assembly review and independent review are still unqualified.

Existing circuit gates remain: independent Class 4 shutdown/rearm, E-stop/override behavior, RGB reset-off drive, 5 V GPIO interfaces and fan PWM contracts, analog output range, scan-fail limitations, buck ratings and input protection. Physical testing is required. V1 perfboard operation does not qualify v2 or fault safety. No Gerbers were generated.

See [routing evidence](2026-09-14-v2-routing.md), [remaining plan](2026-09-14-v2-pcb-plan.md) and `codex-todos.md`.
