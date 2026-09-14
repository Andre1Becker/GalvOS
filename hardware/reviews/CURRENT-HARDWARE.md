# Current hardware checkpoint

Version/tag: **hw-v2.0.5-draft** (2026-09-14).

**FULLY CONNECTED ROUTING DRAFT — NOT FOR FABRICATION OR LASER OPERATION.**

## Artifacts

Paths are relative to `hardware/schematics/`.

| Artifact | State | SHA-256 |
|---|---|---|
| `Laser Controllerv2_only_for_pcb_test.kicad_sch` | V2.0.5; 122 components, 110 nets | `61eddc9cc116d27805ba51afe990f4846717bedd4ed8d1236df5769847c212ea` |
| `Laser Controllerv2_only_for_pcb_test.kicad_pcb` | Routed v2 engineering draft with three ground zones | `409bdad5b96b8d2ca58b411886f56c5680fd6183dce24af600a56ee80ec83d0f` |
| `Laser Controller.kicad_pcb` | Unchanged historical board; not the v2 layout | `9703ba1bf5343e8a77ab9dbb0781785384c4f238bedc52c1341898d9e9bbf6b2` |

## Implemented

- V2.0.5 adds U_DMXLV1 (SN74LVC1G17DBVR), R_DMXIN1 (10 kohm pull-down) and C_DMXLV1 (100 nF bypass). J_DMX1.3 now drives the 5.5 V-tolerant input; the 3V3-powered noninverting output drives GPIO4. The module supply/pinout and firmware are unchanged. A disconnected module holds RX LOW/break; this is not an independent laser-off function.
- Against v2.0.4, exactly those three components and the RO/3V3/power-ground memberships change. All 119 previous footprints/placements and pad geometry are preserved; only J_DMX1.3 changes pad net. Of 1098 baseline copper items, 1097 retain exact geometry (five renamed); one RO diagonal is trimmed by 1.8 mm on each axis. All retained item UUIDs were restored after router import. New copper is confined to the local DMX stage and its supply/ground connections.

- Preserve the approved external 12 V fan rails: J4.1–J5.2 and J6.1–J7.2, separate from each other and the buck positive rails. J2 supplies 12.6 V to the 5 V buck.
- Added U_SCANLV1 (SN74LVC1G17DBVR), R_SCANIN1 (10 kohm input pull-down) and C_SCANLV1 (100 nF local bypass). The timer's 5 V output no longer directly drives GPIO39. The buffer uses MCU +3V3 and power ground, preserving HIGH=OK. Full supply/temperature/fault qualification remains open; see the scan-status review.
- The earlier V2.0.4 change against v2.0.3 added exactly three components and changed only the status/3V3/power-ground memberships. All previous values, footprints and unrelated electrical memberships match. Firmware and shutdown/arming logic are unchanged.
- PCB: 175 x 115 mm closed outline, two copper layers, nominal 1.6 mm thickness, 122 footprints, 1017 trace segments, 111 vias and three filled, named ground zones.
- In V2.0.4, all original 116 footprint placements and pad geometries are preserved. Removed one original GPIO39 segment at the MCU end and reassigned the other six status copper items to SCAN_STATUS_5V. All 1066 retained baseline copper items were verified exactly after that rename. Unrelated autorouter normalization was restored from the baseline.
- U1 already specifies two 1x22 female socket strips; corrected the footprint's stale 21/20-pin description. Retained all 44 pads, 2.54 mm pitch and provisional 22.86 mm row spacing. Actual fit and antenna/USB clearance remain unqualified.
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
- DAC, sensor/tach, fan-power, scan-status and DMX checks pass. The DMX checker also verifies exactly the approved electrical delta against the v2.0.4 netlist.
- Draft-evidence guard passes. Twenty-eight unit tests cover nine report/rule guards, eight scan-buffer cases and eleven DMX cases. A passing draft guard is not a production approval.
- Front and back routing and filled back copper were rendered and inspected. Native inspection confirms three zones are filled and assigned to the intended nets.
- Export, all five interface checks, twenty-eight unit tests, ERC and the same native DRC/parity result were reproduced from a relocated copy of the staged project without untracked user files.

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
PYTHONDONTWRITEBYTECODE=1 python hardware/tests/check_scan_status.py /tmp/galvos-current.xml
PYTHONDONTWRITEBYTECODE=1 python hardware/tests/check_dmx_interface.py /tmp/galvos-current.xml
PYTHONDONTWRITEBYTECODE=1 python -m unittest discover -s hardware/tests -p 'test_*.py'
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

Existing circuit gates remain: independent Class 4 shutdown/rearm, E-stop/override behavior, RGB reset-off drive, DMX module/receiver/cable qualification and fan PWM contracts, analog output range, scan-fail limitations and full buffer/timer qualification, buck ratings and input protection. Physical testing is required. V1 perfboard operation does not qualify v2 or fault safety. No Gerbers were generated.

See [DMX input change](2026-09-14-v2-dmx-input.md), [scan-status change](2026-09-14-v2-scan-status.md), [v2.0.3 routing evidence](2026-09-14-v2-routing.md), [remaining plan](2026-09-14-v2-pcb-plan.md) and `codex-todos.md`.
