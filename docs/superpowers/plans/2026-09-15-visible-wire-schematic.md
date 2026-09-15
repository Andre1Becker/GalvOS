# Visible-Wire Schematic Copy Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> `superpowers:subagent-driven-development` (recommended) or
> `superpowers:executing-plans` to implement this plan task-by-task. Before
> execution, use `superpowers:using-git-worktrees` and work in an isolated
> worktree. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce a human-readable, one-page A1 copy of the GalvOS V2.0.10
schematic in which every multi-pin signal net is shown by one continuous,
orthogonal wire graph and no net label is visible.

**Architecture:** Keep the canonical schematic and all canonical hardware
artifacts immutable. A deterministic Python layout helper reads the canonical
KiCad schematic, applies a fixed A1 placement/routing manifest to a copy, and
emits only drawing-level changes. A separate baseline-aware wire-graph checker
proves visible connectivity, while the existing XML preservation checker and
hardware interface gates remain the electrical authorities.

**Tech Stack:** KiCad CLI 10.0.6, KiCad `.kicad_sch` S-expressions, Python 3
standard library, `unittest`, Poppler `pdfinfo`/`pdftoppm`, existing GalvOS
hardware checkers.

**Spec:**
`docs/superpowers/specs/2026-09-15-visible-wire-schematic-design.md`

## Global Constraints

- Editable output is only
  `hardware/snapshots/2026-09-15-visible-wires/Laser Controllerv2_only_for_pcb_test.kicad_sch`.
- Rendered output is only
  `hardware/snapshots/2026-09-15-visible-wires/GalvOS_V2.0.10_Schematic.pdf`.
- Keep the canonical schematic, canonical PDF, PCB, KiCad project, firmware,
  Gerbers, and manufacturing outputs byte-identical.
- Preserve exactly 122 components and 110 nets, including every reference,
  value, footprint, custom property, library record, symbol unit, pin number,
  no-connect flag, net name, and pin-to-net membership.
- Keep the drawing on one A1 landscape sheet. Use the 1.27 mm grid for every
  symbol anchor, label anchor, wire vertex, junction, and section heading.
- Lay out signal flow left to right and positive supply to ground top to bottom.
- Show all multi-pin non-power connections as continuous orthogonal wires. Do
  not use a bus or separated labels as a substitute for visible connectivity.
- No `label`, `global_label`, or `hierarchical_label` text may render. A named
  root-sheet signal net may have exactly one hidden local label on its connected
  graph; anonymous `Net-(...)` graphs have no label and retain their KiCad name
  through their exact pin membership.
- Put junction dots only at real branches and put one at every real branch.
  Crossing wires without a junction remain electrically separate.
- Show Reference and Value horizontally, with Reference above and Value below
  unless a collision-free side placement is required by a rotated custom
  symbol. Do not render anonymous `Net-(...)` names as explanatory text.
- Keep five visually separated regions: power; MCU/storage/sensors/DMX/fans;
  DAC/analogue galvo; RGB laser; E-stop/watchdog/scan-fail/SSR status.
- Do not add or imply the separate SSR restart-interlock design. This remains
  an engineering drawing copy, not fabrication, safety, or laser-operation
  approval.
- Do not weaken KiCad ERC/DRC rules, severities, exclusions, or the existing
  documented `U_BUCK1` footprint-type warning.
- Preserve the untracked user-owned `agents.md`; never stage or modify it.

## File Structure

- `hardware/tools/kicad_schematic_geometry.py`: lossless top-level
  S-expression access, symbol-pin coordinate resolution, and orthogonal graph
  primitives shared by generator and checker.
- `hardware/tools/create_visible_wire_schematic.py`: fixed A1 regions,
  placement transforms, routing policies, deterministic UUIDs, and CLI that
  writes the copied schematic.
- `hardware/tests/test_kicad_schematic_geometry.py`: focused parser,
  transform, graph, crossing, and junction tests.
- `hardware/tests/check_visible_connectivity.py`: baseline XML plus schematic
  checker for continuous visible signal graphs and forbidden labels/buses.
- `hardware/tests/test_visible_connectivity.py`: positive and negative
  connectivity-contract tests.
- `hardware/tests/test_create_visible_wire_schematic.py`: deterministic,
  output-scope, A1, label-hiding, and manifest-completeness tests.
- `hardware/tests/check_schematic_presentation.py`: extend the existing checker
  with the `visible-copy` A1 profile; preserve the canonical default profile.
- `hardware/tests/test_schematic_presentation.py`: regression tests for both
  profiles and collision/flow rules.
- `hardware/reviews/2026-09-15-visible-wire-schematic.md`: final hashes,
  electrical checks, rendering inspection, and non-claims.

---

### Task 1: Lock the canonical baseline and establish the copied artifact

**Files:**

- Create:
  `hardware/snapshots/2026-09-15-visible-wires/Laser Controllerv2_only_for_pcb_test.kicad_sch`
- Create:
  `hardware/snapshots/2026-09-15-visible-wires/GalvOS_V2.0.10_Schematic.pdf`
- Do not modify any canonical hardware file.

**Interfaces:**

- Consumes canonical schematic/PDF at commit `9403bd6`.
- Produces initial byte-identical copied artifacts and an untracked proof
  directory whose hashes bracket every later task.

- [ ] **Step 1: Create the proof directory and lock all immutable hashes**

```bash
proof_dir=$(mktemp -d /tmp/galvos-visible-wires.XXXXXX)
printf '%s\n' "$proof_dir" > /tmp/galvos-visible-wires-proof-path
sha256sum \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch' \
  'hardware/GalvOS_V2.0.10_Schematic.pdf' \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_pcb' \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_pro' \
  > "$proof_dir/immutable.sha256"
kicad-cli sch export netlist --format kicadxml \
  -o "$proof_dir/canonical.xml" \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
```

Expected: all commands exit zero. The four hashes are respectively
`4f36655a3db6b5c36ef65a4722ed1f9eb7d4c4fcdeb77973966a9bf6b506f086`,
`c4b863705803ee55f5cea0628ce073e603d91b3b57d3fccb38cf3b6bce4ad9d6`,
`b8111d778abc5b57ec3adb1bbc94060598fb87cc0d16e584e53e28c0bce4f47a`,
and `dc77f4155067018d81c509e667ad67642031a5cf69abe7181144c19c11bd3b1b`.

- [ ] **Step 2: Prove the baseline counts before copying**

```bash
python - "$(cat /tmp/galvos-visible-wires-proof-path)/canonical.xml" <<'PY'
import sys
import xml.etree.ElementTree as ET

root = ET.parse(sys.argv[1]).getroot()
components = root.findall("./components/comp")
nets = root.findall("./nets/net")
assert len(components) == 122, len(components)
assert len(nets) == 110, len(nets)
print("PASS: canonical baseline has 122 components and 110 nets")
PY
```

Expected: the exact PASS line above.

- [ ] **Step 3: Create the target directory from canonical files**

