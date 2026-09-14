"""Regression guards for the buck's feedback and local bypass connections."""

import unittest
import xml.etree.ElementTree as ET

from check_buck_interface import CAPS, EXACT, GROUND, MEMBERS, VIN, VOUT
from check_buck_interface import check_bom, check_contract


class BuckInterfaceTests(unittest.TestCase):
    def setUp(self):
        self.components = {
            "U_BUCK1": ("LMR33630ADDA", "Package_SO:Texas_HSOP-8-1EP_3.9x4.9mm_P1.27mm_ThermalVias"),
            "R_FB1": ("100k 1%", "Resistor_SMD:R_1206_3216Metric"),
            "R_FB2": ("24.9k 1%", "Resistor_SMD:R_1206_3216Metric"),
        }
        self.components.update({r: (v, "Capacitor_SMD:C_1206_3216Metric")
                                for r, (v, _) in CAPS.items()})
        self.nets = list(EXACT) + [frozenset((anchor,) + members)
                                  for anchor, members in MEMBERS.items()]
        self.nets.append(frozenset((("U2", "3"),)))

    def check(self):
        check_contract(self.components, {pin: net for net in self.nets for pin in net})

    def move(self, pin, destination):
        self.nets = [net - {pin} for net in self.nets]
        self.nets = [net | {pin} if destination in net else net for net in self.nets]

    def test_valid(self):
        self.check()

    def test_old_hf_bypass(self):
        self.components["C_INHF1"] = ("100nF", self.components["C_INHF1"][1])
        with self.assertRaises(ValueError):
            self.check()

    def test_wrong_feedback_value(self):
        self.components["R_FB2"] = ("10k 1%", self.components["R_FB2"][1])
        with self.assertRaises(ValueError):
            self.check()

    def test_wrong_power_connections(self):
        for pin, destination in [(("C_INHF1", "1"), VOUT),
                                 (("C_BOOT1", "1"), GROUND),
                                 (("C_VCC1", "1"), VIN),
                                 (("R_FB2", "2"), ("U2", "3")),
                                 (("U_BUCK1", "9"), VOUT)]:
            with self.subTest(pin=pin):
                self.setUp()
                self.move(pin, destination)
                with self.assertRaises(ValueError):
                    self.check()

    def test_ceramic_requirements(self):
        root = ET.Element("export")
        comps = ET.SubElement(root, "components")
        for ref, (_, voltage) in CAPS.items():
            fields = ET.SubElement(ET.SubElement(comps, "comp", ref=ref), "fields")
            for name, value in {"Voltage": voltage, "Dielectric": "X7R", "Tolerance": "10%"}.items():
                ET.SubElement(fields, "field", name=name).text = value
        check_bom(root)
        root.find("./components/comp[@ref='C_INHF1']/fields/field[@name='Voltage']").text = "10V"
        with self.assertRaises(ValueError):
            check_bom(root)


if __name__ == "__main__":
    unittest.main()
