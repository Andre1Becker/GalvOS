#!/usr/bin/env python3
"""Check v2 external 12 V fan supplies, isolated from the buck positive rails.

Usage: python hardware/tests/check_fan_power.py AFTER.xml [V2_0_1_BEFORE.xml]
This is a connectivity check, not a voltage/current rating or safety approval.
"""

import sys
import xml.etree.ElementTree as ET

from check_dac_interface import read_netlist, require


INPUT_VALUES = {"J2": "12.6V IN (BUCK)", "J4": "FAN1 12V IN", "J6": "FAN2 12V IN"}
FAN_NETS = {frozenset((("J4", "1"), ("J5", "2"))),
            frozenset((("J6", "1"), ("J7", "2")))}


def check(path, before_path=None):
    components, nets, pins = read_netlist(path)
    for net in FAN_NETS:
        require(pins[next(iter(net))] == net,
                "Fan positive supply must join only its input and fan header")
        require(net != pins[("L1", "2")] and net != pins[("J2", "1")],
                "Fan positive supply must be separate from buck input/output")
    for ref in ("J4", "J5", "J6", "J7"):
        ground_pin = "2" if ref in ("J4", "J6") else "1"
        require(pins[(ref, ground_pin)] == pins[("U_BUCK1", "1")],
                f"{ref}: preserve common power ground")
    for ref, value in INPUT_VALUES.items():
        require(components[ref][0] == value, f"{ref}: input voltage identification missing")
    print("PASS: two separate external 12 V fan branches; common power ground")

    if before_path:
        old_components, old_nets, old_pins = read_netlist(before_path)
        expected_components = dict(old_components)
        for ref, value in INPUT_VALUES.items():
            expected_components[ref] = (value, old_components[ref][1])
        require(components == expected_components, "Unexpected component/value/footprint change")
        detached = frozenset().union(*FAN_NETS)
        require(detached <= old_pins[("L1", "2")], "Wrong baseline fan supply topology")
        expected_nets = {net - detached for net in old_nets if net - detached} | FAN_NETS
        require(set(nets) == expected_nets, "Unexpected net-membership change")
        print("PASS: only approved input values and four fan power-pin memberships changed")


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    try:
        check(*sys.argv[1:])
    except (OSError, ET.ParseError, ValueError, KeyError) as error:
        sys.exit(f"FAIL: {error}")