```bash
install -d hardware/snapshots/2026-09-15-visible-wires
cp --preserve=mode,timestamps \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch' \
  'hardware/snapshots/2026-09-15-visible-wires/Laser Controllerv2_only_for_pcb_test.kicad_sch'
cp --preserve=mode,timestamps \
  'hardware/GalvOS_V2.0.10_Schematic.pdf' \
  'hardware/snapshots/2026-09-15-visible-wires/GalvOS_V2.0.10_Schematic.pdf'
cmp \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch' \
  'hardware/snapshots/2026-09-15-visible-wires/Laser Controllerv2_only_for_pcb_test.kicad_sch'
cmp \
  'hardware/GalvOS_V2.0.10_Schematic.pdf' \
  'hardware/snapshots/2026-09-15-visible-wires/GalvOS_V2.0.10_Schematic.pdf'
```

Expected: both comparisons exit zero. Do not copy or stage the pre-existing
untracked `2026-09-15-label-layout` directory.

- [ ] **Step 4: Commit only the initial copy**

```bash
git add hardware/snapshots/2026-09-15-visible-wires
git diff --cached --name-only
git commit -m "docs(hw): snapshot visible-wire schematic baseline"
```

Expected: exactly the two new snapshot paths are staged; `agents.md` and the
canonical files are absent.

---

### Task 2: Build lossless schematic geometry primitives

**Files:**

- Create: `hardware/tools/kicad_schematic_geometry.py`
- Create: `hardware/tests/test_kicad_schematic_geometry.py`

**Interfaces:**

- Produces `Point = tuple[Decimal, Decimal]` and `PinRef = tuple[str, str]`.
- Produces `parse_document(text: str) -> Document`, where `Document` retains
  every untouched byte and exposes direct root objects with source spans.
- Produces `pin_endpoints(document: Document) -> dict[PinRef, Point]`, resolving
  placed-symbol translation, rotation, mirror, body style, and unit against
  the embedded `lib_symbols` records.
- Produces `wire_graph(document: Document) -> WireGraph`, where an orthogonal
  crossing connects only when a junction exists at the crossing.
- Produces `transform_point(local: Point, at: Point, angle: Decimal,
  mirror: str | None) -> Point` and `on_grid(point: Point) -> bool`.

- [ ] **Step 1: Write parser, transform, and graph tests**

```python
class GeometryTests(unittest.TestCase):
    def test_round_trip_preserves_untouched_source_bytes(self):
        text = '(kicad_sch (text "A (B)") (wire (pts (xy 1.27 2.54) (xy 5.08 2.54))))'
        self.assertEqual(parse_document(text).render(), text)

    def test_pin_transform_handles_rotation_and_mirror(self):
        self.assertEqual(
            transform_point((D("2.54"), D("0")), (D("10.16"), D("20.32")), D("90"), "x"),
            (D("10.16"), D("17.78")),
        )

    def test_crossing_without_junction_stays_disconnected(self):
        graph = graph_for(
            wires=[(("0", "2.54"), ("5.08", "2.54")),
                   (("2.54", "0"), ("2.54", "5.08"))],
            junctions=[],
        )
        self.assertNotEqual(graph.component((D("0"), D("2.54"))),
                            graph.component((D("2.54"), D("0"))))

    def test_t_branch_with_junction_is_one_component(self):
        graph = graph_for(
            wires=[(("0", "2.54"), ("5.08", "2.54")),
                   (("2.54", "2.54"), ("2.54", "5.08"))],
            junctions=[("2.54", "2.54")],
        )
        self.assertEqual(graph.degree((D("2.54"), D("2.54"))), 3)
```

Add fixture symbols with two units and mirrored/rotated pins so pin lookup is
tested independently from the production schematic.

- [ ] **Step 2: Run the focused tests and confirm red state**

```bash
PYTHONDONTWRITEBYTECODE=1 python -m unittest \
  hardware/tests/test_kicad_schematic_geometry.py -v
```

Expected: FAIL because `hardware.tools.kicad_schematic_geometry` is absent.

- [ ] **Step 3: Implement exact-span parsing and pin transforms**

Use a balanced-parenthesis scanner that tracks quotes and escapes. Retain each
direct root object's `start`, `end`, `kind`, and original source. Parse only the
fields required for geometry; render by splicing replacements into the
original string in descending source-offset order.

```python
GRID = Decimal("1.27")
GRID_TOLERANCE = Decimal("0.001")

@dataclass(frozen=True)
class RootObject:
    kind: str
    start: int
    end: int
    source: str

@dataclass
class Document:
    source: str
    objects: list[RootObject]
    replacements: dict[tuple[int, int], str]

    def replace(self, obj: RootObject, source: str) -> None:
        self.replacements[(obj.start, obj.end)] = source

    def remove(self, obj: RootObject) -> None:
        self.replace(obj, "")

    def append(self, source: str) -> None:
        root_end = self.source.rfind(")")
        prior = self.replacements.get((root_end, root_end), "")
        self.replacements[(root_end, root_end)] = prior + "\n  " + source

    def render(self) -> str:
        spans = sorted(self.replacements)
        for left, right in zip(spans, spans[1:]):
            if left[1] > right[0]:
                raise ValueError(f"overlapping replacements: {left}, {right}")
        rendered = self.source
        for (start, end), replacement in sorted(
            self.replacements.items(), reverse=True
        ):
            rendered = rendered[:start] + replacement + rendered[end:]
        return rendered
```

The implementations must contain real bodies: `replace` records an exact span,
`remove` records an empty replacement, `append` inserts before the root closing
parenthesis, and `render` rejects overlapping edits before splicing.

- [ ] **Step 4: Implement topology with explicit crossing semantics**

Split collinear segments at wire endpoints, pin endpoints, and junction
coordinates. At an X crossing with no junction, retain separate horizontal and
vertical graph nodes by using `(point, axis)` internally. Merge axes only for a
junction or a common segment endpoint. Reject zero-length and diagonal
segments and expose graph component IDs, degrees, and touched pins.

- [ ] **Step 5: Run focused and existing presentation tests**

```bash
PYTHONDONTWRITEBYTECODE=1 python -m unittest \
  hardware/tests/test_kicad_schematic_geometry.py \
  hardware/tests/test_schematic_presentation.py -v
```

Expected: all tests pass and the existing presentation parser behavior remains
unchanged.

- [ ] **Step 6: Commit the geometry layer**

```bash
git add hardware/tools/kicad_schematic_geometry.py \
  hardware/tests/test_kicad_schematic_geometry.py
git commit -m "test(hw): add schematic wire geometry model"
```

---

### Task 3: Add the baseline-aware visible-connectivity checker

**Files:**

- Create: `hardware/tests/check_visible_connectivity.py`
- Create: `hardware/tests/test_visible_connectivity.py`

**Interfaces:**

- Consumes `BASELINE.xml` and copied `.kicad_sch`.
- Produces `read_baseline(path: Path) -> dict[str, frozenset[PinRef]]`.
- Produces `check_visible_connectivity(baseline: Path,
  schematic: Path) -> ConnectivityReport`; raises one aggregated `ValueError`
  with net names, pins, and coordinates on contract failure.
- Produces CLI:
  `python -B hardware/tests/check_visible_connectivity.py BASELINE.xml COPY.kicad_sch`.
- `ConnectivityReport` carries checked net count, connected graph count,
  junction count, and hidden naming-label count for the dated review.
- Test helper `check_fixture(*, nets, pins, wires, labels=(), junctions=(),
  extra_objects=())` writes a minimal XML baseline and matching two-symbol
  KiCad source into `TemporaryDirectory`, calls the public checker, and returns
  its report. Tuple coordinates are converted to `Decimal` by the helper.
