"""Focused negative controls for the selected SOD-123 trigger diodes."""

import unittest
import xml.etree.ElementTree as ET

from check_trigger_diodes import DIODES, FIELDS, PART, check_bom, check_contract


class TriggerDiodeTests(unittest.TestCase):
    def setUp(self):
        self.components = dict.fromkeys(DIODES, PART)
        ground = frozenset({("U1", "J2_1")} |
                           {(ref, "2") for ref in DIODES} |
                           {(timer, "1") for timer in DIODES.values()})
        self.pins = {pin: ground for pin in ground}
        self.root = ET.Element("export")
        comps = ET.SubElement(self.root, "components")
        nets = ET.SubElement(self.root, "nets")
        for ref, timer in DIODES.items():
            trigger = frozenset(((ref, "1"), (timer, "2")))
            self.pins.update({pin: trigger for pin in trigger})
            comp = ET.SubElement(comps, "comp", ref=ref)
            ET.SubElement(comp, "libsource", lib="Diode", part="1N4148W")
            fields = ET.SubElement(comp, "fields")
            for name, value in FIELDS.items():
                ET.SubElement(fields, "field", name=name).text = value
            net = ET.SubElement(nets, "net")
            for pin, function in (("1", "K_1"), ("2", "A_2")):
                ET.SubElement(net, "node", ref=ref, pin=pin, pinfunction=function)

    def test_accepts_selected_part_and_polarity(self):
        check_contract(self.components, self.pins)
        check_bom(self.root)

    def test_rejects_axial_or_wrong_package_part(self):
        for spec in (("1N4148", PART[1]), (PART[0], "Diode_SMD:D_SOD-323")):
            with self.subTest(spec=spec):
                self.components["D_TRIGCL_SCAN1"] = spec
                with self.assertRaises(ValueError):
                    check_contract(self.components, self.pins)

    def test_rejects_reversed_polarity(self):
        ref = "D_TRIGCL_SCAN1"
        self.pins[(ref, "1")], self.pins[(ref, "2")] = (
            self.pins[(ref, "2")], self.pins[(ref, "1")])
        with self.assertRaises(ValueError):
            check_contract(self.components, self.pins)

    def test_rejects_wrong_anode_ground(self):
        self.pins[("D_TRIGCL_WD1", "2")] = frozenset({("U2", "3")})
        with self.assertRaises(ValueError):
            check_contract(self.components, self.pins)

    def test_rejects_missing_or_wrong_bom_fields(self):
        for name in FIELDS:
            with self.subTest(field=name):
                field = self.root.find(f"./components/comp/fields/field[@name='{name}']")
                original = field.text
                field.text = "wrong"
                with self.assertRaises(ValueError):
                    check_bom(self.root)
                field.text = original

    def test_rejects_old_axial_symbol(self):
        self.root.find("./components/comp/libsource").set("part", "1N4148")
        with self.assertRaises(ValueError):
            check_bom(self.root)

    def test_rejects_symbol_pin_swap(self):
        self.root.find("./nets/net/node").set("pinfunction", "A_1")
        with self.assertRaises(ValueError):
            check_bom(self.root)


if __name__ == "__main__":
    unittest.main()
