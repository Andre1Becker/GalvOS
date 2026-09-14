#!/usr/bin/env python3
"""Check trigger-clamp BOM/package/polarity, NOT timer-input or laser safety.

Usage: python hardware/tests/check_trigger_diodes.py AFTER.xml [V209.xml]
The optional baseline permits only the two diode part-identity corrections.
"""

import sys
import xml.etree.ElementTree as ET

from check_dac_interface import read_netlist, require

DIODES = {"D_TRIGCL_SCAN1": "U_SCAN1", "D_TRIGCL_WD1": "U_WD1"}
PART = ("1N4148W", "Diode_SMD:D_SOD-123")
FIELDS = {
    "Manufacturer": "Diodes Incorporated",
    "MPN": "1N4148W-7-F",
    "Datasheet": "https://www.diodes.com/assets/Datasheets/ds30086.pdf",
}


def check_contract(components, pins):
    ground = pins.get(("U1", "J2_1"))
    require(ground is not None, "Missing MCU power-ground anchor")
    triggers = []
    for ref, timer in DIODES.items():
        require(components.get(ref) == PART, f"{ref}: wrong diode/package")
        trigger = pins.get((timer, "2"))
        require(trigger is not None and trigger != ground,
                f"{ref}: missing or shorted timer trigger")
        require(pins.get((ref, "1")) == trigger,
                f"{ref}: cathode pad 1 must connect to timer trigger")
        require(pins.get((ref, "2")) == ground == pins.get((timer, "1")),
                f"{ref}: anode pad 2 must connect to MCU/timer power ground")
        triggers.append(trigger)
    require(triggers[0] != triggers[1], "Scan and watchdog triggers must be separate")


def check_bom(root):
    for ref in DIODES:
        component = root.find(f"./components/comp[@ref='{ref}']")
        require(component is not None, f"{ref}: missing component")
        symbol = component.find("libsource")
        require(symbol is not None and symbol.get("lib") == "Diode"
                and symbol.get("part") == "1N4148W",
                f"{ref}: wrong symbol/package identity")
        fields = {f.get("name"): f.text for f in component.findall("fields/field")}
        require(all(fields.get(name) == value for name, value in FIELDS.items()),
                f"{ref}: missing or changed manufacturer/MPN/datasheet")
        for pin, function in (("1", "K_1"), ("2", "A_2")):
            node = root.find(f"./nets/net/node[@ref='{ref}'][@pin='{pin}']")
            require(node is not None and node.get("pinfunction") == function,
                    f"{ref}.{pin}: symbol pin polarity changed")


def check(after, before=None):
    components, nets, pins = read_netlist(after)
    check_contract(components, pins)
    check_bom(ET.parse(after).getroot())
    if before:
        old, old_nets, _ = read_netlist(before)
        for ref in DIODES:
            require(old.get(ref) == ("1N4148", PART[1]),
                    f"{ref}: wrong baseline part/package")
            old[ref] = PART
        require(components == old, "Unrelated component change")
        require(set(nets) == set(old_nets), "Electrical net membership changed")


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    try:
        check(*sys.argv[1:])
        print("PASS: trigger-diode BOM/package/polarity; NOT complete input protection")
    except (OSError, ET.ParseError, KeyError, ValueError) as error:
        sys.exit(f"FAIL: {error}")
