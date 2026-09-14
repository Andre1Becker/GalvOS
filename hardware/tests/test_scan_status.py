"""Negative connectivity checks for the scan-status input buffer."""

import unittest

from check_scan_status import ADDED, GROUND, INPUT, OUTPUT, V3, check_contract


class ScanStatusTests(unittest.TestCase):
    def setUp(self):
        self.components = dict(ADDED)
        self.nets = [
            INPUT, OUTPUT, frozenset((("U_SCANLV1", "1"),)),
            frozenset((V3, ("U_SCANLV1", "5"), ("C_SCANLV1", "1"))),
            frozenset((GROUND, ("U_SCAN1", "1"), ("U_SCANLV1", "3"),
                       ("R_SCANIN1", "2"), ("C_SCANLV1", "2"))),
            frozenset((("U_SCAN1", "8"),)),
        ]

    def check(self):
        pins = {pin: net for net in self.nets for pin in net}
        check_contract(self.components, pins)

    def move(self, pin, destination):
        self.nets = [net - {pin} for net in self.nets]
        self.nets = [net | {pin} if destination in net else net for net in self.nets]

    def test_valid(self):
        self.check()

    def test_direct_5v_bypass(self):
        self.move(("U1", "J2_9"), ("U_SCAN1", "3"))
        with self.assertRaises(ValueError):
            self.check()

    def test_5v_buffer_supply(self):
        self.move(("U_SCANLV1", "5"), ("U_SCAN1", "8"))
        with self.assertRaises(ValueError):
            self.check()

    def test_bias_must_be_pulldown(self):
        self.move(("R_SCANIN1", "2"), V3)
        with self.assertRaises(ValueError):
            self.check()

    def test_decoupler_wrong_ground(self):
        self.move(("C_SCANLV1", "2"), V3)
        with self.assertRaises(ValueError):
            self.check()

    def test_missing_bias(self):
        del self.components["R_SCANIN1"]
        with self.assertRaises(ValueError):
            self.check()

    def test_wrong_logic_part(self):
        self.components["U_SCANLV1"] = ("SN74LVC1G14DBVR", ADDED["U_SCANLV1"][1])
        with self.assertRaises(ValueError):
            self.check()

    def test_nc_pin_connected(self):
        self.move(("U_SCANLV1", "1"), V3)
        with self.assertRaises(ValueError):
            self.check()


if __name__ == "__main__":
    unittest.main()
