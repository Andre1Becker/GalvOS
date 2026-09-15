"""Negative controls for the schematic electrical-preservation gate."""

from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as ET

from check_schematic_preservation import check


class SchematicPreservationTests(unittest.TestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tempdir.cleanup)
        self.root = Path(self.tempdir.name)
        self.serial = 0

    def write_netlist(
        self,
        *,
        source="fixture.kicad_sch",
        value="10k",
        net_nodes=(("U1", "1"),),
        include_nets=True,
        libpart_description="fixture part",
    ):
        export = ET.Element("export")
        design = ET.SubElement(export, "design")
        ET.SubElement(design, "source").text = source

        components = ET.SubElement(export, "components")
        component = ET.SubElement(components, "comp", ref="R1")
        ET.SubElement(component, "value").text = value
        ET.SubElement(component, "footprint").text = "Resistor_SMD:R_1206"

        libparts = ET.SubElement(export, "libparts")
        libpart = ET.SubElement(libparts, "libpart", lib="Device", part="R")
        ET.SubElement(libpart, "description").text = libpart_description

        libraries = ET.SubElement(export, "libraries")
        library = ET.SubElement(libraries, "library", logical="Device")
        ET.SubElement(library, "uri").text = "${KICAD10_SYMBOL_DIR}/Device.kicad_sym"

        if include_nets:
            nets = ET.SubElement(export, "nets")
            net = ET.SubElement(nets, "net", code="1", name="/SIGNAL")
            for ref, pin in net_nodes:
                ET.SubElement(net, "node", ref=ref, pin=pin)

        self.serial += 1
        path = self.root / f"netlist-{self.serial}.xml"
        ET.ElementTree(export).write(path, encoding="utf-8", xml_declaration=True)
        return path

    def test_accepts_design_metadata_only_change(self):
        before = self.write_netlist(source="before.kicad_sch")
        after = self.write_netlist(source="after.kicad_sch")

        check(before, after)

    def test_rejects_component_property_change(self):
        before = self.write_netlist(value="10k")
        after = self.write_netlist(value="22k")

        with self.assertRaisesRegex(ValueError, "components"):
            check(before, after)

    def test_rejects_pin_membership_change(self):
        before = self.write_netlist(net_nodes=(("U1", "1"),))
        after = self.write_netlist(net_nodes=(("U1", "2"),))

        with self.assertRaisesRegex(ValueError, "nets"):
            check(before, after)

    def test_rejects_library_part_change(self):
        before = self.write_netlist(libpart_description="fixture part")
        after = self.write_netlist(libpart_description="different part")

        with self.assertRaisesRegex(ValueError, "libparts"):
            check(before, after)

    def test_rejects_missing_required_section(self):
        before = self.write_netlist()
        after = self.write_netlist(include_nets=False)

        with self.assertRaisesRegex(ValueError, "nets"):
            check(before, after)


if __name__ == "__main__":
    unittest.main()