- Produces `normalize_local_label(text: str) -> str`, which maps root-sheet
  local label `SIG` to baseline XML name `/SIG`; it never rewrites anonymous or
  power-net names.

- [ ] **Step 1: Write contract tests for labels, buses, and net identity**

```python
class VisibleConnectivityTests(unittest.TestCase):
    def test_accepts_one_hidden_label_on_one_continuous_graph(self):
        report = self.check_fixture(
            nets={"/SIG": {("J1", "1"), ("U1", "2")}},
            pins=two_pin_positions(),
            wires=[(("10.16", "10.16"), ("20.32", "10.16"))],
            labels=[hidden_label("SIG", "15.24", "10.16")],
        )
        self.assertEqual(report.checked_signal_nets, 1)

    def test_rejects_matching_hidden_labels_on_disconnected_graphs(self):
        with self.assertRaisesRegex(ValueError, "/SIG.*disconnected labels"):
            self.check_fixture(
                nets={"/SIG": {("J1", "1"), ("U1", "2")}},
                pins={("J1", "1"): ("10.16", "10.16"),
                      ("U1", "2"): ("30.48", "10.16")},
                wires=[(("10.16", "10.16"), ("15.24", "10.16")),
                       (("25.40", "10.16"), ("30.48", "10.16"))],
                labels=[hidden_label("SIG", "15.24", "10.16"),
                        hidden_label("SIG", "25.40", "10.16")],
            )

    def test_rejects_visible_local_global_and_hierarchical_labels(self):
        for kind in ("label", "global_label", "hierarchical_label"):
            with self.subTest(kind=kind), self.assertRaisesRegex(ValueError, "visible label"):
                self.check_fixture(
                    nets={"/SIG": {("J1", "1"), ("U1", "2")}},
                    pins=two_pin_positions(), wires=straight_sig_wire(),
                    labels=[visible_label(kind, "SIG", "15.24", "10.16")],
                )

    def test_rejects_hidden_global_or_hierarchical_signal_label(self):
        for kind in ("global_label", "hierarchical_label"):
            with self.subTest(kind=kind), self.assertRaisesRegex(ValueError, "forbidden signal label"):
                self.check_fixture(
                    nets={"/SIG": {("J1", "1"), ("U1", "2")}},
                    pins=two_pin_positions(), wires=straight_sig_wire(),
                    labels=[hidden_label("SIG", "15.24", "10.16", kind=kind)],
                )

    def test_rejects_bus_and_bus_entry_objects(self):
        with self.assertRaisesRegex(ValueError, "bus objects are forbidden"):
            self.check_fixture(
                nets={"/SIG": {("J1", "1"), ("U1", "2")}},
                pins=two_pin_positions(), wires=straight_sig_wire(),
                extra_objects=[bus_object(), bus_entry_object()],
            )

    def test_rejects_graph_that_joins_two_baseline_nets(self):
        with self.assertRaisesRegex(ValueError, "/A.*joined.*\/B"):
            self.check_fixture(
                nets={"/A": {("J1", "1"), ("U1", "1")},
                      "/B": {("J2", "1"), ("U1", "2")}},
                pins=four_pin_short_positions(), wires=foreign_short_wires(),
            )
```

- [ ] **Step 2: Write graph-continuity, grid, and junction tests**

```python
    def test_rejects_multi_pin_signal_split_into_two_graphs(self):
        with self.assertRaisesRegex(ValueError, "/SIG.*not continuous"):
            self.check_fixture(
                nets={"/SIG": {("J1", "1"), ("U1", "2"), ("U2", "3")}},
                pins=split_three_pin_positions(), wires=split_three_pin_wires(),
            )

    def test_crossing_without_junction_does_not_join_nets(self):
        self.check_fixture(
            nets={"/H": {("J1", "1"), ("U1", "1")},
                  "/V": {("J2", "1"), ("U2", "1")}},
            pins=crossing_pin_positions(), wires=crossing_wires(), junctions=[],
        )

    def test_rejects_branch_without_junction(self):
        with self.assertRaisesRegex(ValueError, "/SIG.*missing junction"):
            self.check_fixture(
                nets={"/SIG": {("J1", "1"), ("U1", "2"), ("U2", "3")}},
                pins=t_branch_pin_positions(), wires=t_branch_wires(),
                junctions=[],
            )

    def test_rejects_junction_on_unbranched_wire(self):
        with self.assertRaisesRegex(ValueError, "spurious junction"):
            self.check_fixture(
                nets={"/SIG": {("J1", "1"), ("U1", "2")}},
                pins=two_pin_positions(), wires=straight_sig_wire(),
                junctions=[("15.24", "10.16")],
            )

    def test_rejects_diagonal_and_off_grid_segments(self):
        with self.assertRaisesRegex(ValueError, "orthogonal.*1.27 mm grid"):
            self.check_fixture(
                nets={"/SIG": {("J1", "1"), ("U1", "2")}},
                pins=two_pin_positions(),
                wires=[(("10.16", "10.16"), ("20.30", "11.43"))],
            )
```

Define the small helper constructors named in the snippets in the test module:
`two_pin_positions()` returns endpoints `(10.16, 10.16)` and
`(20.32, 10.16)`; `straight_sig_wire()` joins them; the crossing helpers create
one horizontal and one vertical two-pin net intersecting at `(15.24, 15.24)`;
the T helpers create endpoints `(10.16, 10.16)`, `(20.32, 10.16)`, and
`(15.24, 15.24)` with the branch point `(15.24, 10.16)`. The split helpers
leave the third endpoint on a separate segment. `foreign_short_wires()` joins
four endpoints placed at x=`10.16`, `15.24`, `20.32`, and `25.40`, all at
y=`10.16`, through one horizontal spine. `hidden_label()`,
`visible_label()`, `bus_object()`, and `bus_entry_object()` return complete
minimal S-expression objects with deterministic fixture UUIDs.
`graph_for(wires, junctions)` serializes those primitives into a minimal
`kicad_sch` root, parses it with `parse_document()`, and calls `wire_graph()`.

- [ ] **Step 3: Run the tests and confirm red state**

```bash
PYTHONDONTWRITEBYTECODE=1 python -m unittest \
  hardware/tests/test_visible_connectivity.py -v
```

Expected: FAIL because the checker module is absent.

- [ ] **Step 4: Implement the checker against exact baseline membership**

Use this exact power-net allowlist only for the continuity exemption; visible
labels and bus objects remain forbidden for every net:

```python
POWER_NET_NAMES = frozenset({
    "+3V3", "+15V", "-15V", "/+5V_ANA", "/+5V_MCU",
    "/FAN1_12V", "/FAN2_12V", "/VCC_BUCK", "/VIN_BUCK",
    "5V GND Buck", "AGND (from Galvo Board)", "Buck +5V", "VIN_RAW",
})
```

Reject every `global_label` and `hierarchical_label` object on a signal net,
even when its effects are hidden. For every baseline net with at least two
component pins and not in this set,
require all endpoint coordinates in one wire component. Require that component
to contain no pin belonging to another baseline net. A hidden local naming
label may occur once on that component; repeated hidden labels must resolve to
the same graph and are rejected even when they do. Treat `unconnected-(...)`
nets as single-pin no-connects, never as route candidates.

