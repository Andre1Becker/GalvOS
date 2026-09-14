# Current hardware checkpoint

Electrical/PCB revision: **hw-v2.0.10-draft** (2026-09-14).
Schematic presentation: readability update after export commit `995eb85`;
see [drawing-only review](2026-09-14-v2-schematic-readability.md).
The existing hardware tag is unchanged; Git records this presentation update.

**FULLY CONNECTED ROUTING DRAFT — NOT FOR FABRICATION OR LASER OPERATION.**

User scope decision (2026-09-14): thermal, load and timing work is excluded
from further activity unless requested again. Existing evidence and warnings
remain recorded; these subjects are **unverified, not passed**. Continue
mechanical, BOM/assembly, manufacturing-data and remaining safety/interface
work. The exclusion does not imply a thermal/current/timing guarantee or
fabrication/laser-operation approval.

## Artifacts

Paths are relative to `hardware/schematics/`.

| Artifact | State | SHA-256 |
|---|---|---|
| `Laser Controllerv2_only_for_pcb_test.kicad_sch` | V2.0.10, readability update; 122 components, 110 nets | `adb9bf0af905d7a063de0bc2b287c3bc65b554794207a8d884ebd85c7f033f29` |
| `Laser Controllerv2_only_for_pcb_test.kicad_pcb` | Routed v2 engineering draft with five ground zones | `267c312d4451f386fe3bcb2e500c779456d1787064c24c7be033837a4de578ee` |
| `Laser Controller.kicad_pcb` | Unchanged historical board; not the v2 layout | `9703ba1bf5343e8a77ab9dbb0781785384c4f238bedc52c1341898d9e9bbf6b2` |

## PDF and PCB views

PCB views retain V2.0.10 source commit `f58ff538b2cda6d26dd76c6a4b9417fa833b695b`.
The schematic PDF is refreshed directly from the current canonical schematic.
All exports are stored directly in `hardware/`:

- [Schematic PDF](../GalvOS_V2.0.10_Schematic.pdf)
- [PCB top](../GalvOS_V2.0.10_PCB_Top.png)
- [PCB bottom](../GalvOS_V2.0.10_PCB_Bottom.png)
- [PCB perspective](../GalvOS_V2.0.10_PCB_Perspective.png)

The canonical schematic now uses a 620 x 440 mm page and separate draft
title warnings; no temporary export-only formatting is needed. Six IC
reference/value pairs and 22 nearby labels have been repositioned/oriented
for readability, with all electrical content preserved. Other legacy text
overlaps remain; this is not a complete drawing-readability sign-off.
The native KiCad 10.0.6 PNGs are unchanged at 2384 x 1568 pixels. Some parts
have no visible 3D model (including the ESP32/socket assembly and some ICs),
so these are CAD views, not proof of complete assembly or mechanical fit.
All draft restrictions remain applicable.

## Implemented

- Drawing-only update after V2.0.10: moved selected IC fields outside bodies,
  hid two footprint display fields, oriented 22 labels outward, and fixed
  canonical page/title clipping. All 122 component definitions, 110 named
  nets/classes, pin mappings, wires and electrical anchors are unchanged;
  PCB/project hashes match V2.0.10. PDF regenerated from the canonical source.
  See [readability proof and remaining work](2026-09-14-v2-schematic-readability.md).

- V2.0.10 resolves the two trigger-diode package identities using standard 1N4148W symbols and explicit Diodes Incorporated 1N4148W-7-F manufacturer/MPN/assembly fields. SOD-123 geometry, all 122 placements, all 400 pad records, all 1115 copper objects and all 110 electrical nets are unchanged. Only two diode values/identities and metadata change. The negative-only clamps still do not qualify timer-input protection or laser safety. See [package/polarity review](2026-09-14-v2-diode-package.md).

- V2.0.9 moves the four existing 22 ohm DAC source resistors closer to the translator. Source links drop from 6.783–16.095 mm to 2.796–6.352 mm, all on F.Cu without vias. Only four placements and local source/DAC-side routing change; 118 placements and 1084 retained copper objects are exact. All 122 values/footprints and 110 nets remain unchanged. The native source-layout guard and six negative fixtures pass; 40 MHz timing remains unqualified. See [source-layout evidence](2026-09-14-v2-dac-source-layout.md).

