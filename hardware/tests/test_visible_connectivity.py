"""Contract tests for visible, continuous schematic connectivity."""

from decimal import Decimal
import unittest

from hardware.tests.check_visible_connectivity import Label, validate_connectivity


def point(x: str, y: str):
    return Decimal(x), Decimal(y)


P1 = point("10.16", "10.16")
P2 = point("20.32", "10.16")
P3 = point("15.24", "15.24")


class VisibleConnectivityTests(unittest.TestCase):
    def test_accepts_one_hidden_label_on_continuous_signal(self):
        report = validate_connectivity(
            {"/SIG": frozenset({("J1", "1"), ("U1", "2")})},
            {("J1", "1"): P1, ("U1", "2"): P2},
            [(P1, P2)],
            set(),
            [Label("label", "SIG", point("15.24", "10.16"), True)],
            [],
        )
        self.assertEqual(report.checked_signal_nets, 1)
        self.assertEqual(report.hidden_labels, 1)

    def test_rejects_split_signal_and_duplicate_hidden_labels(self):
        with self.assertRaisesRegex(ValueError, "not continuous.*disconnected labels"):
            validate_connectivity(
                {"/SIG": frozenset({("J1", "1"), ("U1", "2")})},
                {("J1", "1"): P1, ("U1", "2"): P2},
                [(P1, point("12.70", "10.16")),
                 (point("17.78", "10.16"), P2)],
                set(),
                [Label("label", "SIG", point("12.70", "10.16"), True),
                 Label("label", "SIG", point("17.78", "10.16"), True)],
                [],
            )

    def test_rejects_graph_joining_foreign_baseline_nets(self):
        with self.assertRaisesRegex(ValueError, "joins baseline nets /A, /B"):
            validate_connectivity(
                {"/A": frozenset({("J1", "1"), ("U1", "1")}),
                 "/B": frozenset({("J2", "1"), ("U2", "1")})},
                {("J1", "1"): point("10.16", "10.16"),
                 ("U1", "1"): point("15.24", "10.16"),
                 ("J2", "1"): point("20.32", "10.16"),
                 ("U2", "1"): point("25.40", "10.16")},
                [(point("10.16", "10.16"), point("25.40", "10.16"))],
                set(), [], [],
            )

    def test_rejects_signal_graph_joining_power_net(self):
        with self.assertRaisesRegex(ValueError, r"joins baseline nets \+3V3, /SIG"):
            validate_connectivity(
                {
                    "/SIG": frozenset({("J1", "1"), ("U1", "1")}),
                    "+3V3": frozenset({("J2", "1"), ("U2", "1")}),
                },
                {
                    ("J1", "1"): point("10.16", "10.16"),
                    ("U1", "1"): point("15.24", "10.16"),
                    ("J2", "1"): point("20.32", "10.16"),
                    ("U2", "1"): point("25.40", "10.16"),
                },
                [(point("10.16", "10.16"), point("25.40", "10.16"))],
                set(),
                [Label("label", "SIG", point("10.16", "10.16"), True)],
                [],
            )

    def test_crossing_without_junction_keeps_signals_separate(self):
        validate_connectivity(
            {"/H": frozenset({("J1", "1"), ("U1", "1")}),
             "/V": frozenset({("J2", "1"), ("U2", "1")})},
            {("J1", "1"): point("10.16", "15.24"),
             ("U1", "1"): point("20.32", "15.24"),
             ("J2", "1"): point("15.24", "10.16"),
             ("U2", "1"): point("15.24", "20.32")},
            [(point("10.16", "15.24"), point("20.32", "15.24")),
             (point("15.24", "10.16"), point("15.24", "20.32"))],
            set(), [], [],
        )

    def test_t_contact_without_junction_keeps_signals_separate(self):
        validate_connectivity(
            {"/H": frozenset({("J1", "1"), ("U1", "1")}),
             "/V": frozenset({("J2", "1"), ("U2", "1")})},
            {("J1", "1"): point("10.16", "15.24"),
             ("U1", "1"): point("20.32", "15.24"),
             ("J2", "1"): point("15.24", "17.78"),
             ("U2", "1"): point("15.24", "20.32")},
            [(point("10.16", "15.24"), point("20.32", "15.24")),
             (point("15.24", "15.24"), point("15.24", "20.32"))],
            set(), [], [],
        )

    def test_ignores_hidden_power_labels(self):
        report = validate_connectivity(
            {"+3V3": frozenset({("J1", "1"), ("U1", "1")}),
             "/+5V_ANA": frozenset({("J2", "1"), ("U2", "1")})},
            {("J1", "1"): point("10.16", "10.16"),
             ("U1", "1"): point("20.32", "10.16"),
             ("J2", "1"): point("10.16", "20.32"),
             ("U2", "1"): point("20.32", "20.32")},
            [], set(),
            [Label("global_label", "+3V3", point("10.16", "10.16"), True),
             Label("label", "+5V_ANA", point("10.16", "20.32"), True),
             Label("label", "+5V_ANA", point("20.32", "20.32"), True)],
            [],
        )
        self.assertEqual(report.checked_signal_nets, 0)

    def test_rejects_visible_and_nonlocal_signal_labels(self):
        cases = [
            Label("label", "SIG", point("15.24", "10.16"), False),
            Label("global_label", "SIG", point("15.24", "10.16"), True),
            Label("hierarchical_label", "SIG", point("15.24", "10.16"), True),
        ]
        for label in cases:
            with self.subTest(label=label), self.assertRaisesRegex(
                ValueError, "visible label|forbidden signal label"
            ):
                validate_connectivity(
                    {"/SIG": frozenset({("J1", "1"), ("U1", "2")})},
                    {("J1", "1"): P1, ("U1", "2"): P2},
                    [(P1, P2)], set(), [label], [],
                )

    def test_rejects_bus_objects(self):
        with self.assertRaisesRegex(ValueError, "bus objects are forbidden"):
            validate_connectivity(
                {"/SIG": frozenset({("J1", "1"), ("U1", "2")})},
                {("J1", "1"): P1, ("U1", "2"): P2},
                [(P1, P2)], set(), [], ["bus_entry"],
            )

    def test_rejects_missing_and_spurious_junctions(self):
        branch_wires = [
            (P1, P2),
            (point("15.24", "10.16"), P3),
        ]
        pins = {("J1", "1"): P1, ("U1", "2"): P2, ("U2", "3"): P3}
        nets = {"/SIG": frozenset(pins)}
        with self.assertRaisesRegex(ValueError, "missing junction"):
            validate_connectivity(nets, pins, branch_wires, set(), [], [])
        with self.assertRaisesRegex(ValueError, "spurious junction"):
            validate_connectivity(
                {"/SIG": frozenset({("J1", "1"), ("U1", "2")})},
                {("J1", "1"): P1, ("U1", "2"): P2},
                [(P1, P2)], {point("15.24", "10.16")}, [], [],
            )

    def test_rejects_off_grid_or_diagonal_wire(self):
        with self.assertRaisesRegex(ValueError, "orthogonal.*1.27 mm grid"):
            validate_connectivity(
                {"/SIG": frozenset({("J1", "1"), ("U1", "2")})},
                {("J1", "1"): P1, ("U1", "2"): P2},
                [(P1, point("20.30", "11.43"))], set(), [], [],
            )


if __name__ == "__main__":
    unittest.main()
