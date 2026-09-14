"""Regression cases for SMD coverage, including the plated-hole buck."""

import unittest

from check_smd_placement_coverage import check_rows


class PlacementCoverageTests(unittest.TestCase):
    def setUp(self):
        self.expected = {
            "U_BUCK1": ("LMR33630ADDA", "HSOP_thermal_holes", "top"),
            "R1": ("10k", "R_1206", "bottom"),
        }
        self.rows = [
            {"Ref": ref, "Val": value, "Package": package, "Side": side}
            for ref, (value, package, side) in self.expected.items()
        ]

    def test_complete_inventory(self):
        self.assertEqual(check_rows(self.rows, self.expected), 2)

    def test_missing_plated_hole_smd(self):
        with self.assertRaisesRegex(ValueError, "missing=.*U_BUCK1"):
            check_rows(self.rows[1:], self.expected)

    def test_duplicate_reference(self):
        with self.assertRaisesRegex(ValueError, "duplicate"):
            check_rows(self.rows + [self.rows[0]], self.expected)

    def test_unexpected_part(self):
        extra = dict(self.rows[0], Ref="J1")
        with self.assertRaisesRegex(ValueError, "extra=.*J1"):
            check_rows(self.rows + [extra], self.expected)

    def test_wrong_identity_or_side(self):
        for field in ("Val", "Package", "Side"):
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "mismatch"):
                rows = [dict(self.rows[0], **{field: "wrong"}), self.rows[1]]
                check_rows(rows, self.expected)

    def test_malformed_row(self):
        with self.assertRaisesRegex(ValueError, "fields"):
            check_rows([{"Ref": "U_BUCK1"}], self.expected)

    def test_empty_csv(self):
        with self.assertRaisesRegex(ValueError, "missing"):
            check_rows([], self.expected)

    def test_empty_board(self):
        with self.assertRaisesRegex(ValueError, "No populated SMD"):
            check_rows([], {})


if __name__ == "__main__":
    unittest.main()
