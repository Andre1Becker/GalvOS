# V2.0.10 schematic layout aesthetics — design specification

Date: 2026-09-15

Target: `hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch`

Baseline: `a01e4e1` on `main`

Status: design approved in chat; implementation planning awaits review of this written specification.

## 1. Objective

Refactor the canonical V2.0.10 KiCad schematic into a consistently readable
engineering drawing without changing its electrical design. Signal flow must
read from left to right, power must read from top to bottom, text must not
collide with symbols or conductors, and every electrical anchor must remain on
the standard KiCad schematic grid.

The result remains an engineering draft. Improving its presentation does not
approve fabrication, laser operation, component selection, assembly, thermal
performance, current capacity, timing, EMC or functional safety.

## 2. Preservation boundary

This work is a drawing-only refactor. It must preserve:

- all 122 component references, values, property contents and footprint assignments;
- all symbol units, pin numbers, pin-to-net memberships and no-connect flags;
- all 110 existing electrical net names and net classes;
- the behavior and polarity of every interface, including RGB inversion,
  DAC translation, scan status, watchdog, E-stop and SSR control;
- the current PCB and project configuration byte-for-byte;
- the unimplemented status of the separate SSR-only restart-interlock design.

No label may be added to an unnamed connection if doing so changes the net name
seen by KiCad or the PCB. No existing named net may be renamed for tidiness.
Electrical changes discovered to be desirable during layout are recorded as
separate findings and are not folded into this refactor.

## 3. Chosen structure

The schematic remains a single sheet. A functional-zone layout provides most
of the readability benefit without the net-path and maintenance cost of a new
hierarchy.

The alternatives rejected for this revision are:

1. Text-only cleanup. It has the lowest risk but leaves the long conductors and
   mixed signal directions that currently obscure circuit intent.
2. Hierarchical sheets. They can scale better, but this design does not need
   the added ports, sheet navigation or hierarchical net paths. They also make
   exact PCB-net preservation harder to audit.

Within the sheet, each functional block has one purpose and exposes a small,
visually obvious set of inputs, outputs and power connections.

## 4. Sheet organization and flow

Use a three-column reading order within every signal-processing row:

| Left: source/input | Centre: processing/control | Right: sink/output |
| --- | --- | --- |
| Supply connectors | Regulation, filtering and rail flags | Distributed rails and powered loads |
| SD, DMX and sensor connectors | Level translation and ESP32 | Status/control destinations |
| ESP32 DAC control signals | DAC translator, DAC and analogue stage | Galvo output connector |
| ESP32 RGB signals | Bias networks and optocouplers | Laser-driver connector |
| E-stop and monitored conditions | Watchdog, scan detector and status buffers | SSR and MCU-status interfaces |
| Fan tachometer inputs | ESP32/control signals | Fan power/PWM connectors |

The rows are separated by whitespace and titled consistently. Blocks may be
reordered to achieve the table above, but their electrical contents may not
change.

Inside every block:

- inputs enter at the left edge and outputs leave at the right edge;
- positive supplies enter from above;
- grounds and negative returns leave downward;
- signal conductors are horizontal where practical;
- supply and return conductors are vertical where practical;
- wires use orthogonal segments only, with explicit junctions at connections;
- conductors do not pass through symbol bodies, field text or unrelated blocks.

The title block and the two draft warnings remain unobstructed. The existing
620 x 440 mm custom page may be retained if all spacing rules fit; increasing
the page is allowed only when the implementation proof shows that the current
page cannot meet the spacing rules without compressing a functional block.

## 5. Wire and label policy

Short local wiring remains visible because it communicates circuit topology.
Long connections that cross functional blocks are replaced by paired local
net labels using their exact existing net names. Labels are placed at the
source and destination boundaries and point into their attached conductor.

Global labels are reserved for existing supply and ground domains or signals
that genuinely require global scope. They are not used as a substitute for
ordinary local labels on this single sheet.

