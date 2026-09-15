# V2.0.10 Schematic Layout Aesthetics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> `superpowers:subagent-driven-development` (recommended) or
> `superpowers:executing-plans` to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reflow the canonical V2.0.10 KiCad schematic into a grid-aligned,
collision-free single-sheet drawing with left-to-right signal flow and
top-to-bottom power flow while preserving its exact electrical design.

**Architecture:** Keep the one-sheet electrical model and reorganize it into
five horizontal functional rows with input, processing and output columns.
Use short local wires inside blocks and exact-name local labels between blocks.
Bracket every drawing edit with an XML-netlist preservation checker and a
source-level presentation checker; KiCad ERC, PCB parity and the existing
hardware gates remain the final authority.

**Tech Stack:** KiCad 10.0.6 CLI, KiCad `.kicad_sch` S-expressions, Python 3
standard library, `unittest`, existing GalvOS hardware checkers.

**Spec:**
`docs/superpowers/specs/2026-09-15-schematic-layout-aesthetics-design.md`

## Global Constraints

- Target only `hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch` for electrical drawing edits.
- Preserve all 122 component references, values, property contents and footprint assignments.
- Preserve every symbol unit, pin number, no-connect flag, pin-to-net membership, all 110 net names and all net classes.
- Keep `hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_pcb` and `.kicad_pro` byte-identical.
- Do not implement or imply the separate SSR-only restart-interlock design.
- Do not weaken ERC/DRC severities, project rules or the known-warning allowlist.
- Introduce no electrical bus; keep DAC SPI and RGB as aligned exact-name label groups.
- Keep the user-owned untracked `agents.md` untouched and out of every commit.
- Keep the 620 x 440 mm page unless a recorded failed layout candidate proves it cannot satisfy the spacing rules.
- Electrical identity wins over presentation if the two conflict.

---

### Task 1: Add the exact electrical-preservation gate

**Files:**

- Create: `hardware/tests/check_schematic_preservation.py`
- Create: `hardware/tests/test_schematic_preservation.py`

**Interfaces:**

- Consumes: two KiCad XML netlists exported from the baseline and candidate schematics.
- Produces: `check(before: Path, after: Path) -> None`, which raises `ValueError` on any difference in `components`, `libparts`, `libraries` or `nets`.
- Produces: CLI `python -B hardware/tests/check_schematic_preservation.py BEFORE.xml AFTER.xml` with a concise PASS line on equality.

- [ ] **Step 1: Write focused negative and positive tests**

Create minimal XML fixtures in `tempfile.TemporaryDirectory()` and exercise
the public `check()` function:

```python
class SchematicPreservationTests(unittest.TestCase):
    def test_accepts_design_metadata_only_change(self):
        before = self.write_netlist(source="before.kicad_sch")
        after = self.write_netlist(source="after.kicad_sch")
        check(before, after)

    def test_rejects_component_property_change(self):
        before = self.write_netlist()
        after = self.write_netlist(value="22k")
        with self.assertRaisesRegex(ValueError, "components"):
            check(before, after)

    def test_rejects_pin_membership_change(self):
        before = self.write_netlist(net_nodes=(("U1", "1"),))
        after = self.write_netlist(net_nodes=(("U1", "2"),))
        with self.assertRaisesRegex(ValueError, "nets"):
            check(before, after)
```

- [ ] **Step 2: Run the focused test and confirm the missing module fails**

Run:

```bash
PYTHONDONTWRITEBYTECODE=1 python -m unittest hardware/tests/test_schematic_preservation.py -v
```

Expected: FAIL because `check_schematic_preservation` does not exist.

- [ ] **Step 3: Implement canonical XML-subtree comparison**

Use only `pathlib` and `xml.etree.ElementTree`. Ignore the top-level `design`
subtree and compare serialized copies of these four children in their original
order:

```python
SECTIONS = ("components", "libparts", "libraries", "nets")

def snapshot(path: Path) -> dict[str, bytes]:
    root = ET.parse(path).getroot()
    return {
        name: ET.tostring(root.find(f"./{name}"), encoding="utf-8")
        for name in SECTIONS
    }

def check(before: Path, after: Path) -> None:
    old = snapshot(before)
    new = snapshot(after)
    changed = [name for name in SECTIONS if old[name] != new[name]]
    if changed:
        raise ValueError(f"Electrical netlist changed: {', '.join(changed)}")
```