- [ ] **Step 5: Run the new tests and demonstrate the canonical copy is red**

```bash
PYTHONDONTWRITEBYTECODE=1 python -m unittest \
  hardware/tests/test_visible_connectivity.py -v
proof_dir=$(cat /tmp/galvos-visible-wires-proof-path)
python -B hardware/tests/check_visible_connectivity.py \
  "$proof_dir/canonical.xml" \
  'hardware/snapshots/2026-09-15-visible-wires/Laser Controllerv2_only_for_pcb_test.kicad_sch'
```

Expected: unit tests pass; the copied label-based drawing fails with visible
labels and disconnected multi-pin signal graphs.

- [ ] **Step 6: Commit the checker**

```bash
git add hardware/tests/check_visible_connectivity.py \
  hardware/tests/test_visible_connectivity.py
git commit -m "test(hw): enforce visible schematic connectivity"
```

---

### Task 4: Build the deterministic A1 layout generator shell

**Files:**

- Create: `hardware/tools/create_visible_wire_schematic.py`
- Create: `hardware/tests/test_create_visible_wire_schematic.py`

**Interfaces:**

- Consumes `SOURCE.kicad_sch`, `BASELINE.xml`, and `OUTPUT.kicad_sch`.
- Produces CLI:
  `python -B hardware/tools/create_visible_wire_schematic.py --source SOURCE --baseline BASELINE.xml --output OUTPUT`.
- Produces `build(source: Path, baseline: Path, output: Path) -> BuildReport`.
- Produces `LayoutBuilder.place_symbol()`, `add_wire()`, `add_junction()`,
  `add_hidden_label()`, `add_section_text()`, and `render()`.
- Produces `NetIndex`, containing exact baseline net-to-pin and pin-to-net
  dictionaries, and `BuildReport(routed_by_region: dict[str, set[str]],
  unrouted_multi_pin_signal_nets: set[str], laser_channel_order:
  list[tuple[str, str, str, str]])`. Its `left_to_right(*refs)` method compares
  final symbol-centre x coordinates.
- Generates stable UUIDs with UUIDv5 from
  `"galvos-visible-wire:{kind}:{logical-key}"`; two runs are byte-identical.
- Test helpers `build_fixture(name)`, `build_fixture_pair()`, and
  `build_production_copy()` call `build()` with checked-in miniature fixture
  strings or the canonical input plus temporary output. `electrical_payload()`
  returns the exact `lib_symbols` and placed-symbol property/pin children.
  `hidden_label_names()` and `visible_label_names()` inspect label effects.

- [ ] **Step 1: Write determinism, scope, page, and manifest tests**

```python
class VisibleWireGeneratorTests(unittest.TestCase):
    def test_two_builds_are_byte_identical(self):
        first = self.build_fixture("first.kicad_sch")
        second = self.build_fixture("second.kicad_sch")
        self.assertEqual(first.read_bytes(), second.read_bytes())

    def test_sets_one_a1_landscape_sheet(self):
        output = self.build_fixture("a1.kicad_sch")
        self.assertIn('(paper "A1")', output.read_text())

    def test_preserves_library_and_component_payloads(self):
        source, output = self.build_fixture_pair()
        self.assertEqual(electrical_payload(source), electrical_payload(output))

    def test_rejects_missing_or_extra_placement_references(self):
        with self.assertRaisesRegex(ValueError, "placement manifest.*U2"):
            build_with_manifest_without("U2")

    def test_hides_one_local_naming_label_per_routed_net(self):
        output = self.build_fixture("labels.kicad_sch")
        self.assertEqual(hidden_label_names(output), {"/SIG"})
        self.assertEqual(visible_label_names(output), set())
```

- [ ] **Step 2: Run focused tests and confirm red state**

```bash
PYTHONDONTWRITEBYTECODE=1 python -m unittest \
  hardware/tests/test_create_visible_wire_schematic.py -v
```

Expected: FAIL because the generator module is absent.

- [ ] **Step 3: Implement the fixed A1 region and column model**

```python
REGIONS = {
    "power":  Rect(D("25.40"), D("25.40"), D("815.34"), D("114.30")),
    "io":     Rect(D("25.40"), D("127.00"), D("815.34"), D("228.60")),
    "analog": Rect(D("25.40"), D("241.30"), D("815.34"), D("342.90")),
    "laser":  Rect(D("25.40"), D("355.60"), D("815.34"), D("444.50")),
    "safety": Rect(D("25.40"), D("457.20"), D("815.34"), D("568.96")),
}
COLUMNS = {
    "input": Rect(D("25.40"), D("0"), D("190.50"), D("594.00")),
    "process": Rect(D("215.90"), D("0"), D("622.30"), D("594.00")),
    "output": Rect(D("647.70"), D("0"), D("815.34"), D("594.00")),
}
```

Reserve x=`203.20` and x=`635.00` as vertical inter-region routing corridors.
Reserve the upper 12.70 mm and lower 12.70 mm of every region for supply and
return rails. All calculated positions pass through `snap_grid()`.

- [ ] **Step 4: Implement lossless drawing replacement and full manifest guards**

Remove only top-level `wire`, `junction`, `label`, `global_label`,
`hierarchical_label`, `bus`, `bus_entry`, and section `text` objects from the
copy. Preserve `lib_symbols`, every placed component symbol, power-helper
symbol, no-connect, title block content, and all non-drawing properties.
Replace `(paper "User" 620 440)` with `(paper "A1")`.

During Tasks 4–8, preserve not-yet-routed net membership with a development
fallback: attach a short grid-aligned stub and a hidden local label to every pin
of an unrouted named `/<name>` net; retain and hide the canonical global naming
objects for unrouted anonymous nets because a local label would rename them.
The CLI accepts
`--allow-incomplete` only while this fallback is present and reports the exact
unrouted set. Task 9 removes the fallback and option after the set reaches
zero. Move every `no_connect` anchor with its corresponding single-pin
unconnected endpoint. Associate each `#PWR` helper with its canonical touched
pin and move it by the same placement delta until the final rail pass.

Build the placement manifest from the canonical placements through a fixed
piecewise grid transform, then apply explicit principal-component overrides.
The manifest guard compares its keys with all non-helper references and
requires exactly 122 keys. Use these anchors:

```python
PRINCIPAL = {
    "power":  {"J2": ("input", "upper"), "U_BUCK1": ("process", "middle")},
    "io":     {"J_DMX1": ("input", "upper"), "U_DMXLV1": ("process", "upper"),
               "U1": ("process", "middle")},
    "analog": {"U_DACLV1": ("input", "middle"), "U2": ("process", "left"),
               "U12": ("process", "middle"), "U13": ("output", "middle")},
    "laser":  {"U15": ("process", "upper"), "U16": ("process", "middle"),
               "U17": ("process", "lower"), "U4": ("output", "upper"),
               "U5": ("output", "middle"), "U3": ("output", "lower")},
    "safety": {"J_ESTOP1": ("input", "upper"), "U_WD1": ("process", "upper"),
               "U_SCAN1": ("process", "middle"), "U_SCANLV1": ("process", "lower"),
               "J_SSR1": ("output", "middle")},
}
ANCHORS = {
    "input": {"left": D("50.80"), "middle": D("101.60"), "right": D("165.10")},
    "process": {"left": D("266.70"), "middle": D("419.10"), "right": D("571.50")},
    "output": {"left": D("673.10"), "middle": D("723.90"), "right": D("787.40")},
}
ROW_Y = {
    "power": {"upper": D("50.80"), "middle": D("69.85"), "lower": D("95.25")},
    "io": {"upper": D("152.40"), "middle": D("177.80"), "lower": D("215.90")},
    "analog": {"upper": D("266.70"), "middle": D("292.10"), "lower": D("330.20")},
    "laser": {"upper": D("374.65"), "middle": D("400.05"), "lower": D("425.45")},
    "safety": {"upper": D("482.60"), "middle": D("520.70"), "lower": D("558.80")},
}
```