- V2.0.8 moves R26 toward the MCU/DAC boundary, adds two local front-layer return zones and three AGND stitching vias, and preserves all signal routing. Geometric opposite-layer ground overlap on the four DAC input nets improves from 1.8–31.9% to 74.8–90.7%; this does not qualify impedance, timing or EMC.
- User reports a 28 x 57 mm ESP32 body with 22 pins per side. Updated U1's placed/library body and courtyard while preserving all 44 pads. Two bypass capacitors move/rotate clear of the wider module while retaining their VCC pad positions. Their ground routing and two nearby supply-feed segments are adapted. Pin-grid centering, 2.54 mm pitch, 22.86 mm row spacing and antenna/USB clearance remain provisional.
- In V2.0.8, against V2.0.7: all 122 values/footprint assignments and all 110 net memberships are unchanged; 119 placements and 1104 retained copper objects are preserved. Ten obsolete copper objects are replaced by fifteen scoped objects; project rules and the legacy PCB are unchanged. See the [V2.0.8 return/mechanics review](2026-09-14-v2-return-layout.md).

- V2.0.7 relocates the existing C_IN2 10 uF capacitor beside C_INHF1. Its VIN and ground paths to U_BUCK1 are each 6.905 mm of direct F.Cu routing, without vias in those local connections. Six obsolete capacitor stubs/vias are removed and two 0.8 mm local traces added. All values, footprint types and electrical memberships are unchanged; 121 placements are retained.
- The exact-object audit found two coincident +3V3 trace pairs carrying duplicate IDs since V2.0.5's UUID restoration. V2.0.7 removes one identical copy from each pair, without changing the occupied +3V3 copper geometry. All 1112 retained unique baseline copper objects match their original geometry/width/IDs. The new input-layout guard also rejects duplicate copper IDs.

- V2.0.6 corrects C_INHF1 from 100 nF to TI's 220 nF / 50 V X7R requirement, specifies ceramic ratings for C_BOOT1/C_VCC1, and preserves every electrical net. R_FB1/R_FB2 move close to the buck FB pin; total FB trace length drops from 23.655 to 6.550 mm and its two vias are removed. The ground spur is replaced by a local return to the exposed-pad ground; BOOT/VCC traces widen from 0.25 to 0.40 mm. No load/current/thermal or loop-stability rating is established by this change.
- Against V2.0.5: 120 placements, all pad geometry and all pad nets are preserved. Of 1128 old copper items, 1108 are unchanged, 17 obsolete feedback/ground/stub items are removed and three BOOT/VCC tracks are widened. Nine short front-layer tracks replace the removed routing. The only component value change is C_INHF1; no footprint is replaced or added.

- V2.0.5 adds U_DMXLV1 (SN74LVC1G17DBVR), R_DMXIN1 (10 kohm pull-down) and C_DMXLV1 (100 nF bypass). J_DMX1.3 now drives the 5.5 V-tolerant input; the 3V3-powered noninverting output drives GPIO4. The module supply/pinout and firmware are unchanged. A disconnected module holds RX LOW/break; this is not an independent laser-off function.
- Against v2.0.4, exactly those three components and the RO/3V3/power-ground memberships change. All 119 previous footprints/placements and pad geometry are preserved; only J_DMX1.3 changes pad net. Of 1098 baseline copper items, 1097 retain exact geometry (five renamed); one RO diagonal is trimmed by 1.8 mm on each axis. All retained item UUIDs were restored after router import. New copper is confined to the local DMX stage and its supply/ground connections.