Reject missing sections with a named `ValueError`; do not silently serialize
`None`.

- [ ] **Step 4: Run the focused and complete hardware unit-test suites**

Run:

```bash
PYTHONDONTWRITEBYTECODE=1 python -m unittest hardware/tests/test_schematic_preservation.py -v
PYTHONDONTWRITEBYTECODE=1 python -m unittest discover -s hardware/tests -p 'test_*.py'
```

Expected: preservation tests pass; the complete count increases from 49 by the
number of new methods and all tests pass.

- [ ] **Step 5: Commit the preservation gate**

```bash
git add hardware/tests/check_schematic_preservation.py hardware/tests/test_schematic_preservation.py
git commit -m "test(hw): guard schematic electrical identity"
```

---

### Task 2: Add the source-level presentation contract

**Files:**

- Create: `hardware/tests/check_schematic_presentation.py`
- Create: `hardware/tests/test_schematic_presentation.py`

**Interfaces:**

- Consumes: the canonical `.kicad_sch` source and an optional `--block` name.
- Produces: `parse_root_objects(text: str) -> list[SExprObject]` without a third-party parser.
- Produces: `check_grid()`, `check_fields()` and `check_block()` validation functions.
- Produces: CLI block selectors `power`, `io`, `analog`, `laser`, `safety` and `all`.

- [ ] **Step 1: Write parser, grid, field and ordering tests**

Use tiny S-expression fixtures that include quoted parentheses and UUIDs so
the parser cannot rely on a naive regular expression:

```python
def test_root_parser_ignores_parentheses_inside_strings(self):
    objects = parse_root_objects('(kicad_sch (text "A (B)") (wire (pts (xy 1.27 2.54))))')
    self.assertEqual([obj.kind for obj in objects], ["text", "wire"])

def test_rejects_off_grid_wire_vertex(self):
    with self.assertRaisesRegex(ValueError, "1.27 mm grid"):
        check_grid(self.parse('(wire (pts (xy 1.30 2.54) (xy 5.08 2.54)))'))

def test_rejects_vertical_reference_field(self):
    symbol = self.symbol(reference_at=(10.16, 7.62, 90))
    with self.assertRaisesRegex(ValueError, "Reference.*horizontal"):
        check_fields([symbol])

def test_rejects_reversed_signal_chain(self):
    placements = {"J_DMX1": (100.0, 100.0), "U_DMXLV1": (80.0, 100.0), "U1": (120.0, 100.0)}
    with self.assertRaisesRegex(ValueError, "DMX"):
        check_chain("DMX", ("J_DMX1", "U_DMXLV1", "U1"), placements)
```

- [ ] **Step 2: Run the focused test and confirm the missing checker fails**

```bash
PYTHONDONTWRITEBYTECODE=1 python -m unittest hardware/tests/test_schematic_presentation.py -v
```

Expected: FAIL because `check_schematic_presentation` does not exist.

- [ ] **Step 3: Implement a minimal lossless reader and presentation rules**

The reader only identifies balanced root objects and extracts their first
placement, wire vertices and `Reference`/`Value` properties. It never writes
the schematic. Use `decimal.Decimal` and require electrical coordinates to be
integral multiples of `Decimal("1.27")` within `Decimal("0.001")`.

Define the reader's value type and validation interfaces explicitly:

```python
@dataclass(frozen=True)
class SExprObject:
    kind: str
    source: str
```

Implement `parse_root_objects(text: str) -> list[SExprObject>` as a balanced
scanner that tracks string and escape state. Implement
`check_grid(objects: list[SExprObject]) -> None` by extracting top-level
symbol/label/junction/no-connect anchors and wire vertices. Implement
`check_fields(objects: list[SExprObject]) -> None` from placed-symbol
properties, and `check_block(name: str, objects: list[SExprObject]) -> None`
from the fixed row/column and chain tables below. Every checker accumulates all
violations, then raises one `ValueError` containing the affected UUID or
reference and rule name; an empty violation list returns normally.