Assign each passive to the same region as the principal device sharing the
largest number of its non-power nets; resolve a tie by the canonical y-region,
then reference lexicographically. Reject any unresolved reference instead of
placing it silently.

Every `route_<region>()` function consumes every signal net assigned to that
region, not merely the required subsets named in later tests. Assign a net to
the region containing the majority of its non-passive pins; break ties by the
region of the leftmost non-passive pin, then by the order `power`, `io`,
`analog`, `laser`, `safety`. The build report must show every baseline
multi-pin non-power net in exactly one region or in the temporary fallback set.

- [ ] **Step 5: Make fixture tests green and commit the generator shell**

```bash
PYTHONDONTWRITEBYTECODE=1 python -m unittest \
  hardware/tests/test_create_visible_wire_schematic.py -v
git add hardware/tools/create_visible_wire_schematic.py \
  hardware/tests/test_create_visible_wire_schematic.py
git commit -m "feat(hw): scaffold deterministic A1 schematic copy"
```

Expected: all focused tests pass; the production copy is not regenerated until
the region routing tables are added in Tasks 5–9.

---

### Task 5: Route the power-input and conversion region

**Files:**

- Modify: `hardware/tools/create_visible_wire_schematic.py`
- Modify: `hardware/tests/test_create_visible_wire_schematic.py`
- Modify: copied `.kicad_sch` target.

**Interfaces:**

- Adds `route_power(builder: LayoutBuilder, nets: NetIndex) -> None`.
- Produces a top-to-bottom J2/protection/buck/output path with local feedback,
  switch, enable, boot, and filtering networks.

- [ ] **Step 1: Add a failing region-route test**

```python
def test_power_region_routes_converter_control_nets(self):
    report = self.build_production_copy()
    self.assertLessEqual(
        {"/BOOT_BUCK", "/EN_BUCK", "/FB_BUCK", "/SW_BUCK"},
        report.routed_by_region["power"],
    )
```

Run:

```bash
PYTHONDONTWRITEBYTECODE=1 python -m unittest \
  hardware/tests/test_create_visible_wire_schematic.py \
  -k power_region -v
```

Expected: FAIL because `route_power` has not populated the route report.

- [ ] **Step 2: Implement power-region placement and routing**

Place input connectors and protection on the left, U_BUCK1 and its close
passives centrally, and L1/output filtering on the right. Use upper rails for
`VIN_RAW`, `/VIN_BUCK`, `/VCC_BUCK`, and `Buck +5V`; use the lower rail for
`5V GND Buck`. Keep `/BOOT_BUCK` and `/SW_BUCK` compact beside U_BUCK1. Put one
explicit junction at each `/FB_BUCK`, `/EN_BUCK`, and supply-rail branch.

- [ ] **Step 3: Generate the production copy and run region gates**

```bash
proof_dir=$(cat /tmp/galvos-visible-wires-proof-path)
python -B hardware/tools/create_visible_wire_schematic.py \
  --allow-incomplete \
  --source 'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch' \
  --baseline "$proof_dir/canonical.xml" \
  --output 'hardware/snapshots/2026-09-15-visible-wires/Laser Controllerv2_only_for_pcb_test.kicad_sch'
kicad-cli sch export netlist --format kicadxml \
  -o "$proof_dir/after-power.xml" \
  'hardware/snapshots/2026-09-15-visible-wires/Laser Controllerv2_only_for_pcb_test.kicad_sch'
python -B hardware/tests/check_schematic_preservation.py \
  "$proof_dir/canonical.xml" "$proof_dir/after-power.xml"
python -B hardware/tests/check_buck_interface.py "$proof_dir/after-power.xml"
```

Expected: preservation and buck-interface checks pass. The full visible checker
still reports unrouted regions, which is the required intermediate red state.

- [ ] **Step 4: Commit the power route**

```bash
git add hardware/tools/create_visible_wire_schematic.py \
  hardware/tests/test_create_visible_wire_schematic.py \
  'hardware/snapshots/2026-09-15-visible-wires/Laser Controllerv2_only_for_pcb_test.kicad_sch'
git commit -m "style(hw): wire visible schematic power region"
```

---

### Task 6: Route MCU, storage, sensors, DMX, and fans

**Files:** same three files as Task 5.

**Interfaces:**

- Adds `route_io(builder: LayoutBuilder, nets: NetIndex) -> None`.
- Routes SD SPI3, DMX receive/translation, DS18B20, fan tach/PWM, and MCU
  signals without mixing SPI2 and SPI3.

- [ ] **Step 1: Add failing exact-net coverage test**

```python
def test_io_region_routes_named_interfaces(self):
    report = self.build_production_copy()
    required = {
        "/SD_CS", "/SD_MISO", "/SD_MOSI", "/SD_SCK", "/DMX_RO_5V",
        "/GPIO4", "/FAN1_PWM", "/FAN1_TACH", "/FAN2_PWM", "/FAN2_TACH",
        "Net-(U1-GPIO18)",
    }
    self.assertLessEqual(required, report.routed_by_region["io"])
```

Run the focused test and expect FAIL because the I/O route set is empty.

- [ ] **Step 2: Implement I/O lanes**

Place external connectors at the left/right region boundaries according to
direction, U1 centrally, and translators beside their destination pins. Route
SD and DMX in separate parallel lane groups. Route fan tach inputs left to U1
and PWM outputs U1 to right. Keep `/FAN1_12V` and `/FAN2_12V` separate upper
rails and route returns downward. Use x=`203.20` for signals that leave U1 for
lower functional regions; assign those vertical lanes by sorted exact net name
at 1.27 mm spacing.

- [ ] **Step 3: Regenerate and prove I/O interfaces**

```bash
proof_dir=$(cat /tmp/galvos-visible-wires-proof-path)
python -B hardware/tools/create_visible_wire_schematic.py \
  --allow-incomplete \
  --source 'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch' \
  --baseline "$proof_dir/canonical.xml" \
  --output 'hardware/snapshots/2026-09-15-visible-wires/Laser Controllerv2_only_for_pcb_test.kicad_sch'
kicad-cli sch export netlist --format kicadxml -o "$proof_dir/after-io.xml" \
  'hardware/snapshots/2026-09-15-visible-wires/Laser Controllerv2_only_for_pcb_test.kicad_sch'
python -B hardware/tests/check_schematic_preservation.py \
  "$proof_dir/canonical.xml" "$proof_dir/after-io.xml"
python -B hardware/tests/check_fan_power.py "$proof_dir/after-io.xml"
python -B hardware/tests/check_sensor_fan_interfaces.py "$proof_dir/after-io.xml"
python -B hardware/tests/check_dmx_interface.py "$proof_dir/after-io.xml"
```

