"""Negative connectivity checks for the DMX receive input buffer."""

import unittest

from check_dmx_interface import ADDED, GROUND, INPUT, OUTPUT, V3, check_contract


class DmxInterfaceTests(unittest.TestCase):
    def setUp(self):
        self.components = dict(ADDED)
        self.nets = [
            INPUT, OUTPUT, frozenset((("U_DMXLV1", "1"),)),
            frozenset((V3, ("U_DMXLV1", "5"), ("C_DMXLV1", "1"))),
            frozenset((GROUND, ("J_DMX1", "2"), ("U_DMXLV1", "3"),
                       ("R_DMXIN1", "2"), ("C_DMXLV1", "2"))),
            frozenset((("J_DMX1", "1"), ("U_SCAN1", "8"))),
            frozenset((("J_DMX1", "4"),)),
            frozenset((("U2", "3"),)),
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
        self.move(("U1", "J1_4"), ("J_DMX1", "3"))
        with self.assertRaises(ValueError):
            self.check()

    def test_5v_buffer_supply(self):
        self.move(("U_DMXLV1", "5"), ("J_DMX1", "1"))
        with self.assertRaises(ValueError):
            self.check()

    def test_bias_must_be_pulldown(self):
        self.move(("R_DMXIN1", "2"), V3)
        with self.assertRaises(ValueError):
            self.check()

    def test_decoupler_wrong_ground(self):
        self.move(("C_DMXLV1", "2"), V3)
        with self.assertRaises(ValueError):
            self.check()

    def test_missing_bias(self):
        del self.components["R_DMXIN1"]
        with self.assertRaises(ValueError):
            self.check()

    def test_wrong_logic_part(self):
        self.components["U_DMXLV1"] = ("SN74LVC1G14DBVR", ADDED["U_DMXLV1"][1])
        with self.assertRaises(ValueError):
            self.check()

    def test_nc_pin_connected(self):
        self.move(("U_DMXLV1", "1"), V3)
        with self.assertRaises(ValueError):
            self.check()

    def test_connector_supply_swapped(self):
        self.move(("J_DMX1", "1"), GROUND)
        with self.assertRaises(ValueError):
            self.check()

    def test_connector_nc_connected(self):
        self.move(("J_DMX1", "4"), GROUND)
        with self.assertRaises(ValueError):
            self.check()

    def test_analog_ground_joined(self):
        self.move(("U2", "3"), GROUND)
        with self.assertRaises(ValueError):
            self.check()


if __name__ == "__main__":
    unittest.main()
