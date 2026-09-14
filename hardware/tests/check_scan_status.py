#!/usr/bin/env python3
"""Check v2 scan-status voltage-domain connectivity, not laser safety.

Usage: python hardware/tests/check_scan_status.py AFTER.xml [BEFORE.xml]
BEFORE is the v2.0.3 export, before insertion of the input buffer.
"""

import sys
import xml.etree.ElementTree as ET

from check_dac_interface import read_netlist, require


ADDED = {
    "U_SCANLV1": ("SN74LVC1G17DBVR", "Package_TO_SOT_SMD:SOT-23-5"),
    "R_SCANIN1": ("10k", "Resistor_SMD:R_1206_3216Metric"),
    "C_SCANLV1": ("100nF", "Capacitor_SMD:C_1206_3216Metric"),
}
INPUT = frozenset((("U_SCAN1", "3"), ("U_SCANLV1", "2"), ("R_SCANIN1", "1")))
OUTPUT = frozenset((("U_SCANLV1", "4"), ("U1", "J2_9")))
OLD_STATUS = frozenset((("U_SCAN1", "3"), ("U1", "J2_9")))
V3 = ("U1", "J1_1")
GROUND = ("U1", "J2_1")
SUPPLY_ADDITIONS = {
    V3: {("U_SCANLV1", "5"), ("C_SCANLV1", "1")},
    GROUND: {("U_SCANLV1", "3"), ("R_SCANIN1", "2"), ("C_SCANLV1", "2")},
}


def check_contract(components, pins):
    for ref, spec in ADDED.items():
        require(components.get(ref) == spec, f"{ref}: wrong or missing part/package")
    for expected in (INPUT, OUTPUT, frozenset((("U_SCANLV1", "1"),))):
        require(all(pins.get(pin) == expected for pin in expected),
                f"Wrong status connectivity: {sorted(expected)}")
    for anchor, extra in SUPPLY_ADDITIONS.items():
        require(anchor in pins and all(pins.get(pin) == pins[anchor] for pin in extra),
                f"Wrong buffer supply/bias connections at {anchor}")
    require(len({pins[V3], pins[GROUND], pins[("U_SCAN1", "8")]}) == 3,
            "MCU 3V3, power ground and timer 5V must remain separate")
    require(pins[GROUND] == pins[("U_SCAN1", "1")],
            "Use MCU/timer power ground, not the analog side of R26")


def check_bom(root):
    source = root.find("./components/comp[@ref='U_SCANLV1']/libsource")
    require(source is not None and source.get("lib") == "74xGxx"
            and source.get("part") == "74LVC1G17", "Wrong buffer symbol/polarity")
    for ref, fields in {
        "U_SCANLV1": {"MPN": "SN74LVC1G17DBVR", "Manufacturer": "Texas Instruments"},
        "R_SCANIN1": {"Tolerance": "1%", "Power": "0.25W"},
        "C_SCANLV1": {"Voltage": "50V", "Dielectric": "X7R", "Tolerance": "10%"},
    }.items():
        actual = {f.get("name"): f.text for f in
                  root.findall(f"./components/comp[@ref='{ref}']/fields/field")}
        require(all(actual.get(k) == v for k, v in fields.items()),
                f"{ref}: missing or changed BOM requirements")
    pin = root.find("./nets/net/node[@ref='U1'][@pin='J2_9']")
    require(pin is not None and pin.get("pinfunction") == "GPIO39_J2_9",
            "Wrong ESP32 GPIO/header mapping")


def check(after, before=None):
    components, nets, pins = read_netlist(after)
    check_contract(components, pins)
    check_bom(ET.parse(after).getroot())
    if before is not None:
        old_components, old_nets, old_pins = read_netlist(before)
        require(not (set(old_components) & set(ADDED)), "Wrong baseline: buffer already present")
        require(components == old_components | ADDED, "Unrelated component change")
        require(OLD_STATUS in old_nets, "Wrong baseline: expected direct timer/MCU net")
        expected = set(old_nets) - {OLD_STATUS}
        for anchor, added in SUPPLY_ADDITIONS.items():
            expected.remove(old_pins[anchor])
            expected.add(old_pins[anchor] | added)
        expected.update((INPUT, OUTPUT, frozenset((("U_SCANLV1", "1"),))))
        require(set(nets) == expected, "Unrelated net membership changed")


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    try:
        check(*sys.argv[1:])
        print("PASS: scan-status buffer connectivity, supplies, bias and BOM")
        if len(sys.argv) == 3:
            print("PASS: only the approved buffer stage changes the baseline netlist")
    except (OSError, ET.ParseError, KeyError, ValueError) as error:
        sys.exit(f"FAIL: {error}")