Expected: all five commands exit zero.

- [ ] **Step 4: Commit the I/O route**

```bash
git add hardware/tools/create_visible_wire_schematic.py \
  hardware/tests/test_create_visible_wire_schematic.py \
  'hardware/snapshots/2026-09-15-visible-wires/Laser Controllerv2_only_for_pcb_test.kicad_sch'
git commit -m "style(hw): wire visible schematic controller interfaces"
```

---

### Task 7: Route the DAC and analogue galvo path

**Files:** same three files as Task 5.

**Interfaces:**

- Adds `route_analog(builder: LayoutBuilder, nets: NetIndex) -> None`.
- Produces left-to-right U1 corridor → U_DACLV1 → U2 → U12 → U13 chains and
  top-to-bottom analogue supplies/returns.

- [ ] **Step 1: Add failing coverage and ordering tests**

```python
def test_analog_route_covers_spi_translation_and_xy_outputs(self):
    report = self.build_production_copy()
    required = {
        "/DAC_CLR_3V3", "/DAC_CLR_5V", "/DAC_DIN_3V3", "/DAC_DIN_5V",
        "/DAC_SCLK_3V3", "/DAC_SCLK_5V", "/DAC_SYNC_3V3", "/DAC_SYNC_5V",
        "Net-(U2-VOUTA)", "Net-(U2-VOUTB)",
        "Net-(U12-OUT_A)", "Net-(U12-OUT_B)",
    }
    self.assertLessEqual(required, report.routed_by_region["analog"])
    self.assertTrue(report.left_to_right("U_DACLV1", "U2", "U12", "U13"))
```

Run the focused test and expect FAIL on empty analogue routes.

- [ ] **Step 2: Implement analogue routing**

Fan out the eight DAC translator nets as parallel visible wires with one-grid
spacing and no bus objects. Keep DAC reference/bypass and U12 feedback parts
adjacent to their pins. Route X and Y as two matched horizontal channel bands.
Place `+5V_ANA` and `+15V` above U2/U12, `-15V` and
`AGND (from Galvo Board)` below, and show the intentional return bridge without
merging unrelated grounds.

- [ ] **Step 3: Regenerate and prove analogue preservation**

```bash
proof_dir=$(cat /tmp/galvos-visible-wires-proof-path)
python -B hardware/tools/create_visible_wire_schematic.py \
  --allow-incomplete \
  --source 'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch' \
  --baseline "$proof_dir/canonical.xml" \
  --output 'hardware/snapshots/2026-09-15-visible-wires/Laser Controllerv2_only_for_pcb_test.kicad_sch'
kicad-cli sch export netlist --format kicadxml -o "$proof_dir/after-analog.xml" \
  'hardware/snapshots/2026-09-15-visible-wires/Laser Controllerv2_only_for_pcb_test.kicad_sch'
python -B hardware/tests/check_schematic_preservation.py \
  "$proof_dir/canonical.xml" "$proof_dir/after-analog.xml"
python -B hardware/tests/check_dac_interface.py "$proof_dir/after-analog.xml"
```

Expected: both checks pass; exact SPI and analogue net memberships are intact.

- [ ] **Step 4: Commit the analogue route**

```bash
git add hardware/tools/create_visible_wire_schematic.py \
  hardware/tests/test_create_visible_wire_schematic.py \
  'hardware/snapshots/2026-09-15-visible-wires/Laser Controllerv2_only_for_pcb_test.kicad_sch'
git commit -m "style(hw): wire visible schematic analogue path"
```

---

### Task 8: Route the RGB laser interface

**Files:** same three files as Task 5.

**Interfaces:**

- Adds `route_laser(builder: LayoutBuilder, nets: NetIndex) -> None`.
- Produces three aligned U1 corridor → 6N137/bias → RGB connector channels.

- [ ] **Step 1: Add a failing three-channel symmetry test**

```python
def test_laser_routes_three_parallel_channels(self):
    report = self.build_production_copy()
    self.assertEqual(report.laser_channel_order, [
        ("/GPIO7", "/TTL_RGB_RED", "U15", "U4"),
        ("/GPIO8", "/TTL_RGB_GREEN", "U16", "U5"),
        ("/GPIO21", "/TTL_RGB_BLUE", "U17", "U3"),
    ])
```

Run the focused test and expect FAIL because the channel report is empty.

- [ ] **Step 2: Implement three parallel visible channels**

Use red, green, and blue horizontal channel bands from top to bottom. Put each
fail-safe pull-up and optocoupler directly in its band, with supply above and
return below. Route the exact inverted logic unchanged: GPIO HIGH remains laser
OFF through the existing 6N137 circuit. Do not add an enable, interlock, or
polarity annotation that implies new electrical behavior.

- [ ] **Step 3: Regenerate and prove electrical identity**

```bash
proof_dir=$(cat /tmp/galvos-visible-wires-proof-path)
python -B hardware/tools/create_visible_wire_schematic.py \
  --allow-incomplete \
  --source 'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch' \
  --baseline "$proof_dir/canonical.xml" \
  --output 'hardware/snapshots/2026-09-15-visible-wires/Laser Controllerv2_only_for_pcb_test.kicad_sch'
kicad-cli sch export netlist --format kicadxml -o "$proof_dir/after-laser.xml" \
  'hardware/snapshots/2026-09-15-visible-wires/Laser Controllerv2_only_for_pcb_test.kicad_sch'
python -B hardware/tests/check_schematic_preservation.py \
  "$proof_dir/canonical.xml" "$proof_dir/after-laser.xml"
```

Expected: preservation passes, including RGB pin membership and exact net
names.

- [ ] **Step 4: Commit the RGB route**

```bash
git add hardware/tools/create_visible_wire_schematic.py \
  hardware/tests/test_create_visible_wire_schematic.py \
  'hardware/snapshots/2026-09-15-visible-wires/Laser Controllerv2_only_for_pcb_test.kicad_sch'
git commit -m "style(hw): wire visible schematic laser interface"
```

---

### Task 9: Route E-stop, watchdog, scan-fail, and SSR status

**Files:** same three files as Task 5.

**Interfaces:**

- Adds `route_safety(builder: LayoutBuilder, nets: NetIndex) -> None`.
- Completes every remaining multi-pin signal net and returns a production
  schematic that passes the full visible-connectivity checker.

- [ ] **Step 1: Add failing safety coverage and completion tests**

```python
def test_safety_region_routes_current_status_interfaces(self):
    report = self.build_production_copy()
    required = {
        "/GPIO14", "/GPIO38", "/GPIO39", "/GPIO47", "/SCAN_CONT",
        "/SCAN_RC", "/SCAN_STATUS_5V", "/SCAN_TRIG", "/SSR1_CTRL",
        "/WD_CONT", "/WD_OUT", "/WD_RC", "/WD_TRIG",
    }
    self.assertLessEqual(required, report.routed_by_region["safety"])
    self.assertEqual(report.unrouted_multi_pin_signal_nets, set())
```

Run the focused test and expect FAIL on safety nets and remaining route count.

- [ ] **Step 2: Implement safety/status lanes without redesign**