Encode the approved principal signal chains:

```python
CHAINS = {
    "DMX": ("J_DMX1", "U_DMXLV1", "U1"),
    "DAC": ("U1", "U_DACLV1", "U2", "U12", "U13"),
    "LASER_RED": ("U1", "U15", "U4"),
    "LASER_GREEN": ("U1", "U16", "U5"),
    "LASER_BLUE": ("U1", "U17", "U3"),
    "BUCK": ("J2", "U_BUCK1", "L1"),
}
```

Use the following page zones, all on 1.27 mm boundaries:

```python
COLUMNS = {
    "input": (Decimal("25.40"), Decimal("139.70")),
    "process": (Decimal("165.10"), Decimal("406.40")),
    "output": (Decimal("431.80"), Decimal("571.50")),
}
ROWS = {
    "power": (Decimal("25.40"), Decimal("88.90")),
    "io": (Decimal("101.60"), Decimal("165.10")),
    "analog": (Decimal("177.80"), Decimal("241.30")),
    "laser": (Decimal("254.00"), Decimal("304.80")),
    "safety": (Decimal("317.50"), Decimal("381.00")),
}
BLOCK_PLACEMENT = {
    "power": {
        "input": ("J2", "J3", "J4", "J6"),
        "process": ("D1", "D2", "FB1", "U_BUCK1"),
        "output": ("L1",),
    },
    "io": {
        "input": ("J1", "J_DMX1", "U6", "U7", "U8", "U9", "U10"),
        "process": ("U_DMXLV1", "U1"),
        "output": ("J5", "J7"),
    },
    "analog": {
        "process": ("U_DACLV1", "U2", "U12"),
        "output": ("U13",),
    },
    "laser": {
        "process": ("U15", "U16", "U17"),
        "output": ("U4", "U5", "U3"),
    },
    "safety": {
        "input": ("J_ESTOP1",),
        "process": ("U_WD1", "U_SCAN1", "U_SCANLV1"),
        "output": ("J_SSR1",),
    },
}
```

The checker verifies principal-symbol containment, left-to-right chains,
orthogonal wires, grid conformity and horizontal visible Reference/Value
fields. Power-symbol and helper-symbol fields follow KiCad defaults and are
excluded by library identifier, not by arbitrary reference allowlists.

- [ ] **Step 4: Run unit tests and record the current drawing as the negative control**

```bash
PYTHONDONTWRITEBYTECODE=1 python -m unittest hardware/tests/test_schematic_presentation.py -v
python -B hardware/tests/check_schematic_presentation.py --block all \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
```

Expected: unit tests pass; the real V2.0.10 drawing fails with off-layout
principal symbols and inconsistent visible fields. Preserve that output in the
task notes; a failing current drawing is the required red state.

- [ ] **Step 5: Commit the presentation checker**

```bash
git add hardware/tests/check_schematic_presentation.py hardware/tests/test_schematic_presentation.py
git commit -m "test(hw): define schematic presentation contract"
```

---

### Task 3: Establish the baseline proof and reflow the power row

**Files:**

- Modify: `hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch`

**Interfaces:**

- Consumes: Task 1 XML comparator and Task 2 `--block power` contract.
- Produces: the supply-input, buck-converter, rail-flag and fan-power blocks in the top row.

- [ ] **Step 1: Export an immutable baseline from commit `d4cf341`**

```bash
proof_dir='.pio/galvos-sch-layout-proof'
baseline_dir="$proof_dir/baseline"
mkdir -p "$baseline_dir"
git archive d4cf341 hardware/schematics | tar -x -C "$baseline_dir"
kicad-cli sch export netlist --format kicadxml -o "$proof_dir/before.xml" \
  "$baseline_dir/hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch"
sha256sum \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_pcb' \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_pro' \
  > "$proof_dir/locked-source.sha256"
```

Expected: baseline exports with 122 components and 110 nets.

- [ ] **Step 2: Confirm the power-row contract fails before editing**

```bash
python -B hardware/tests/check_schematic_presentation.py --block power \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
```

Expected: FAIL because the current supply/buck elements are dispersed and
their visible fields are inconsistent.

- [ ] **Step 3: Reflow only power-row symbols and their local topology**

