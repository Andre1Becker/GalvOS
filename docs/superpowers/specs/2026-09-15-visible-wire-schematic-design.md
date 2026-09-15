# Visible-Wire Schematic Copy Design

Date: 2026-09-15

## Goal

Create a human-readable A1 landscape copy of the GalvOS V2.0.10 schematic in
which every non-power electrical connection is represented by continuous,
visible, orthogonal wires. Preserve the current canonical schematic and its PDF
unchanged.

## Scope and artifact ownership

The editable output is the copied schematic at:

`hardware/snapshots/2026-09-15-visible-wires/Laser Controllerv2_only_for_pcb_test.kicad_sch`

Its rendered output is:

`hardware/snapshots/2026-09-15-visible-wires/GalvOS_V2.0.10_Schematic.pdf`

The canonical schematic, canonical PDF, PCB, KiCad project, firmware and
manufacturing outputs are outside this change and must remain byte-identical.

## Electrical invariants

- The copied schematic must retain exactly 122 components and 110 nets.
- Component references, values, footprints, custom properties, library data,
  pin numbers and pin-to-net membership must remain identical to the canonical
  baseline.
- No component, pin, net, footprint or library substitution is permitted.
- The change must not implement or imply the separate SSR restart-interlock
  design.
- ERC and DRC severities, exclusions and project rules must not be weakened.
- The output remains an engineering draft, not a fabrication or laser-operation
  approval.

## Page and functional layout

The copy uses one A1 landscape sheet. The five existing functional regions
remain visually distinct:

1. Power input and conversion
2. MCU, storage, sensors, DMX and fans
3. DAC and analogue galvo path
4. RGB laser interface
5. E-stop, watchdog, scan-fail and SSR status

Within every region, signal sources and connectors are placed on the left,
processing components in the centre and outputs on the right. Positive supply
symbols and rails are above their loads; ground and return symbols are below.
Large blank routing corridors separate functional regions and prevent wires
from crossing unrelated symbols or text.

## Visible connectivity contract

- Every non-power net containing two or more component pins must have one
  continuous visible wire graph that touches every pin on that net.
- Signal connectivity must not depend on separated matching `label`,
  `global_label`, `hierarchical_label`, bus or bus-entry objects.
- One hidden local label may remain on a continuous wire graph solely to retain
  its exact locked net name. Global and hierarchical signal labels are replaced
  by hidden local labels, and no electrical signal label may render in the PDF.
  Section headings and ordinary explanatory text remain.
- Standard KiCad power symbols are allowed for power nets because they are the
  approved visible notation for supply and return connectivity.
- Signal wires are orthogonal and remain on the 1.27 mm grid.
- Every intentional branch has an explicit junction. A crossing without a
  junction never represents a connection.
- No diagonal wires, hidden wire segments or bus aggregation are used.
- Long connections use reserved routing lanes at the edge of their functional
  region. Parallel lanes keep at least one grid step of separation.

## Text and symbol presentation

- Visible Reference and Value fields render horizontally.
- Reference fields remain above their symbol and values below unless a custom
  symbol requires a documented collision-free side placement.
- Fields, pin text, wires, power symbols and section headings must not overlap.
- Anonymous generated names such as `Net-(...)` are not rendered as labels or
  explanatory text.

## Implementation structure

A deterministic hardware-layout helper will create the copy from the locked
canonical electrical baseline. It owns only page size, placements, fields,
wire geometry, junctions, power helpers and explanatory text. It must never
derive or alter electrical membership.

A separate visibility checker will consume the copied schematic and the locked
baseline netlist. It will map component pin endpoints to wire graphs and reject:

- any multi-pin signal net without a continuous visible wire path;
- a wire graph that joins pins from different baseline nets;
- a visible signal label, any global/hierarchical signal label or any bus object;
- hidden same-name labels attached to disconnected wire graphs;
- off-grid or diagonal connectivity;
- missing branch junctions;
- non-horizontal visible fields or reversed principal signal flow.

The existing electrical-preservation checker remains the authority for
component/library/property and pin/net identity.

## Verification

Completion requires all of the following on the copied schematic:

- exact electrical-preservation PASS: 122 components and 110 nets;
- visible-connectivity checker PASS for every multi-pin non-power net;
- presentation checker PASS for A1 layout, grid and left-to-right chains;
- all seven hardware interface checks PASS;
- complete hardware unit-test suite PASS;
- native KiCad ERC reports zero violations;
- canonical PCB DRC reports zero unconnected and zero schematic-parity items,
  with only the already documented `U_BUCK1` footprint-type warning;
- canonical PCB and project hashes remain unchanged;
- canonical schematic and canonical PDF hashes remain unchanged;
- PDF export succeeds directly from the copied schematic;
- full-page and detailed 240 dpi inspection confirms readable wires, junctions,
  fields and power flow across all five regions.

## Deliverables and non-claims

Deliverables are the copied A1 `.kicad_sch`, its PDF, the deterministic layout
helper, automated visibility tests and a dated review. No PCB update, Gerber,
assembly output, firmware change, safety qualification or production approval
is part of this work.
