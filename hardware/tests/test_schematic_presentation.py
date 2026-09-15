"""Unit tests for the V2 schematic presentation contract."""

from decimal import Decimal
import unittest

from check_schematic_presentation import (
    SExprObject,
    check_chain,
    check_fields,
    check_grid,
    parse_root_objects,
)


class SchematicPresentationTests(unittest.TestCase):
    def test_root_parser_ignores_parentheses_inside_strings(self):
        objects = parse_root_objects(
            '(kicad_sch (text "A (B)") (wire (pts (xy 1.27 2.54))))'
        )

        self.assertEqual([obj.kind for obj in objects], ["text", "wire"])

    def test_root_parser_handles_escaped_quote(self):
        objects = parse_root_objects(
            '(kicad_sch (text "A \\"quoted\\" value") (junction (at 1.27 2.54)))'
        )

        self.assertEqual([obj.kind for obj in objects], ["text", "junction"])

    def test_rejects_off_grid_wire_vertex(self):
        objects = parse_root_objects(
            '(kicad_sch (wire (pts (xy 1.30 2.54) (xy 5.08 2.54))))'
        )

        with self.assertRaisesRegex(ValueError, "1.27 mm grid"):
            check_grid(objects)

    def test_rejects_diagonal_wire(self):
        objects = parse_root_objects(
            '(kicad_sch (wire (pts (xy 1.27 2.54) (xy 5.08 6.35))))'
        )

        with self.assertRaisesRegex(ValueError, "orthogonal"):
            check_grid(objects)

    def test_accepts_serialization_noise_on_horizontal_wire(self):
        objects = parse_root_objects(
            "(kicad_sch (wire (pts "
            "(xy 185.42000000000002 180.33999999999997) "
            "(xy 196.85 180.34))))"
        )

        check_grid(objects)

    def test_rejects_electrical_bus(self):
        objects = parse_root_objects(
            '(kicad_sch (bus (pts (xy 1.27 2.54) (xy 5.08 2.54))))'
        )

        with self.assertRaisesRegex(ValueError, "bus"):
            check_grid(objects)

    def test_rejects_vertical_reference_field(self):
        objects = parse_root_objects(
            self.symbol(
                reference="U1",
                symbol_at=(Decimal("10.16"), Decimal("10.16"), 0),
                reference_at=(Decimal("10.16"), Decimal("7.62"), 90),
                value_at=(Decimal("10.16"), Decimal("12.70"), 0),
            )
        )

        with self.assertRaisesRegex(ValueError, "Reference.*horizontal"):
            check_fields(objects)

    def test_rejects_ic_value_above_symbol(self):
        objects = parse_root_objects(
            self.symbol(
                reference="U1",
                symbol_at=(Decimal("10.16"), Decimal("10.16"), 0),
                reference_at=(Decimal("10.16"), Decimal("7.62"), 0),
                value_at=(Decimal("10.16"), Decimal("8.89"), 0),
            )
        )

        with self.assertRaisesRegex(ValueError, "Value.*below"):
            check_fields(objects)

    def test_accepts_standard_ic_fields(self):
        objects = parse_root_objects(
            self.symbol(
                reference="U1",
                symbol_at=(Decimal("10.16"), Decimal("10.16"), 0),
                reference_at=(Decimal("10.16"), Decimal("7.62"), 0),
                value_at=(Decimal("10.16"), Decimal("12.70"), 0),
            )
        )

        check_fields(objects)

    def test_rejects_fields_that_render_vertical_on_rotated_symbol(self):
        objects = parse_root_objects(
            self.symbol(
                reference="R1",
                symbol_at=(Decimal("10.16"), Decimal("10.16"), 270),
                reference_at=(Decimal("5.08"), Decimal("10.16"), 0),
                value_at=(Decimal("15.24"), Decimal("10.16"), 0),
            )
        )

        with self.assertRaisesRegex(ValueError, "Reference.*horizontal"):
            check_fields(objects)

    def test_accepts_counter_rotated_fields_on_rotated_symbol(self):
        objects = parse_root_objects(
            self.symbol(
                reference="R1",
                symbol_at=(Decimal("10.16"), Decimal("10.16"), 270),
                reference_at=(Decimal("10.16"), Decimal("7.62"), 90),
                value_at=(Decimal("10.16"), Decimal("12.70"), 90),
            )
        )

        check_fields(objects)

    def test_rejects_rotated_passive_fields_inline_with_signal_path(self):
        objects = parse_root_objects(
            self.symbol(
                reference="R1",
                symbol_at=(Decimal("10.16"), Decimal("10.16"), 270),
                reference_at=(Decimal("5.08"), Decimal("10.16"), 90),
                value_at=(Decimal("15.24"), Decimal("10.16"), 90),
            )
        )

        with self.assertRaisesRegex(ValueError, "Reference.*above"):
            check_fields(objects)

    def test_mirroring_does_not_change_horizontal_field_axis(self):
        objects = parse_root_objects(
            self.symbol(
                reference="R1",
                symbol_at=(Decimal("10.16"), Decimal("10.16"), 270),
                reference_at=(Decimal("10.16"), Decimal("7.62"), 90),
                value_at=(Decimal("10.16"), Decimal("12.70"), 90),
                mirror="x",
            )
        )

        check_fields(objects)

    def test_rejects_reversed_signal_chain(self):
        placements = {
            "J_DMX1": (Decimal("100"), Decimal("100")),
            "U_DMXLV1": (Decimal("80"), Decimal("100")),
            "U1": (Decimal("120"), Decimal("100")),
        }

        with self.assertRaisesRegex(ValueError, "DMX"):
            check_chain("DMX", ("J_DMX1", "U_DMXLV1", "U1"), placements)

    def test_accepts_left_to_right_signal_chain(self):
        placements = {
            "J_DMX1": (Decimal("40"), Decimal("100")),
            "U_DMXLV1": (Decimal("80"), Decimal("100")),
            "U1": (Decimal("120"), Decimal("100")),
        }

        check_chain("DMX", ("J_DMX1", "U_DMXLV1", "U1"), placements)

    @staticmethod
    def symbol(reference, symbol_at, reference_at, value_at, mirror=None):
        sx, sy, sa = symbol_at
        rx, ry, ra = reference_at
        vx, vy, va = value_at
        mirror_clause = f"\n            (mirror {mirror})" if mirror else ""
        return f'''(kicad_sch
          (symbol
            (lib_id "MCU_Module:Fixture")
            (at {sx} {sy} {sa}){mirror_clause}
            (uuid "fixture-{reference}")
            (property "Reference" "{reference}"
              (at {rx} {ry} {ra})
              (effects (font (size 1.27 1.27))))
            (property "Value" "Fixture"
              (at {vx} {vy} {va})
              (effects (font (size 1.27 1.27))))))'''


if __name__ == "__main__":
    unittest.main()