Place J2/J3 and approved fan supply inputs at the left, D1/D2/FB1 and input
filtering next, U_BUCK1 with its feedback/bypass network in the process column,
then L1/output filtering and powered-load rails toward the right. Within each
sub-block place positive rails above parts and ground/negative returns below.
Keep all existing net names. Replace only cross-block conductors with paired
local labels; retain short feedback and bypass wiring visibly.

- [ ] **Step 4: Prove the block presentation and electrical identity**

```bash
proof_dir='.pio/galvos-sch-layout-proof'
kicad-cli sch export netlist --format kicadxml -o "$proof_dir/after-power.xml" \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
python -B hardware/tests/check_schematic_preservation.py \
  "$proof_dir/before.xml" "$proof_dir/after-power.xml"
python -B hardware/tests/check_schematic_presentation.py --block power \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
kicad-cli sch erc --format json --severity-all --exit-code-violations \
  -o "$proof_dir/erc-power.json" \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
```

Expected: preservation PASS, power presentation PASS, ERC zero violations.

- [ ] **Step 5: Commit the power row**

```bash
git add 'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
git commit -m "style(hw): reflow schematic power path"
```

---

### Task 4: Reflow external inputs, MCU and fan interfaces

**Files:**

- Modify: `hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch`

**Interfaces:**

- Consumes: immutable `before.xml`; approved input/process/output columns.
- Produces: the I/O row containing SD, DMX, sensors, U1 and fan connectors.

- [ ] **Step 1: Confirm the I/O-row contract fails**

```bash
python -B hardware/tests/check_schematic_presentation.py --block io \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
```

Expected: FAIL on current positions and field placement.

- [ ] **Step 2: Reflow the input side and MCU**

Place J1, J_DMX1, U6–U10 and tach inputs in aligned groups on the left. Place
U_DMXLV1 and its bias/bypass parts immediately to the right of J_DMX1. Place
U1 centrally with visible pin groups unobstructed. Use aligned exact-name local
labels for SD, DMX, sensor, fan-tach and fan-PWM signals that cross block
boundaries. Do not join or rename SPI2 and SPI3.

- [ ] **Step 3: Reflow fan connectors as bidirectional boundary blocks**

Place J5/J7 on the right with +12 V entering from above and ground below.
Place PWM labels on their left-facing control path and tach labels on a
separate leftward return line. Keep J4/J6 as the independent 12 V fan supply
inputs; do not merge their positive rails.

- [ ] **Step 4: Prove I/O presentation, topology and electrical identity**

```bash
proof_dir='.pio/galvos-sch-layout-proof'
kicad-cli sch export netlist --format kicadxml -o "$proof_dir/after-io.xml" \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
python -B hardware/tests/check_schematic_preservation.py \
  "$proof_dir/before.xml" "$proof_dir/after-io.xml"
python -B hardware/tests/check_schematic_presentation.py --block io \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
python -B hardware/tests/check_fan_power.py "$proof_dir/after-io.xml"
python -B hardware/tests/check_sensor_fan_interfaces.py "$proof_dir/after-io.xml"
python -B hardware/tests/check_dmx_interface.py "$proof_dir/after-io.xml"
```

Expected: all six commands exit zero and the two fan-positive rails remain
separate.

- [ ] **Step 5: Commit the I/O row**

```bash
git add 'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
git commit -m "style(hw): reflow schematic controller interfaces"
```

---

### Task 5: Reflow the DAC and analogue-output row

**Files:**

- Modify: `hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch`

**Interfaces:**

- Consumes: DAC control labels from U1 and immutable baseline netlist.
- Produces: U_DACLV1 → U2 → U12 → U13 left-to-right signal chain.

- [ ] **Step 1: Confirm the analogue-row contract fails**

```bash
python -B hardware/tests/check_schematic_presentation.py --block analog \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
```

Expected: FAIL on the dispersed or reversed current chain.

- [ ] **Step 2: Reflow translated DAC control and conversion**

Place the four 3.3 V DAC controls as a vertically ordered label group feeding
U_DACLV1 from the left. Place the translated 5 V group in the same top-to-bottom
order feeding U2 on the right. Keep CLR, SYNC, SCLK and DIN exact names and
retain each source/termination resistor visibly beside its owner.