- Preserve the approved external 12 V fan rails: J4.1–J5.2 and J6.1–J7.2, separate from each other and the buck positive rails. J2 supplies 12.6 V to the 5 V buck.
- Added U_SCANLV1 (SN74LVC1G17DBVR), R_SCANIN1 (10 kohm input pull-down) and C_SCANLV1 (100 nF local bypass). The timer's 5 V output no longer directly drives GPIO39. The buffer uses MCU +3V3 and power ground, preserving HIGH=OK. Full supply/temperature/fault qualification remains open; see the scan-status review.
- The earlier V2.0.4 change against v2.0.3 added exactly three components and changed only the status/3V3/power-ground memberships. All previous values, footprints and unrelated electrical memberships match. Firmware and shutdown/arming logic are unchanged.
- PCB: 175 x 115 mm closed outline, two copper layers, nominal 1.6 mm thickness, 122 footprints, 1011 trace segments, 104 vias and five filled ground zones (two new zones explicitly named).
- In V2.0.4, all original 116 footprint placements and pad geometries are preserved. Removed one original GPIO39 segment at the MCU end and reassigned the other six status copper items to SCAN_STATUS_5V. All 1066 retained baseline copper items were verified exactly after that rename. Unrelated autorouter normalization was restored from the baseline.
- U1 already specifies two 1x22 female socket strips; corrected the footprint's stale 21/20-pin description. Retained all 44 pads, 2.54 mm pitch and provisional 22.86 mm row spacing. V2.0.8 adds the user-reported 28 x 57 mm body envelope; actual pin-grid alignment, fit and antenna/USB clearance remain unqualified.
- Repositioned seven analog parts for local feedback/supply routing; routed analog feedback, output and bypass connections manually. All 30 pre-autoroute segments survived the routing import exactly, along with every footprint position and pad net.
- Completed remaining routing in an isolated offline candidate, corrected sub-minimum neckdowns, added an R9 escape via and verified the candidate before adopting it.
- Added B.Cu AGND and power-ground planes, plus local F.Cu buck thermal copper. U_BUCK1 exposed-pad/thermal-hole pad 9 uses solid zone connection. Isolated copper removal is enabled. These first planes do not qualify the MCU/DAC return boundary or system EMC.
- `PowerDraft` retains provisional 0.8 mm routing for both fan-positive rails, VIN_RAW, VIN_BUCK and Buck +5V. Default signal routing is 0.25 mm, minimum 0.20 mm; vias are 0.60/0.30 mm. Minima include 0.20 mm copper clearance, 0.50 mm copper-to-edge and 0.10 mm silk clearance. No current rating or copper-weight/temperature qualification is claimed.

## Verification

KiCad 10.0.6, native all-severity DRC with schematic parity and freshly refilled zones:

- ERC: **0 violations**.
- PCB: **0 unconnected items**, **0 non-routing errors**, **1 footprint-type warning**.
- Native schematic-parity section: **0 findings**. Both diode-filter warnings are resolved by explicit matching SOD-123 part identities, not exclusions.
- No reported shorts, clearance/courtyard conflicts, undersized tracks, silk collisions, isolated copper or starved thermals.
- DAC, sensor/tach, fan-power, scan-status, DMX, buck and trigger-diode interface checks pass. The diode checker also verifies only the two approved identity changes against V2.0.9; all electrical net memberships remain identical. Earlier optional baseline comparisons in individual historical reviews apply to their named revisions.
- Input-layout guard proves the two explicit same-layer connections and unique copper IDs; negative checks reject the old duplicate-ID board and a candidate with the local VIN link removed. Its 8 mm/0.60 mm limits are project-local routing guards, not manufacturer electrical ratings.
- Draft-evidence guard passes. Forty-nine unit tests cover ten report/rule methods, eight scan-buffer cases, eleven DMX cases, five buck methods seven diode methods and eight SMD-placement coverage methods (including
  parameterized cases). Resolved diode warnings are no longer allowed. A passing draft guard is not production approval.
- Combined front/back copper and the revised module outline/placements were rendered and inspected. Native inspection confirms five filled zones assigned to the intended ground nets. The read-only DAC overlap diagnostic records geometric improvement, not electrical qualification.
- Current local verification includes export, seven interface checks, 41 unit tests, both native layout guards, ERC and native DRC/parity. Relocated-copy results are recorded in the current revision's review.

Remaining known warning:

1. `U_BUCK1`: KiCad's type heuristic expects through-hole because the SMD footprint contains plated thermal holes. Qualify the exposed-pad, solder/paste, thermal-hole and assembly construction.
The former D_TRIGCL_SCAN1/D_TRIGCL_WD1 package warnings were resolved in V2.0.10. Reappearance is a failed draft gate; trigger-circuit electrical and safety qualification remains open.

