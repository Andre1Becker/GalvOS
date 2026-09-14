#!/usr/bin/env python3
"""Check v2 DMX receive voltage-domain connectivity, not laser safety.

Usage: python hardware/tests/check_dmx_interface.py AFTER.xml [BEFORE.xml]
BEFORE is the v2.0.4 export, before insertion of the input buffer.
"""

import sys
import xml.etree.ElementTree as ET

from check_dac_interface import read_netlist, require


ADDED = {
    "U_DMXLV1": ("SN74LVC1G17DBVR", "Package_TO_SOT_SMD:SOT-23-5"),
    "R_DMXIN1": ("10k", "Resistor_SMD:R_1206_3216Metric"),
    "C_DMXLV1": ("100nF", "Capacitor_SMD:C_1206_3216Metric"),
}
INPUT = frozenset((("J_DMX1", "3"), ("U_DMXLV1", "2"), ("R_DMXIN1", "1")))
OUTPUT = frozenset((("U_DMXLV1", "4"), ("U1", "J1_4")))
OLD_RO = frozenset((("J_DMX1", "3"), ("U1", "J1_4")))
V3 = ("U1", "J1_1")
GROUND = ("U1", "J2_1")
SUPPLY_ADDITIONS = {
    V3: {("U_DMXLV1", "5"), ("C_DMXLV1", "1")},
    GROUND: {("U_DMXLV1", "3"), ("R_DMXIN1", "2"), ("C_DMXLV1", "2")},
}


def check_contract(components, pins):
    for ref, spec in ADDED.items():
        require(components.get(ref) == spec, f"{ref}: wrong or missing part/package")
    for expected in (INPUT, OUTPUT, frozenset((("U_DMXLV1", "1"),)),
                     frozenset((("J_DMX1", "4"),))):
        require(all(pins.get(pin) == expected for pin in expected),
                f"Wrong DMX connectivity: {sorted(expected)}")
    for anchor, extra in SUPPLY_ADDITIONS.items():
        require(anchor in pins and all(pins.get(pin) == pins[anchor] for pin in extra),
                f"Wrong buffer supply/bias connections at {anchor}")
    require(len({pins[V3], pins[GROUND], pins[("J_DMX1", "1")]}) == 3,
            "MCU 3V3, power ground and module 5V must remain separate")
    require(pins[GROUND] == pins[("J_DMX1", "2")],
            "Use MCU/module power ground, not the analog side of R26")

    require(pins[("J_DMX1", "1")] == pins[("U_SCAN1", "8")],
            "DMX connector pin 1 must retain timer/buck 5V supply")
    require(pins[GROUND] != pins[("U2", "3")],
            "Power ground and analog ground must remain distinct")


def check_bom(root):
    source = root.find("./components/comp[@ref='U_DMXLV1']/libsource")
    require(source is not None and source.get("lib") == "74xGxx"
            and source.get("part") == "74LVC1G17", "Wrong buffer symbol/polarity")
    for ref, fields in {
        "U_DMXLV1": {"MPN": "SN74LVC1G17DBVR", "Manufacturer": "Texas Instruments"},
        "R_DMXIN1": {"Tolerance": "1%", "Power": "0.25W"},
        "C_DMXLV1": {"Voltage": "50V", "Dielectric": "X7R", "Tolerance": "10%"},
    }.items():
        actual = {f.get("name"): f.text for f in
                  root.findall(f"./components/comp[@ref='{ref}']/fields/field")}
        require(all(actual.get(k) == v for k, v in fields.items()),
                f"{ref}: missing or changed BOM requirements")
    pin = root.find("./nets/net/node[@ref='U1'][@pin='J1_4']")
    require(pin is not None and pin.get("pinfunction") == "GPIO4_J1_4",
            "Wrong ESP32 GPIO/header mapping")


def check(after, before=None):
    components, nets, pins = read_netlist(after)
    check_contract(components, pins)
    check_bom(ET.parse(after).getroot())
    if before is not None:
        old_components, old_nets, old_pins = read_netlist(before)
        require(not (set(old_components) & set(ADDED)), "Wrong baseline: buffer already present")
        require(components == old_components | ADDED, "Unrelated component change")
        require(OLD_RO in old_nets, "Wrong baseline: expected direct module/MCU net")
        expected = set(old_nets) - {OLD_RO}
        for anchor, added in SUPPLY_ADDITIONS.items():
            expected.remove(old_pins[anchor])
            expected.add(old_pins[anchor] | added)
        expected.update((INPUT, OUTPUT, frozenset((("U_DMXLV1", "1"),))))
        require(set(nets) == expected, "Unrelated net membership changed")


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    try:
        check(*sys.argv[1:])
        print("PASS: DMX receive buffer connectivity, supplies, bias and BOM")
        if len(sys.argv) == 3:
            print("PASS: only the approved buffer stage changes the baseline netlist")
    except (OSError, ET.ParseError, KeyError, ValueError) as error:
        sys.exit(f"FAIL: {error}")