- [ ] **Step 3: Reflow analogue processing and output**

Place U2, X/Y reconstruction networks, U12 and U13 sequentially from left to
right. Put +5V_ANA/+15 V decoupling above the associated IC and AGND/-15 V
returns below. Keep R26's single AGND-to-power-ground bridge explicit and do
not introduce another ground connection.

- [ ] **Step 4: Prove analogue presentation and electrical identity**

```bash
proof_dir='.pio/galvos-sch-layout-proof'
kicad-cli sch export netlist --format kicadxml -o "$proof_dir/after-analog.xml" \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
python -B hardware/tests/check_schematic_preservation.py \
  "$proof_dir/before.xml" "$proof_dir/after-analog.xml"
python -B hardware/tests/check_schematic_presentation.py --block analog \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
python -B hardware/tests/check_dac_interface.py "$proof_dir/after-analog.xml"
```

Expected: presentation, preservation and DAC checks pass.

- [ ] **Step 5: Commit the analogue row**

```bash
git add 'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
git commit -m "style(hw): reflow schematic analogue path"
```

---

### Task 6: Reflow the RGB laser-output row

**Files:**

- Modify: `hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch`

**Interfaces:**

- Consumes: exact RGB signal labels from U1.
- Produces: three parallel U1 → bias/6N137 → U3/U4/U5 channels.

- [ ] **Step 1: Confirm the laser-row contract fails**

```bash
python -B hardware/tests/check_schematic_presentation.py --block laser \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
```

Expected: FAIL on current ordering, field placement or crossing conductors.

- [ ] **Step 2: Lay out three identical channel lanes**

Use top-to-bottom channel order Red, Green, Blue consistently at the U1 label
group, bias resistors, U15/U16/U17 optocouplers and U4/U5/U3 outputs. Within
each lane, input is left, the optocoupler is central and the output is right;
+3V3/5 V bias enters above and ground returns below. Preserve active-HIGH
driver and inverted GPIO semantics exactly.

- [ ] **Step 3: Keep channel-local components local**

Place R1/R2/R4, R18–R23, C5–C7 and R27–R29 beside their channel owner. Replace
only the long U1-to-channel conductors with paired exact-name local labels.
Keep the existing draft note that reset-OFF behavior is not yet verified.

- [ ] **Step 4: Prove laser-row presentation and full electrical identity**

```bash
proof_dir='.pio/galvos-sch-layout-proof'
kicad-cli sch export netlist --format kicadxml -o "$proof_dir/after-laser.xml" \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
python -B hardware/tests/check_schematic_preservation.py \
  "$proof_dir/before.xml" "$proof_dir/after-laser.xml"
python -B hardware/tests/check_schematic_presentation.py --block laser \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
```

Expected: both checkers pass; XML proves RGB polarity/connectivity is unchanged.

- [ ] **Step 5: Commit the laser row**

```bash
git add 'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
git commit -m "style(hw): reflow schematic laser outputs"
```

---

### Task 7: Reflow the watchdog, scan and external-safety row

**Files:**

- Modify: `hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch`

**Interfaces:**

- Consumes: existing GPIO14/GPIO38/GPIO39/GPIO47 meanings and exact baseline netlist.
- Produces: current E-stop/watchdog/scan/SSR circuitry presented left-to-right without implying the future SSR architecture exists.

- [ ] **Step 1: Confirm the safety-row contract fails**

```bash
python -B hardware/tests/check_schematic_presentation.py --block safety \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
```

Expected: FAIL on current dispersion and field layout.

- [ ] **Step 2: Reflow watchdog and scan detector as separate sub-blocks**

Place timing/trigger inputs left of U_WD1 and U_SCAN1, their local R/C/clamp
networks immediately around each timer, and status/drive outputs to the right.
Place +5 V and decoupling above, power ground below. Keep the scan-command
status note explicit: it is not mirror-motion feedback.

- [ ] **Step 3: Reflow external E-stop and SSR interfaces without redesigning them**