Place J_ESTOP1 on the left, watchdog and scan timers with their RC/clamp
networks centrally, and J_SSR1 on the right. Keep scan status visibly distinct
from mirror-motion feedback. Route GPIO14/38/39/47 from the left inter-region
corridor into the block, reserve separate lanes for watchdog and scan RC nets,
and retain the existing NE555-to-330-ohm SSR connector meaning. Add no latch,
restart acknowledgement, second SSR channel, or firmware dependency.

- [ ] **Step 3: Add one hidden naming label per continuous signal graph**

After all wires and junctions are complete, add exactly one hidden local label
at the lexicographically smallest grid coordinate on every routed named signal
graph whose baseline XML name starts with `/`; the label text is the baseline
name with that one root-sheet slash removed. Anonymous `Net-(...)` graphs get
no label and retain their automatic names through unchanged pin membership.
These local labels retain named nets in KiCad but never render.

Delete the per-pin fallback-label generation and remove the
`--allow-incomplete` CLI option and its development-only test. Make `build()`
raise `ValueError` when `unrouted_multi_pin_signal_nets` is non-empty.

- [ ] **Step 4: Regenerate and run full connectivity plus safety gates**

```bash
proof_dir=$(cat /tmp/galvos-visible-wires-proof-path)
python -B hardware/tools/create_visible_wire_schematic.py \
  --source 'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch' \
  --baseline "$proof_dir/canonical.xml" \
  --output 'hardware/snapshots/2026-09-15-visible-wires/Laser Controllerv2_only_for_pcb_test.kicad_sch'
kicad-cli sch export netlist --format kicadxml -o "$proof_dir/after-safety.xml" \
  'hardware/snapshots/2026-09-15-visible-wires/Laser Controllerv2_only_for_pcb_test.kicad_sch'
python -B hardware/tests/check_schematic_preservation.py \
  "$proof_dir/canonical.xml" "$proof_dir/after-safety.xml"
python -B hardware/tests/check_visible_connectivity.py \
  "$proof_dir/canonical.xml" \
  'hardware/snapshots/2026-09-15-visible-wires/Laser Controllerv2_only_for_pcb_test.kicad_sch'
python -B hardware/tests/check_scan_status.py "$proof_dir/after-safety.xml"
python -B hardware/tests/check_trigger_diodes.py "$proof_dir/after-safety.xml"
```

Expected: all checks pass. The visible checker reports zero disconnected signal
nets, zero foreign-net joins, zero visible labels/buses, and exact junction use.

- [ ] **Step 5: Commit the safety route**

```bash
git add hardware/tools/create_visible_wire_schematic.py \
  hardware/tests/test_create_visible_wire_schematic.py \
  'hardware/snapshots/2026-09-15-visible-wires/Laser Controllerv2_only_for_pcb_test.kicad_sch'
git commit -m "style(hw): wire visible schematic safety status"
```

---

### Task 10: Enforce A1 presentation, field placement, and routing corridors

**Files:**

- Modify: `hardware/tests/check_schematic_presentation.py`
- Modify: `hardware/tests/test_schematic_presentation.py`
- Modify: `hardware/tools/create_visible_wire_schematic.py`
- Modify: copied `.kicad_sch` target.

**Interfaces:**

- Extends CLI with `--profile {canonical,visible-copy}`; default remains
  `canonical`, so existing invocations retain behavior.
- Adds `check_visible_copy(objects: list[SExprObject]) -> None` for A1 page,
  five regions, left-to-right principal chains, horizontal fields, grid,
  routing-corridor bounds, and source-level collision boxes.

- [ ] **Step 1: Add profile-regression and visible-copy tests**

```python
def test_canonical_profile_keeps_existing_user_page_contract(self):
    check_document(self.canonical_fixture(), profile="canonical")

def test_visible_copy_requires_a1(self):
    with self.assertRaisesRegex(ValueError, "A1 landscape"):
        check_document(self.user_page_fixture(), profile="visible-copy")

def test_visible_copy_rejects_reversed_principal_chain(self):
    with self.assertRaisesRegex(ValueError, "U_DACLV1.*U2.*U12.*U13"):
        check_visible_chain(self.reversed_analog_fixture())

def test_visible_copy_rejects_vertical_reference_or_value(self):
    with self.assertRaisesRegex(ValueError, "visible field.*horizontal"):
        check_document(self.vertical_field_fixture(), profile="visible-copy")

def test_visible_copy_rejects_wire_field_overlap(self):
    with self.assertRaisesRegex(ValueError, "wire.*Reference"):
        check_document(self.wire_through_reference_fixture(), profile="visible-copy")
```

- [ ] **Step 2: Run presentation tests and confirm the new tests fail**

```bash
PYTHONDONTWRITEBYTECODE=1 python -m unittest \
  hardware/tests/test_schematic_presentation.py -v
```

Expected: existing canonical tests pass and new visible-copy tests fail because
the profile is not implemented.

- [ ] **Step 3: Implement the visible-copy profile**

Reuse `REGIONS`, `COLUMNS`, `PRINCIPAL`, and geometry helpers from the
generator. Approximate rendered field boxes from font size, text length,
justification, and orientation; compare those boxes with symbols, wires,
section headings, and other visible fields. Ignore hidden properties. Require
Reference above and Value below the symbol centre, or documented left/right
placement when the symbol body is vertical. Accumulate every violation so one
run yields a complete correction list.

- [ ] **Step 4: Normalize fields and section headings in the generator**

Set every visible Reference and Value to horizontal rendered orientation,
clear stale `fields_autoplaced`, and place them one grid step outside the symbol
body. Add five English section headings at each region's upper-left grid point.
Keep anonymous net names only in hidden labels. Move route lanes, fields, or
symbols until both source-level checkers pass; never suppress a collision.

- [ ] **Step 5: Run full source-level checks and unit tests**

```bash
proof_dir=$(cat /tmp/galvos-visible-wires-proof-path)
python -B hardware/tools/create_visible_wire_schematic.py \
  --source 'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch' \
  --baseline "$proof_dir/canonical.xml" \
  --output 'hardware/snapshots/2026-09-15-visible-wires/Laser Controllerv2_only_for_pcb_test.kicad_sch'
python -B hardware/tests/check_visible_connectivity.py \
  "$proof_dir/canonical.xml" \
  'hardware/snapshots/2026-09-15-visible-wires/Laser Controllerv2_only_for_pcb_test.kicad_sch'
python -B hardware/tests/check_schematic_presentation.py \
  --profile visible-copy --block all \
  'hardware/snapshots/2026-09-15-visible-wires/Laser Controllerv2_only_for_pcb_test.kicad_sch'
PYTHONDONTWRITEBYTECODE=1 python -m unittest discover \
  -s hardware/tests -p 'test_*.py' -v
```

Expected: both schematic checkers and all hardware unit tests pass. The current
baseline is 69 tests; the final count is 69 plus the new test methods added in
Tasks 2–4 and 10.

- [ ] **Step 6: Commit presentation normalization**

```bash
git add hardware/tests/check_schematic_presentation.py \
  hardware/tests/test_schematic_presentation.py \
  hardware/tools/create_visible_wire_schematic.py \
  'hardware/snapshots/2026-09-15-visible-wires/Laser Controllerv2_only_for_pcb_test.kicad_sch'
git commit -m "style(hw): normalize visible schematic presentation"
```

---

### Task 11: Export PDF, inspect at 240 dpi, and prove all invariants

**Files:**

