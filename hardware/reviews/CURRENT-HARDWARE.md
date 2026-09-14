# Current hardware checkpoint

Version/tag: **hw-v2.0.1-draft** (2026-09-14).
**NOT FOR FABRICATION — NOT QUALIFIED FOR LASER OPERATION.**
This Git checkpoint records both tracked design artifacts; it does not
claim that the historical PCB implements the revised schematic.

| Artifact | State at this checkpoint | SHA-256 |
| --- | --- | --- |
| `hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch` | Revised v2 schematic, 116 components / 104 nets | `329d9fe71007a3bce5b9ab1d8c6d179d24eb731b78fcd39fc53aaadfcb788028` |
| `hardware/schematics/Laser Controller.kicad_pcb` | Unchanged historical PCB; does not match v2 | `9703ba1bf5343e8a77ab9dbb0781785384c4f238bedc52c1341898d9e9bbf6b2` |

## Approved corrections included

- U6..U10 now use `Connector_Generic:Conn_01x03`, value `DS18B20 connector`,
  and descriptions specifying pin 1 GND, pin 2 DATA, pin 3 +3V3. The misleading
  sensor-package datasheet field was cleared. All five original references,
  pin numbers, JST footprints, net memberships and pin UUIDs are retained.
- Short wire extensions accommodate native connector geometry. Connector
  values sit below the bus wires; the sensor region was rendered and inspected.
- FAN1_TACH now connects J5.3/R30 to GPIO2 (U1.J2_5); FAN2_TACH connects
  J7.3/R31 to GPIO9 (U1.J1_15), matching firmware and repository instructions.
  Only the two MCU-side labels changed destination. PWM, pull-ups and external
  connector pinouts remain unchanged. The affected labels were made legible.
- The title block identifies V2.0.1 as a draft. Firmware is unchanged.

## Verification and reproduction

The new checker failed on the original export for both the sensor identity
and tach assignment, then passed on the corrected export. Its optional
baseline comparison allows exactly five value changes and the two MCU-pin
swaps, while preserving all component references/footprints, other symbol
identities and all other net memberships. Negative checks reject old sensor
symbols, reversed sensor power pins, swapped tachs and changed PWM wiring.
The DAC checker remains unchanged and verifies its complete translated paths.
All-severity ERC reports zero violations. Export, ERC and both checkers also
passed from an archive of the staged project in a separate temporary directory,
including the sensor/fan preservation comparison against the original export.

```sh
kicad-cli sch export netlist --format kicadxml -o /tmp/galvos-current.xml \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
PYTHONDONTWRITEBYTECODE=1 python hardware/tests/check_sensor_fan_interfaces.py \
  /tmp/galvos-current.xml
python hardware/tests/check_dac_interface.py /tmp/galvos-current.xml
kicad-cli sch erc --format json --severity-all --exit-code-violations \
  -o /tmp/galvos-current-erc.json \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
```

For preservation proof, export the schematic from parent commit `5ce5d68`
in a separate checkout with its project libraries, and pass that XML as the
second argument to `check_sensor_fan_interfaces.py`. Do not pass the older
pre-translator baseline to this checker. The DAC checker's optional baseline
mode is specifically for the earlier translator change; run it without that
old baseline on this version, whose approved sensor values/tach nets differ.

## Unresolved release gates

No new PCB layout or Gerbers were generated. Existing legacy DRC failures
and schematic/PCB mismatch remain; this version is not an assembly package.
Still required: supply/load and external module contracts, independent Class 4
laser shutdown architecture, electrical-level and analog-path corrections,
mechanical constraints, a matched layout, full BOM/DRC/assembly review and
physical qualification. See `codex-todos.md` and the dated reviews for evidence.