Place J_ESTOP1 at the left boundary, GPIO47 status toward U1, GPIO38/watchdog
control in the processing column and J_SSR1 at the right boundary. Keep the
existing NE555-to-330-ohm SSR drive and current connector meanings unchanged.
Add no restart latch, transistor driver, contact supply or new SSR part.

- [ ] **Step 4: Prove safety-row presentation and electrical identity**

```bash
proof_dir='.pio/galvos-sch-layout-proof'
kicad-cli sch export netlist --format kicadxml -o "$proof_dir/after-safety.xml" \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
python -B hardware/tests/check_schematic_preservation.py \
  "$proof_dir/before.xml" "$proof_dir/after-safety.xml"
python -B hardware/tests/check_schematic_presentation.py --block safety \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
python -B hardware/tests/check_scan_status.py "$proof_dir/after-safety.xml"
python -B hardware/tests/check_trigger_diodes.py "$proof_dir/after-safety.xml"
```

Expected: all checks pass and the future SSR-only design remains absent.

- [ ] **Step 5: Commit the safety row**

```bash
git add 'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
git commit -m "style(hw): reflow schematic safety interfaces"
```

---

### Task 8: Normalize all fields and finish the visual collision pass

**Files:**

- Modify: `hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch`

**Interfaces:**

- Consumes: all five reflowed rows.
- Produces: standardized visible Reference/Value fields and a full-sheet presentation PASS.

- [ ] **Step 1: Run the all-block checker as the red test**

```bash
python -B hardware/tests/check_schematic_presentation.py --block all \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
```

Expected: FAIL with the remaining field/grid inconsistencies; save the exact
references reported instead of moving unrelated electrical anchors.

- [ ] **Step 2: Apply the field standard mechanically**

For ICs/modules/connectors place Reference above and Value below. For
horizontal passives place Reference above and Value below, centred; for
vertical passives place Reference left and Value right. Keep all visible
fields horizontal. Hide stored Footprint/Datasheet/Manufacturer/Assembly
fields unless the text is an essential assembly instruction.

- [ ] **Step 3: Export SVG and PDF and remove rendered collisions**

```bash
visual_dir=$(mktemp -d /tmp/galvos-sch-visual.XXXXXX)
kicad-cli sch export svg -o "$visual_dir" \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
kicad-cli sch export pdf -o "$visual_dir/GalvOS_V2.0.10_Schematic.pdf" \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
pdftoppm -png -r 192 "$visual_dir/GalvOS_V2.0.10_Schematic.pdf" \
  "$visual_dir/schematic"
```

Inspect the complete page and crops for all five rows. Move only graphical
fields or complete local topology to clear every reference/value collision,
wire-through-text case, hidden junction and ambiguous crossing. Record any
unavoidable KiCad-generated pin-text issue by reference and pin; do not call it
clean without that explicit record.

- [ ] **Step 4: Re-run full presentation and preservation gates**

```bash
proof_dir='.pio/galvos-sch-layout-proof'
kicad-cli sch export netlist --format kicadxml -o "$proof_dir/after-fields.xml" \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
python -B hardware/tests/check_schematic_preservation.py \
  "$proof_dir/before.xml" "$proof_dir/after-fields.xml"
python -B hardware/tests/check_schematic_presentation.py --block all \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
kicad-cli sch erc --format json --severity-all --exit-code-violations \
  -o "$proof_dir/erc-final-layout.json" \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
```

Expected: preservation PASS, presentation PASS and ERC zero violations.

- [ ] **Step 5: Commit the normalized drawing**

```bash
git add 'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
git commit -m "style(hw): normalize schematic fields"
```

---

### Task 9: Regenerate evidence and run the complete release-draft proof

**Files:**

- Modify: `hardware/GalvOS_V2.0.10_Schematic.pdf`
- Create: `hardware/reviews/2026-09-15-v2-schematic-layout-aesthetics.md`
- Modify: `hardware/reviews/CURRENT-HARDWARE.md`

**Interfaces:**

- Consumes: final canonical schematic and every checker from Tasks 1–8.
- Produces: committed PDF, dated before/after evidence and an updated current-hardware checkpoint.

- [ ] **Step 1: Export the final canonical artifacts**

