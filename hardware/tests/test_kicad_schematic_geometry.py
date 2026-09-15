"""Focused tests for KiCad schematic source and wire geometry."""

from decimal import Decimal
import unittest

from hardware.tools.kicad_schematic_geometry import (
    graph_for,
    parse_document,
    pin_endpoints,
    transform_point,
)


D = Decimal


class GeometryTests(unittest.TestCase):
    def test_round_trip_preserves_untouched_source_bytes(self):
        text = (
            '(kicad_sch (text "A (B)") '
            '(wire (pts (xy 1.27 2.54) (xy 5.08 2.54))))'
        )
        self.assertEqual(parse_document(text).render(), text)

    def test_document_splices_replacement_and_append_without_reformatting(self):
        text = "(kicad_sch\n  (paper \"A4\")\n)"
        document = parse_document(text)
        paper = document.objects[0]
        document.replace(paper, '(paper "A1")')
        document.append('(text "POWER" (at 25.4 25.4 0))')
        self.assertEqual(
            document.render(),
            '(kicad_sch\n  (paper "A1")\n\n  (text "POWER" (at 25.4 25.4 0)))',
        )

    def test_pin_transform_handles_rotation_and_mirror(self):
        self.assertEqual(
            transform_point(
                (D("2.54"), D("0")),
                (D("10.16"), D("20.32")),
                D("90"),
                "x",
            ),
            (D("10.16"), D("22.86")),
        )

    def test_resolves_placed_pin_from_embedded_library_symbol(self):
        text = '''(kicad_sch
  (lib_symbols
    (symbol "Fixture:Part"
      (symbol "Part_1_1"
        (pin passive line (at 2.54 0 180)
          (name "P") (number "1")))))
  (symbol (lib_id "Fixture:Part") (at 10.16 20.32 90) (unit 1)
    (property "Reference" "U1" (at 0 0 0))
    (pin "1" (uuid "00000000-0000-0000-0000-000000000001"))))'''
        self.assertEqual(
            pin_endpoints(parse_document(text)),
            {("U1", "1"): (D("10.16"), D("22.86"))},
        )

    def test_crossing_without_junction_stays_disconnected(self):
        graph = graph_for(
            wires=[
                (("0", "2.54"), ("5.08", "2.54")),
                (("2.54", "0"), ("2.54", "5.08")),
            ],
            junctions=[],
        )
        self.assertNotEqual(
            graph.component((D("0"), D("2.54")), axis="h"),
            graph.component((D("2.54"), D("0")), axis="v"),
        )

    def test_t_branch_with_junction_is_one_component(self):
        graph = graph_for(
            wires=[
                (("0", "2.54"), ("5.08", "2.54")),
                (("2.54", "2.54"), ("2.54", "5.08")),
            ],
            junctions=[("2.54", "2.54")],
        )
        self.assertEqual(graph.degree((D("2.54"), D("2.54"))), 3)
        self.assertEqual(
            graph.component((D("0"), D("2.54")), axis="h"),
            graph.component((D("2.54"), D("5.08")), axis="v"),
        )


if __name__ == "__main__":
    unittest.main()
