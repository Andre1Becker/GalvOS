#!/usr/bin/env python3
"""Check the approved v2 sensor-connector and fan-tach corrections.

Usage: python hardware/tests/check_sensor_fan_interfaces.py AFTER.xml [BEFORE.xml]
BEFORE is the pre-correction v2 export, with the DAC translator already fitted.
This verifies exported connectivity/BOM identity, not electrical safety or layout.
"""

import sys
import xml.etree.ElementTree as ET

from check_dac_interface import read_netlist, require


SENSORS = {"U6", "U7", "U8", "U9", "U10"}
SENSOR_VALUE = "DS18B20 connector"
SENSOR_FOOTPRINT = "Connector_JST:JST_XH_B3B-XH-A_1x03_P2.50mm_Vertical"


def check_sensors(root, components, pins):
    for ref in sorted(SENSORS):
        require(components.get(ref) == (SENSOR_VALUE, SENSOR_FOOTPRINT),
                f"{ref}: BOM must identify the fitted sensor connector")
        source = root.find(f"./components/comp[@ref='{ref}']/libsource")
        require(source is not None and source.get("lib") == "Connector_Generic"
                and source.get("part") == "Conn_01x03",
                f"{ref}: use a generic three-pin connector symbol")
        for pin, anchor in (("1", ("U_BUCK1", "1")),
                            ("2", ("U1", "J1_11")),
                            ("3", ("C_DACLVA1", "1"))):
            require(pins[(ref, pin)] == pins[anchor],
                    f"{ref}.{pin}: expected GND / GPIO18 / +3V3 pinout")
        nodes = root.findall(f"./nets/net/node[@ref='{ref}']")
        require(len(nodes) == 3 and all(n.get("pintype") == "passive" for n in nodes),
                f"{ref}: connector terminals must be passive")


def check_fans(root, components, pins):
    for fan, connector, pullup, mcu, gpio, pwm in (
            (1, "J5", "R30", "J2_5", "GPIO2_J2_5", "J1_9"),
            (2, "J7", "R31", "J1_15", "GPIO9_J1_15", "J1_10")):
        require(pins[(connector, "3")] == frozenset(
            ((connector, "3"), (pullup, "2"), ("U1", mcu))),
            f"FAN{fan}_TACH: wrong channel assignment or extra connection")
        node = root.find(f"./nets/net/node[@ref='U1'][@pin='{mcu}']")
        require(node is not None and node.get("pinfunction") == gpio,
                f"FAN{fan}_TACH: unexpected MCU header/GPIO mapping")
        require(components[pullup][0] == "4.7k"
                and pins[(pullup, "1")] == pins[("C_DACLVA1", "1")],
                f"FAN{fan}_TACH: preserve 4.7k pull-up to +3V3")
        require(pins[(connector, "4")] == frozenset(((connector, "4"), ("U1", pwm))),
                f"FAN{fan}_PWM: unrelated wiring changed")


def check_preservation(after_path, before_path):
    components, nets, _ = read_netlist(after_path)
    old_components, old_nets, _ = read_netlist(before_path)
    require(set(components) == set(old_components), "Component reference set changed")
    expected_components = dict(old_components)
    for ref in SENSORS:
        require(old_components[ref] == ("DS18B20", SENSOR_FOOTPRINT),
                f"{ref}: wrong pre-correction baseline")
        expected_components[ref] = (SENSOR_VALUE, SENSOR_FOOTPRINT)
    require(components == expected_components, "Unapproved value/footprint change")
    swapped = {("U1", "J1_15"): ("U1", "J2_5"),
               ("U1", "J2_5"): ("U1", "J1_15")}
    expected_nets = {frozenset(swapped.get(pin, pin) for pin in net) for net in old_nets}
    require(set(nets) == expected_nets, "Unapproved net-membership change")
    before_root, after_root = ET.parse(before_path).getroot(), ET.parse(after_path).getroot()
    for ref in set(components) - SENSORS:
        selector = f"./components/comp[@ref='{ref}']/libsource"
        require(before_root.find(selector).attrib == after_root.find(selector).attrib,
                f"{ref}: unrelated symbol identity changed")


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    try:
        root = ET.parse(sys.argv[1]).getroot()
        components, _, pins = read_netlist(sys.argv[1])
        failures = []
        for test in (check_sensors, check_fans):
            try:
                test(root, components, pins)
                print(f"PASS: {test.__name__}")
            except (ValueError, KeyError) as error:
                failures.append(str(error))
        if len(sys.argv) == 3:
            try:
                check_preservation(sys.argv[1], sys.argv[2])
                print("PASS: only five connector identities and two tach destinations changed")
            except (ValueError, KeyError) as error:
                failures.append(str(error))
        for error in failures:
            print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(bool(failures))
    except (OSError, ET.ParseError, ValueError, KeyError) as error:
        sys.exit(f"FAIL: {error}")