No electrical bus is introduced in this revision. DAC SPI and RGB are small,
sparse interfaces; aligned label groups communicate them more clearly than a
bus while avoiding aliases or member-name changes. Each group uses equal
vertical spacing and a stable top-to-bottom order. A future bus proposal is
valid only if it preserves the exact member net names and demonstrably reduces
visual complexity.

The following are treated as wire-salad indicators and must be eliminated:

- a conductor crossing two or more unrelated functional blocks;
- long return paths drawn across the sheet instead of using the correct power
  or ground symbol;
- parallel signals with inconsistent direction, order or spacing;
- multiple direction changes whose only purpose is avoiding text;
- ambiguous four-way intersections or junction markers hidden by fields;
- remote connections whose source and destination cannot be located from the
  visible net name.

## 6. Text and field standard

Visible fields use a single orientation and hierarchy:

- ICs, modules and connectors: reference above the symbol, value below it;
- horizontal two-pin passives: reference above and value below, centred on the
  body;
- vertical two-pin passives: reference to the left and value to the right,
  aligned to the body centre;
- power symbols and graphical helper symbols retain normal KiCad conventions;
- footprint, datasheet, manufacturer and assembly properties remain stored but
  hidden from the drawing unless they are essential assembly instructions.

Reference and value fields remain horizontal. Text must not overlap a symbol
body, pin name, wire, junction, label or other visible field. Repeated networks
use the same offsets. Section headings use consistent size, colour and
left-aligned placement above their block.

The text-placement pass may move and rotate fields but may not move an
electrical anchor. Collision checks use the rendered field bounding boxes with
clear whitespace rather than relying only on source coordinates.

## 7. Grid and geometry standard

Electrical anchors, symbol pins, labels, junctions and wire vertices align to
KiCad's 1.27 mm (50 mil) schematic grid. Functional block origins and repeated
network spacing align to 2.54 mm multiples where practical. Field offsets use
1.27 mm increments unless a symbol's geometry requires a larger multiple.

Moving a component includes all of its attached local topology. A wire must
not be stretched from its previous position merely to keep an old route. The
implementation proceeds one functional block at a time so every intermediate
candidate remains exportable and electrically comparable.

## 8. Implementation and verification strategy

Before moving anything, export a baseline KiCad XML netlist and record the
canonical schematic, PCB and project hashes. Then refactor one functional block
at a time in an isolated candidate, exporting and comparing after each block.

The final proof requires:

1. Exact equality of the baseline and candidate XML `components`, `libparts`,
   `libraries` and `nets` subtrees, excluding only design-file metadata.
2. Exact equality of component reference/value/footprint properties, pin-to-net
   memberships, named nets and net classes.
3. Byte-identical canonical PCB and project files.
4. KiCad ERC with zero violations.
5. Native PCB DRC with zero unconnected items and zero schematic-parity issues;
   the existing documented `U_BUCK1` footprint-type warning may remain.
6. All seven hardware interface checkers and all 49 current hardware unit tests
   passing, plus both native layout guards and the draft-evidence guard.
7. A fresh PDF export inspected at full-sheet scale and in block-level crops.
8. No text/symbol/wire collisions in the final rendered drawing, except any
   unavoidable KiCad-generated pin text that is explicitly listed and reviewed.
9. A before/after review identifying every long wire replaced by labels and
   confirming that every replacement retained the original net name.

Failed or incomplete candidate checks are not counted as passes. No rule,
severity or known-warning allowlist may be weakened to make the refactor pass.

## 9. Deliverables and exclusions

The implementation updates the canonical `.kicad_sch`, regenerates the
schematic PDF in `hardware/`, and records a dated readability/preservation
review under `hardware/reviews/`. The PCB, project, firmware, 3D views and
manufacturing data remain unchanged.

This task does not implement the SSR-only restart interlock, alter firmware,
select new components, generate V2 Gerbers or close any existing fabrication,
assembly, mechanical or laser-safety gate.

## 10. Acceptance outcome

The task is complete only when the schematic can be read consistently from
left to right and top to bottom, the rendered collision review is clean, and
all electrical-preservation checks pass. If strict layout and exact electrical
identity conflict, electrical identity wins and the remaining presentation
issue is reported rather than hidden by a functional change.