Correction to the v2.0.2 report: its committed project actually retained **0.00 mm** minimum silk clearance, despite the documented 0.10 mm intent. Native save/export operations reset this setting. V2.0.3 explicitly stores and verifies 0.10 mm; the new guard rejects the old/reset value and skipped checks. No findings were excluded.

## Assembly export coverage

The native SMD position export contains 97 parts. Do not add
`--exclude-fp-th`: it removes U_BUCK1 because this SMD package contains
plated holes. The new
`hardware/tests/check_smd_placement_coverage.py` compares the actual PCB's
populated SMD inventory with native CSV references, values, packages and
sides. It passes the 97-row export and rejects the real 96-row negative
export for the missing buck; it does not qualify coordinates or rotations.

The buck's 2.60 x 3.10 mm mask/paste opening matches TI's DDA0008J example
for a 0.125 mm stencil; do not change it merely to suppress a type warning.
The actual assembly process and via treatment need confirmation.
See [assembly evidence and reproduction](2026-09-14-v2-assembly-export.md).
The user has been asked to specify reflow service, self-reflow/hot-air, or
primarily soldering-iron assembly. No production assembly files were issued.

## Reproduce

```sh
kicad-cli sch export netlist --format kicadxml -o /tmp/galvos-current.xml \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
PYTHONDONTWRITEBYTECODE=1 python hardware/tests/check_fan_power.py /tmp/galvos-current.xml
PYTHONDONTWRITEBYTECODE=1 python hardware/tests/check_sensor_fan_interfaces.py /tmp/galvos-current.xml
PYTHONDONTWRITEBYTECODE=1 python hardware/tests/check_dac_interface.py /tmp/galvos-current.xml
PYTHONDONTWRITEBYTECODE=1 python hardware/tests/check_scan_status.py /tmp/galvos-current.xml
PYTHONDONTWRITEBYTECODE=1 python hardware/tests/check_dmx_interface.py /tmp/galvos-current.xml
PYTHONDONTWRITEBYTECODE=1 python hardware/tests/check_buck_interface.py /tmp/galvos-current.xml
PYTHONDONTWRITEBYTECODE=1 python hardware/tests/check_trigger_diodes.py /tmp/galvos-current.xml
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

Also run the native layout guards with KiCad's Python bindings:

```sh
/usr/bin/python -B hardware/tests/check_dac_source_layout.py \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_pcb'
/usr/bin/python -B hardware/tests/check_buck_input_layout.py \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_pcb'
```

PCB DRC deliberately collects the remaining warnings. Its process exit code alone is not proof. Read the report and run the draft guard; do not disable package warnings to obtain an apparent clean result.

## Release remains blocked by engineering work

Routing connectivity is complete, not production qualification. Continue actual ESP32/header fit, antenna/USB and enclosure/connector mechanics, complete BOM/assembly review, manufacturing-data correctness and independent review. Remaining safety/interface gates include independent Class 4 shutdown/rearm, E-stop/override behavior, RGB reset-off drive, scan-fail limitations, external interface protection and physical safety qualification. Thermal, load/current-capacity and timing findings remain unverified but are excluded from further work by explicit user request; do not treat that exclusion as evidence of compliance. V1 perfboard operation does not qualify v2 fault safety. No Gerbers have been generated.

Existing circuit gates remain: independent Class 4 shutdown/rearm, E-stop/override behavior, RGB reset-off drive, DMX module/receiver/cable qualification and fan PWM contracts, analog output range, scan-fail limitations and full buffer/timer qualification, buck ratings and input protection. Physical testing is required. V1 perfboard operation does not qualify v2 or fault safety. No Gerbers were generated.

See [V2.0.8 return/mechanics change](2026-09-14-v2-return-layout.md), [shutdown-boundary evidence](2026-09-14-shutdown-boundary.md), [local input-capacitor change](2026-09-14-v2-buck-input.md), [buck feedback/bypass change](2026-09-14-v2-buck-layout.md), [DMX input change](2026-09-14-v2-dmx-input.md), [scan-status change](2026-09-14-v2-scan-status.md), [v2.0.3 routing evidence](2026-09-14-v2-routing.md), [remaining plan](2026-09-14-v2-pcb-plan.md) and `codex-todos.md`.
