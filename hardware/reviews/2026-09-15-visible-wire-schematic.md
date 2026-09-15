# Visible-wire schematic review — 2026-09-15

## Result

The A1 snapshot is a documentation-only copy of GalvOS V2.0.10. All 67
multi-pin signal nets are represented by continuous orthogonal wires. Signal
net labels are hidden and are used only to preserve KiCad net names. Visible
Reference and Value fields are horizontal, standardized above/below their
symbols, and clear of visible wires.

The canonical schematic, canonical PDF, PCB, KiCad project, and firmware were
not modified. The PCB does not require an update because the copied schematic
has exactly the same components and pin-to-net membership as the canonical
schematic; native PCB schematic-parity reports zero issues.

## Evidence

- Snapshot schematic SHA-256:
  `b6a2cf3082400402ab856ecf88aaa09c5aa5256373f284cadedcfd335a43d1d5`
- Snapshot PDF SHA-256:
  `ce7178ca895a54b95507c1e4290379fac4a28d112b430bf3a07408eb43afef7c`
- PDF: one A1 landscape page, `2383.92 x 1683.79 pt`.
- Electrical preservation: PASS; exactly 122 components and 110 nets, with
  identical libraries and pin membership.
- Visible connectivity: PASS; 67 signal nets, 95 wire graphs, 47 junctions,
  and 77 hidden labels. There are no visible signal labels or bus objects.
- Presentation profile: PASS; A1, 1.27 mm grid, orthogonal wires, horizontal
  fields, no wire/Reference/Value collisions, five functional regions, and
  the principal left-to-right chains.
- KiCad ERC at all severities: PASS; 0 violations.
- Hardware unit tests: PASS; 107 tests.
- Interface checks: PASS; fan power, sensors/fans, DAC, scan status, DMX,
  buck, and trigger diodes (7/7).
- PCB DRC/parity: PASS; 0 unconnected items and 0 schematic-parity issues.
  One existing `U_BUCK1` footprint-type warning remains documented and was
  not excluded or downgraded.
- PCB layout guards: PASS; DAC source routes are 2.796–6.352 mm without vias,
  and both buck-input VIN/GND paths are 6.905 mm.
- Immutable SHA-256 lock: PASS for the canonical schematic, canonical PDF,
  PCB, and KiCad project.

## Mirrored KiCad project context

The preservation netlist and ERC evidence were generated with the copied
schematic placed under its canonical basename in a temporary mirror of the
project context:

```sh
proof_dir=/tmp/galvos-visible-wires.ZPyrPg
context="$proof_dir/final-context"
copy_sch='hardware/snapshots/2026-09-15-visible-wires/Laser Controllerv2_only_for_pcb_test.kicad_sch'

cp "$copy_sch" "$context/Laser Controllerv2_only_for_pcb_test.kicad_sch"
cp 'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_pro' "$context/"
cp 'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_prl' "$context/"
cp hardware/schematics/sym-lib-table hardware/schematics/fp-lib-table "$context/"
ln -sfn "$PWD/hardware/schematics/libraries" "$context/libraries"

kicad-cli sch export netlist --format kicadxml \
  -o "$proof_dir/final.xml" \
  "$context/Laser Controllerv2_only_for_pcb_test.kicad_sch"
kicad-cli sch erc --format json --severity-all --exit-code-violations \
  -o "$proof_dir/final-erc.json" \
  "$context/Laser Controllerv2_only_for_pcb_test.kicad_sch"
```

A direct export from the snapshot directory is not used for the electrical
identity claim: without the project basename and library context, KiCad emits
different sheet/library URIs and netclass metadata even when the electrical
pin membership is unchanged.

## Visual inspection at 240 dpi

| Region | Result | Observation |
|---|---|---|
| Power | PASS | Supply chain is separated above the signal sections; rails flow down toward consumers. |
| MCU / storage / sensors / DMX / fans | PASS | Inputs are left, MCU central, fan outputs right; parallel lanes remain distinguishable. |
| DAC / analogue galvo | PASS | DAC, translator, amplifier, and galvo output read left-to-right; fields do not cross wires. |
| RGB | PASS | Three repeated channels are vertically aligned with visible, separate signal paths. |
| E-stop / watchdog / scan-fail / SSR | PASS | Safety-related blocks are grouped along the lower row and their visible branches remain traceable. |

## Scope and non-claims

This copy does not update the PCB or fabrication data. It is not a fabrication
release, safety qualification, laser-operation approval, or production
approval. It does not add or imply a separate SSR restart-interlock design.
