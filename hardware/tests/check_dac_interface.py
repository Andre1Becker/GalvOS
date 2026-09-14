#!/usr/bin/env python3
"""Verify exported KiCad connectivity for the v2 DAC voltage domains.

Usage: python hardware/tests/check_dac_interface.py AFTER.xml [BEFORE.xml]
This checks schematic connectivity, not timing, layout or laser safety.
"""

import sys
import xml.etree.ElementTree as ET


def read_netlist(path):
    root = ET.parse(path).getroot()
    components = {
        c.attrib["ref"]: (c.findtext("value"), c.findtext("footprint"))
        for c in root.findall("./components/comp")
    }
    nets = [
        frozenset((n.attrib["ref"], n.attrib["pin"]) for n in net.findall("node"))
        for net in root.findall("./nets/net")
    ]
    by_pin = {pin: net for net in nets for pin in net}
    return components, nets, by_pin


def require(condition, message):
    if not condition:
        raise ValueError(message)


def check(after_path, before_path=None):
    components, nets, pins = read_netlist(after_path)
    translator = "U_DACLV1"
    require(
        components.get(translator) == (
            "SN74LVC8T245PWR", "Package_SO:TSSOP-24_4.4x7.8mm_P0.65mm"
        ), "Wrong or missing translator/package",
    )
    added = {translator, "C_DACLVA1", "C_DACLVB1", "R_DACUNUSED1"}
    signals = [
        ("SCLK", "J1_18", "7", "3", "21", False),
        ("DIN", "J1_17", "8", "4", "20", False),
        ("SYNC", "J1_16", "6", "5", "19", True),
        ("CLR", "J1_19", "5", "6", "18", True),
    ]

    def same(*nodes):
        require(all(n in pins for n in nodes), f"Missing pin in {nodes}")
        require(all(pins[n] == pins[nodes[0]] for n in nodes), f"Disconnected {nodes}")

    def exact(*nodes):
        same(*nodes)
        require(pins[nodes[0]] == frozenset(nodes), f"Unexpected connections at {nodes}")

    v3, v5, ground = ("U1", "J1_1"), ("U2", "9"), ("U2", "3")
    require(len({pins[v3], pins[v5], pins[ground]}) == 3, "Supply domains shorted")
    same(v3, (translator, "1"), (translator, "2"), ("C_DACLVA1", "1"))
    same(v5, (translator, "23"), (translator, "24"), ("C_DACLVB1", "1"), ("R3", "2"))
    same(ground, (translator, "11"), (translator, "12"), (translator, "13"),
         (translator, "22"), ("C_DACLVA1", "2"), ("C_DACLVB1", "2"),
         ("R_DACUNUSED1", "2"))

    old_spi_pairs = []
    for signal, mcu, dac, a_pin, b_pin, idle_high in signals:
        series, a_bias = f"R_DAC{signal}1", f"R_DAC{signal}A1"
        b_bias = "R3" if signal == "CLR" else f"R_DAC{signal}B1"
        added.update((series, a_bias))
        if b_bias != "R3":
            added.add(b_bias)
        require(components.get(series) == ("22", "Resistor_SMD:R_1206_3216Metric"),
                f"Wrong source termination {series}")
        a_signal, a_rail = ("2", "1") if idle_high else ("1", "2")
        b_signal, b_rail = ("2", "1") if idle_high else ("1", "2")
        if signal == "CLR":
            b_signal, b_rail = "1", "2"  # Preserve existing R3 numbering.
        exact(("U1", mcu), (translator, a_pin), (a_bias, a_signal))
        # At the schematic's 270-degree orientation, pad 2 faces the buffer.
        exact((translator, b_pin), (series, "2"))
        exact((series, "1"), ("U2", dac), (b_bias, b_signal))
        same((a_bias, a_rail), v3 if idle_high else ground)
        same((b_bias, b_rail), v5 if idle_high else ground)
        if signal != "CLR":
            old_spi_pairs.append(frozenset((("U1", mcu), ("U2", dac))))

    exact(("R_DACUNUSED1", "1"), *((translator, str(p)) for p in range(7, 11)))
    for p in range(14, 18):
        exact((translator, str(p)))
    for ref in added - {translator}:
        value = "100nF" if ref.startswith("C_") else (
            "22" if any(ref == f"R_DAC{s[0]}1" for s in signals) else "10k"
        )
        require(components[ref][0] == value, f"Wrong value on {ref}")

    if before_path:
        old_components, old_nets, _ = read_netlist(before_path)
        require(set(components) - set(old_components) == added, "Unexpected added components")
        require({r: components.get(r) for r in old_components} == old_components,
                "Existing component value, footprint or reference changed")
        ignored = {("R3", "2")}  # Intentional transfer from 3V3 to DAC AVDD.
        expected = set()
        for net in old_nets:
            if net in old_spi_pairs:
                expected.update(frozenset((p,)) for p in net)
            elif net - ignored:
                expected.add(net - ignored)
        require(all(pair in old_nets for pair in old_spi_pairs), "Unexpected baseline SPI topology")
        actual = {
            frozenset(p for p in net if p[0] not in added and p not in ignored)
            for net in nets
        } - {frozenset()}
        require(actual == expected, "Unrelated original connectivity changed")
    print("PASS: four translated DAC signals, GPIO13 reset, bias, isolation topology and bypass")
    if before_path:
        print("PASS: all original components and unrelated net memberships preserved")


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    try:
        check(*sys.argv[1:])
    except (ValueError, KeyError) as error:
        sys.exit(f"FAIL: {error}")