```bash
proof_dir='.pio/galvos-sch-layout-proof'
kicad-cli sch export netlist --format kicadxml -o "$proof_dir/final.xml" \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
kicad-cli sch export pdf -o 'hardware/GalvOS_V2.0.10_Schematic.pdf' \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
```

Expected: both exports succeed from the canonical source without a temporary
formatting copy.

- [ ] **Step 2: Run all schematic and interface gates**

```bash
proof_dir='.pio/galvos-sch-layout-proof'
python -B hardware/tests/check_schematic_preservation.py \
  "$proof_dir/before.xml" "$proof_dir/final.xml"
python -B hardware/tests/check_schematic_presentation.py --block all \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
python -B hardware/tests/check_fan_power.py "$proof_dir/final.xml"
python -B hardware/tests/check_sensor_fan_interfaces.py "$proof_dir/final.xml"
python -B hardware/tests/check_dac_interface.py "$proof_dir/final.xml"
python -B hardware/tests/check_scan_status.py "$proof_dir/final.xml"
python -B hardware/tests/check_dmx_interface.py "$proof_dir/final.xml"
python -B hardware/tests/check_buck_interface.py "$proof_dir/final.xml"
python -B hardware/tests/check_trigger_diodes.py "$proof_dir/final.xml"
PYTHONDONTWRITEBYTECODE=1 python -m unittest discover -s hardware/tests -p 'test_*.py'
```

Expected: exact preservation and presentation pass; all seven interfaces and
the expanded unit-test suite pass.

- [ ] **Step 3: Run native ERC, DRC/parity and PCB guards**

```bash
proof_dir='.pio/galvos-sch-layout-proof'
kicad-cli sch erc --format json --severity-all --exit-code-violations \
  -o "$proof_dir/final-erc.json" \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
kicad-cli pcb drc --refill-zones --schematic-parity --all-track-errors \
  --format json --severity-all -o "$proof_dir/final-drc.json" \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_pcb'
python -B hardware/tests/check_pcb_draft.py "$proof_dir/final-drc.json" \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_pro'
/usr/bin/python -B hardware/tests/check_dac_source_layout.py \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_pcb'
/usr/bin/python -B hardware/tests/check_buck_input_layout.py \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_pcb'
sha256sum -c "$proof_dir/locked-source.sha256"
```

Expected: ERC zero; DRC zero unconnected and zero parity findings; only the
documented U_BUCK1 type warning; draft and both layout guards pass; PCB and
project hashes remain unchanged.

- [ ] **Step 4: Write the dated review and update the checkpoint**

The dated review must include:

- baseline and final schematic/PDF hashes;
- 122-component/110-net identity result;
- a table of each long-wire group replaced by exact-name local labels;
- the final grid, field and signal-chain checker results;
- the visual inspection outcome for all five rows;
- ERC/DRC/interface/unit-test counts;
- the unchanged U_BUCK1 warning and every remaining release blocker;
- an explicit statement that the SSR-only restart interlock is not implemented.

Update `CURRENT-HARDWARE.md` to link the review and describe this as a
drawing-only revision after V2.0.10, not a new electrical release.

- [ ] **Step 5: Check the final diff scope and documentation**

```bash
git diff --check
git status --short
git diff --stat d4cf341
git diff --name-only d4cf341
```

Expected changed product/evidence files: canonical schematic, regenerated PDF,
the two new checker/test pairs, this implementation plan, dated review and
current checkpoint. The untracked review is visible in `git status` until Step
6 stages it. PCB, project, firmware, 3D views and `agents.md` must not appear in
the diff or staging area; the pre-existing untracked `agents.md` may remain in
the status output.

- [ ] **Step 6: Commit the evidence checkpoint**

```bash
git add \
  'hardware/GalvOS_V2.0.10_Schematic.pdf' \
  hardware/reviews/2026-09-15-v2-schematic-layout-aesthetics.md \
  hardware/reviews/CURRENT-HARDWARE.md
git commit -m "docs(hw): verify schematic layout refactor"
```

- [ ] **Step 7: Repeat the final proof from committed HEAD**

Repeat Steps 2, 3 and 5 without changing files. Report actual command exit
codes and counts. Do not claim completion if any command fails, if the visual
inspection has an unrecorded collision, or if Git contains an unintended file.