- Modify:
  `hardware/snapshots/2026-09-15-visible-wires/GalvOS_V2.0.10_Schematic.pdf`
- Create: `hardware/reviews/2026-09-15-visible-wire-schematic.md`

**Interfaces:**

- Consumes final copied schematic and all automated gates.
- Produces one-page A1 PDF and a dated evidence record; changes no canonical
  schematic, PCB, project, PDF, or firmware file.

- [ ] **Step 1: Export final XML and PDF directly from the copied schematic**

```bash
proof_dir=$(cat /tmp/galvos-visible-wires-proof-path)
copy_sch='hardware/snapshots/2026-09-15-visible-wires/Laser Controllerv2_only_for_pcb_test.kicad_sch'
copy_pdf='hardware/snapshots/2026-09-15-visible-wires/GalvOS_V2.0.10_Schematic.pdf'
kicad-cli sch export netlist --format kicadxml \
  -o "$proof_dir/final.xml" "$copy_sch"
kicad-cli sch export pdf -o "$copy_pdf" "$copy_sch"
pdfinfo "$copy_pdf" | rg '^(Pages|Page size):'
```

Expected: one page; A1 landscape page size is approximately
`2383.94 x 1683.78 pts` (841 x 594 mm).

- [ ] **Step 2: Run electrical preservation and all seven interface gates**

```bash
proof_dir=$(cat /tmp/galvos-visible-wires-proof-path)
python -B hardware/tests/check_schematic_preservation.py \
  "$proof_dir/canonical.xml" "$proof_dir/final.xml"
python -B hardware/tests/check_fan_power.py "$proof_dir/final.xml"
python -B hardware/tests/check_sensor_fan_interfaces.py "$proof_dir/final.xml"
python -B hardware/tests/check_dac_interface.py "$proof_dir/final.xml"
python -B hardware/tests/check_scan_status.py "$proof_dir/final.xml"
python -B hardware/tests/check_dmx_interface.py "$proof_dir/final.xml"
python -B hardware/tests/check_buck_interface.py "$proof_dir/final.xml"
python -B hardware/tests/check_trigger_diodes.py "$proof_dir/final.xml"
```

Expected: eight PASS results and exact 122-component/110-net identity.

- [ ] **Step 3: Run visible, presentation, unit, ERC, and PCB DRC gates**

```bash
proof_dir=$(cat /tmp/galvos-visible-wires-proof-path)
copy_sch='hardware/snapshots/2026-09-15-visible-wires/Laser Controllerv2_only_for_pcb_test.kicad_sch'
erc_dir="$proof_dir/erc-project"
install -d "$erc_dir"
ln -s "$(pwd)/$copy_sch" \
  "$erc_dir/Laser Controllerv2_only_for_pcb_test.kicad_sch"
cp 'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_pro' \
  "$erc_dir/Laser Controllerv2_only_for_pcb_test.kicad_pro"
python -B hardware/tests/check_visible_connectivity.py \
  "$proof_dir/canonical.xml" "$copy_sch"
python -B hardware/tests/check_schematic_presentation.py \
  --profile visible-copy --block all "$copy_sch"
PYTHONDONTWRITEBYTECODE=1 python -m unittest discover \
  -s hardware/tests -p 'test_*.py' -v
kicad-cli sch erc --format json --severity-all --exit-code-violations \
  -o "$proof_dir/final-erc.json" \
  "$erc_dir/Laser Controllerv2_only_for_pcb_test.kicad_sch"
kicad-cli pcb drc --refill-zones --schematic-parity --all-track-errors \
  --format json --severity-all -o "$proof_dir/final-drc.json" \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_pcb'
python -B hardware/tests/check_pcb_draft.py \
  "$proof_dir/final-drc.json" \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_pro'
```

Expected: connectivity and presentation pass; all unit tests pass; ERC has zero
violations; DRC has zero unconnected and zero schematic-parity items, with only
the already documented `U_BUCK1` footprint-type warning.

- [ ] **Step 4: Prove PCB layout checks and immutable hashes**

```bash
proof_dir=$(cat /tmp/galvos-visible-wires-proof-path)
/usr/bin/python hardware/tests/check_dac_source_layout.py \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_pcb'
/usr/bin/python hardware/tests/check_buck_input_layout.py \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_pcb'
sha256sum -c "$proof_dir/immutable.sha256"
```

Expected: both layout guards pass and all four canonical schematic/PDF/PCB/
project hashes report `OK`.

- [ ] **Step 5: Render and inspect the whole page and five crops at 240 dpi**

```bash
proof_dir=$(cat /tmp/galvos-visible-wires-proof-path)
copy_pdf='hardware/snapshots/2026-09-15-visible-wires/GalvOS_V2.0.10_Schematic.pdf'
pdftoppm -png -singlefile -r 240 "$copy_pdf" \
  "$proof_dir/visible-wire-full"
pdftoppm -png -singlefile -r 240 -x 240 -y 240 -W 7464 -H 840 \
  "$copy_pdf" "$proof_dir/visible-wire-power"
pdftoppm -png -singlefile -r 240 -x 240 -y 1200 -W 7464 -H 960 \
  "$copy_pdf" "$proof_dir/visible-wire-io"
pdftoppm -png -singlefile -r 240 -x 240 -y 2280 -W 7464 -H 960 \
  "$copy_pdf" "$proof_dir/visible-wire-analog"
pdftoppm -png -singlefile -r 240 -x 240 -y 3360 -W 7464 -H 840 \
  "$copy_pdf" "$proof_dir/visible-wire-laser"
pdftoppm -png -singlefile -r 240 -x 240 -y 4320 -W 7464 -H 1056 \
  "$copy_pdf" "$proof_dir/visible-wire-safety"
```

Open the full PNG and all five generated region PNGs with the workspace image
viewer. Record PASS only after confirming visible
continuous wires, junctions at branches, no visible labels, readable
Reference/Value text, no wire/text/symbol overlap, separated parallel lanes,
left-to-right signal chains, and supply-over-ground flow in all five regions.

- [ ] **Step 6: Write the dated review**

Record:

- canonical and copied schematic/PDF SHA-256 hashes;
- exact component/net counts and XML preservation PASS;
- visible checker counts and zero failures by category;
- five-row visual-inspection table at 240 dpi;
- all seven interface results, hardware unit-test count, ERC result, and DRC
  parity/unconnected result;
- unchanged canonical/PCB/project hash proof;
- the remaining `U_BUCK1` warning;
- explicit non-claims for PCB update, fabrication, safety qualification,
  production approval, and the separate SSR restart-interlock design.

- [ ] **Step 7: Audit final scope and commit evidence**

```bash
git diff --check
git status --short
git diff --name-only 9403bd6
git diff --stat 9403bd6
git add \
  'hardware/snapshots/2026-09-15-visible-wires/GalvOS_V2.0.10_Schematic.pdf' \
  hardware/reviews/2026-09-15-visible-wire-schematic.md
git diff --cached --name-only
git commit -m "docs(hw): verify visible-wire schematic copy"
```

Expected: the final branch changes only the copied schematic/PDF, three new
tool/checker modules, their tests, the presentation checker/tests, this plan,
and the dated review. Canonical hardware, PCB, project, firmware, Gerbers,
manufacturing files, and `agents.md` are absent from the diff and staging area.
