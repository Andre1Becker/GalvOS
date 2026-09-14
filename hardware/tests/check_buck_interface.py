#!/usr/bin/env python3
"""Check buck connectivity and bypass requirements, not output-current rating.

Usage: python hardware/tests/check_buck_interface.py AFTER.xml [V205.xml]
"""

import sys
import xml.etree.ElementTree as ET

from check_dac_interface import read_netlist, require


CAPS = {"C_INHF1": ("220nF", "50V"), "C_BOOT1": ("100nF", "16V"),
        "C_VCC1": ("1uF", "16V")}
EXACT = [
    frozenset((("U_BUCK1", "5"), ("R_FB1", "2"), ("R_FB2", "1"))),
    frozenset((("U_BUCK1", "7"), ("C_BOOT1", "2"))),
    frozenset((("U_BUCK1", "8"), ("C_BOOT1", "1"), ("L1", "1"))),
    frozenset((("U_BUCK1", "6"), ("C_VCC1", "1"))),
    frozenset((("U_BUCK1", "4"),)),
]
GROUND = ("U_BUCK1", "1")
VIN = ("U_BUCK1", "2")
VOUT = ("L1", "2")
MEMBERS = {
    GROUND: (("U_BUCK1", "9"), ("C_INHF1", "2"), ("C_VCC1", "2"),
             ("R_FB2", "2"), ("C_IN1", "2"), ("C_IN2", "2"),
             ("C_OUT1", "2"), ("C_OUT2", "2"), ("U1", "J2_1")),
    VIN: (("C_INHF1", "1"), ("C_IN1", "1"), ("C_IN2", "1"), ("D2", "1")),
    VOUT: (("R_FB1", "1"), ("C_OUT1", "1"), ("C_OUT2", "1"), ("D1", "2")),
}


def check_contract(components, pins):
    require(components.get("U_BUCK1") == (
        "LMR33630ADDA", "Package_SO:Texas_HSOP-8-1EP_3.9x4.9mm_P1.27mm_ThermalVias"),
        "Wrong buck part/package")
    for ref, (value, _) in CAPS.items():
        require(components.get(ref) == (value, "Capacitor_SMD:C_1206_3216Metric"),
                f"{ref}: wrong bypass value/package")
    for ref, value in (("R_FB1", "100k 1%"), ("R_FB2", "24.9k 1%")):
        require(components.get(ref) == (value, "Resistor_SMD:R_1206_3216Metric"),
                f"{ref}: changed feedback divider")
    for net in EXACT:
        require(all(pins.get(pin) == net for pin in net), "Wrong buck local net")
    for anchor, members in MEMBERS.items():
        require(anchor in pins and all(pins.get(pin) == pins[anchor] for pin in members),
                f"Wrong buck power membership at {anchor}")
    require(len({pins[GROUND], pins[VIN], pins[VOUT]}) == 3,
            "Buck VIN, VOUT and ground must remain distinct")
    require(pins[GROUND] != pins[("U2", "3")], "Do not merge AGND into power ground")


def check_bom(root):
    for ref, (_, voltage) in CAPS.items():
        fields = {f.get("name"): f.text for f in
                  root.findall(f"./components/comp[@ref='{ref}']/fields/field")}
        require(all(fields.get(k) == v for k, v in
                    {"Voltage": voltage, "Dielectric": "X7R", "Tolerance": "10%"}.items()),
                f"{ref}: missing or changed ceramic requirements")


def check(after, before=None):
    components, nets, pins = read_netlist(after)
    check_contract(components, pins)
    check_bom(ET.parse(after).getroot())
    if before:
        old, old_nets, _ = read_netlist(before)
        require(old.get("C_INHF1") == ("100nF", "Capacitor_SMD:C_1206_3216Metric"),
                "Wrong baseline: expected V2.0.5 HF bypass")
        old["C_INHF1"] = ("220nF", "Capacitor_SMD:C_1206_3216Metric")
        require(components == old, "Unrelated component value/package change")
        require(set(nets) == set(old_nets), "Electrical net membership changed")


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    try:
        check(*sys.argv[1:])
        print("PASS: buck connectivity/bypass requirements; no power-rating claim")
    except (OSError, ET.ParseError, KeyError, ValueError) as error:
        sys.exit(f"FAIL: {error}")
