"""Tests for the deterministic visible-wire schematic generator."""

from decimal import Decimal
import unittest

from hardware.tools.kicad_schematic_geometry import parse_document
from hardware.tests.check_schematic_presentation import (
    check_field_wire_collisions,
    parse_root_objects,
)

from hardware.tools.create_visible_wire_schematic import (
    RouteResult,
    _detour_route,
    _normalize_wires,
    _prune_segment_leaves,
    _remove_redundant_wires,
    _wire_segments,
    add_hidden_effect,
    build_text,
    route_tree,
    segments_conflict,
    translate_object,
)


D = Decimal


class VisibleWireGeneratorTests(unittest.TestCase):
    def test_prune_keeps_trunk_joining_branch_interiors(self):
        left_top = (D("10.16"), D("10.16"))
        left_bottom = (D("10.16"), D("20.32"))
        right_top = (D("20.32"), D("10.16"))
        right_bottom = (D("20.32"), D("20.32"))
        trunk = ((D("10.16"), D("15.24")), (D("20.32"), D("15.24")))
        segments = (
            (left_top, left_bottom),
            (right_top, right_bottom),
            trunk,
        )

        pruned = _prune_segment_leaves(
            segments,
            {left_top, left_bottom, right_top, right_bottom},
            set(),
        )

        self.assertIn(trunk, pruned)

    def fixture(self):
        source = '''(kicad_sch
  (paper "User" 620 440)
  (lib_symbols
    (symbol "Fixture:One"
      (symbol "One_1_1"
        (pin passive line (at 0 0 0) (name "P") (number "1")))))
  (symbol (lib_id "Fixture:One") (at 10.16 10.16 0) (unit 1)
    (property "Reference" "J1" (at 10.16 7.62 0))
    (property "Value" "IN" (at 10.16 12.70 0))
    (pin "1" (uuid "00000000-0000-0000-0000-000000000001")))
  (symbol (lib_id "Fixture:One") (at 20.32 10.16 0) (unit 1)
    (property "Reference" "U1" (at 20.32 7.62 0))
    (property "Value" "OUT" (at 20.32 12.70 0))
    (pin "1" (uuid "00000000-0000-0000-0000-000000000002")))
  (label "SIG" (at 10.16 10.16 0)
    (effects (font (size 1.27 1.27)))
    (uuid "00000000-0000-0000-0000-000000000003"))
  (label "SIG" (at 20.32 10.16 180)
    (effects (font (size 1.27 1.27)))
    (uuid "00000000-0000-0000-0000-000000000004")))'''
        baseline = {"/SIG": frozenset({("J1", "1"), ("U1", "1")})}
        return source, baseline

    def test_build_text_creates_deterministic_a1_visible_connection(self):
        source, baseline = self.fixture()
        first, first_report = build_text(source, baseline)
        second, second_report = build_text(source, baseline)
        self.assertEqual(first, second)
        self.assertEqual(first_report, second_report)
        self.assertIn('(paper "A1")', first)
        self.assertEqual(first.count('(label "SIG"'), 1)
        self.assertIn('(hide yes)', first)
        self.assertIn('(wire', first)
        self.assertEqual(first_report.unrouted_multi_pin_signal_nets, frozenset())
        self.assertFalse(any(line.endswith(" ") for line in first.splitlines()))

    def test_build_text_replaces_label_stub_with_routing_port(self):
        source, baseline = self.fixture()
        source = source.replace(
            '(label "SIG" (at 10.16 10.16 0)',
            '(wire (pts (xy 10.16 10.16) (xy 12.70 10.16)) '
            '(uuid "stub-a"))\n  (label "SIG" (at 12.70 10.16 0)',
        )
        rendered, report = build_text(source, baseline)
        self.assertNotIn('"stub-a"', rendered)
        self.assertEqual(report.unrouted_multi_pin_signal_nets, frozenset())

    def test_build_text_clears_wire_field_collisions(self):
        source, baseline = self.fixture()
        source = source.replace(
            '(label "SIG"',
            '''(wire
    (pts (xy 5.08 7.62) (xy 15.24 7.62))
    (stroke (width 0) (type default))
    (uuid "00000000-0000-0000-0000-000000000099"))
  (label "SIG"''',
            1,
        )

        rendered, _ = build_text(source, baseline)

        check_field_wire_collisions(parse_root_objects(rendered))

    def test_translate_object_moves_every_drawing_coordinate(self):
        source = '''(symbol (lib_id "Fixture:X") (at 10.16 20.32 90)
  (property "Reference" "U1" (at 7.62 20.32 0))
  (property "Value" "X" (at 12.70 20.32 0)))'''
        self.assertEqual(
            translate_object(source, D("100.33"), D("76.20")),
            '''(symbol (lib_id "Fixture:X") (at 110.49 96.52 90)
  (property "Reference" "U1" (at 107.95 96.52 0))
  (property "Value" "X" (at 113.03 96.52 0)))''',
        )

    def test_add_hidden_effect_is_idempotent(self):
        visible = '(label "SIG" (at 10.16 10.16 0) (effects (font (size 1.27 1.27))))'
        hidden = add_hidden_effect(visible)
        self.assertIn('(hide yes)', hidden)
        self.assertEqual(add_hidden_effect(hidden), hidden)

    def test_route_tree_is_deterministic_and_branches_explicitly(self):
        ports = [
            (D("10.16"), D("10.16")),
            (D("20.32"), D("10.16")),
            (D("15.24"), D("15.24")),
        ]
        first = route_tree("/SIG", ports, [])
        second = route_tree("/SIG", list(reversed(ports)), [])
        self.assertEqual(first, second)
        self.assertTrue(all(a[0] == b[0] or a[1] == b[1] for a, b in first.segments))
        self.assertEqual(first.junctions, {(D("15.24"), D("10.16"))})

    def test_route_tree_splits_trunk_at_collinear_pin_ports(self):
        ports = [
            (D("10.16"), D("10.16")),
            (D("15.24"), D("10.16")),
            (D("20.32"), D("10.16")),
        ]
        route = route_tree("/SIG", ports, [])
        endpoints = {point for segment in route.segments for point in segment}
        self.assertTrue(set(ports) <= endpoints)

    def test_route_tree_keeps_parallel_net_trunks_one_grid_apart(self):
        first = route_tree(
            "/A",
            [(D("10.16"), D("10.16")), (D("20.32"), D("15.24"))],
            [],
        )
        second = route_tree(
            "/B",
            [(D("10.16"), D("11.43")), (D("20.32"), D("16.51"))],
            [first],
        )
        first_lines = {segment for segment in first.segments}
        self.assertTrue(first_lines.isdisjoint(second.segments))

    def test_route_tree_avoids_collinear_foreign_wire(self):
        obstacle = ((D("12.70"), D("10.16")), (D("25.40"), D("10.16")))
        route = route_tree(
            "/SIG",
            [(D("10.16"), D("10.16")), (D("20.32"), D("15.24"))],
            [],
            blocked_segments=[obstacle],
        )
        self.assertFalse(any(segments_conflict(segment, obstacle) for segment in route.segments))


    def test_detour_recomputes_junctions_from_final_segments(self):
        route = RouteResult(
            "/SIG",
            (((D("10.16"), D("10.16")), (D("20.32"), D("10.16"))),
             ((D("15.24"), D("10.16")), (D("15.24"), D("20.32")))),
            frozenset({(D("15.24"), D("10.16"))}),
        )
        detoured = _detour_route(
            route,
            {(D("10.16"), D("10.16")), (D("20.32"), D("10.16")),
             (D("15.24"), D("20.32"))},
            [],
            {(D("15.24"), D("10.16"))},
            [],
        )
        self.assertNotIn((D("15.24"), D("10.16")), detoured.junctions)
        self.assertEqual(detoured.junctions, frozenset())

    def test_removes_wire_fully_covered_by_longer_segment(self):
        document = parse_document('''(kicad_sch
  (wire (pts (xy 10.16 10.16) (xy 20.32 10.16)) (uuid "long"))
  (wire (pts (xy 12.70 10.16) (xy 13.97 10.16)) (uuid "short")))''')
        self.assertEqual(_remove_redundant_wires(document), 1)
        rendered = document.render()
        self.assertIn('"long"', rendered)
        self.assertNotIn('"short"', rendered)

    def test_normalizes_collinear_wires_and_splits_only_at_pin(self):
        document = parse_document('''(kicad_sch
  (wire (pts (xy 10.16 10.16) (xy 12.70 10.16)) (uuid "a"))
  (wire (pts (xy 12.70 10.16) (xy 15.24 10.16)) (uuid "b")))''')
        _normalize_wires(document, {(D("13.97"), D("10.16"))})
        rendered = parse_document(document.render())
        segments = []
        for obj in rendered.objects:
            if obj.kind == "wire":
                segments.extend(_wire_segments(obj.source))
        self.assertEqual(
            segments,
            [((D("10.16"), D("10.16")), (D("13.97"), D("10.16"))),
             ((D("13.97"), D("10.16")), (D("15.24"), D("10.16")))],
        )


if __name__ == "__main__":
    unittest.main()
