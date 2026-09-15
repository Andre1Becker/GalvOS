#!/usr/bin/env python3
"""Verify that copied KiCad signal nets are represented by visible wires."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from decimal import Decimal
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from hardware.tools.kicad_schematic_geometry import (
    PinRef,
    Point,
    graph_for,
    parse_document,
    parse_sexpr,
    pin_endpoints,
)


GRID = Decimal("1.27")
GRID_TOLERANCE = Decimal("0.001")
POWER_NET_NAMES = frozenset(
    {
        "+3V3",
        "+15V",
        "-15V",
        "/+5V_ANA",
        "/+5V_MCU",
        "/FAN1_12V",
        "/FAN2_12V",
        "/VCC_BUCK",
        "/VIN_BUCK",
        "5V GND Buck",
        "AGND (from Galvo Board)",
        "Buck +5V",
        "VIN_RAW",
    }
)


@dataclass(frozen=True)
class Label:
    kind: str
    text: str
    point: Point
    hidden: bool


@dataclass(frozen=True)
class ConnectivityReport:
    checked_signal_nets: int
    connected_graphs: int
    junctions: int
    hidden_labels: int


def _on_grid(point: Point) -> bool:
    for coordinate in point:
        units = coordinate / GRID
        if abs(units - units.to_integral_value()) > GRID_TOLERANCE:
            return False
    return True


def normalize_local_label(text: str) -> str:
    return text if text.startswith("/") else f"/{text}"


def _wire_axis(start: Point, end: Point) -> str | None:
    if start[1] == end[1]:
        return "h"
    if start[0] == end[0]:
        return "v"
    return None


def validate_connectivity(
    baseline_nets: dict[str, frozenset[PinRef]],
    pin_points: dict[PinRef, Point],
    wires: list[tuple[Point, Point]],
    junctions: set[Point],
    labels: list[Label],
    forbidden_objects: list[str],
) -> ConnectivityReport:
    """Validate the visible-wire contract and return evidence counts."""
    errors: list[str] = []
    bad_geometry = False
    for start, end in wires:
        if _wire_axis(start, end) is None or not _on_grid(start) or not _on_grid(end):
            bad_geometry = True
    if any(not _on_grid(point) for point in junctions):
        bad_geometry = True
    if bad_geometry:
        errors.append("wires must be orthogonal and on the 1.27 mm grid")
    if forbidden_objects:
        errors.append("bus objects are forbidden: " + ", ".join(sorted(set(forbidden_objects))))

    graph = None
    if not bad_geometry:
        graph = graph_for(
            [((str(a[0]), str(a[1])), (str(b[0]), str(b[1]))) for a, b in wires],
            [(str(point[0]), str(point[1])) for point in junctions],
            [
                (str(point[0]), str(point[1]))
                for point in set(pin_points.values()) | {label.point for label in labels}
            ],
        )

    pin_components: dict[PinRef, tuple[Point, str]] = {}
    if graph is not None:
        for pin, point in pin_points.items():
            try:
                pin_components[pin] = graph.component(point)
            except (KeyError, ValueError):
                continue

    checked = 0
    component_nets: dict[tuple[Point, str], set[str]] = {}
    for net_name, pins in baseline_nets.items():
        if len(pins) < 2 or net_name in POWER_NET_NAMES or net_name.startswith("unconnected-("):
            continue
        checked += 1
        components = {pin_components[pin] for pin in pins if pin in pin_components}
        missing = sorted(pins - pin_components.keys())
        if missing or len(components) != 1:
            detail = f"; missing pins {missing}" if missing else ""
            errors.append(f"{net_name}: not continuous{detail}")
        for component in components:
            component_nets.setdefault(component, set()).add(net_name)

    for names in component_nets.values():
        if len(names) > 1:
            errors.append("wire graph joins baseline nets " + ", ".join(sorted(names)))

    label_components: dict[str, list[tuple[Point, str]]] = {}
    for label in labels:
        if not label.hidden:
            errors.append(f"{label.kind} {label.text!r}: visible label is forbidden")
        if label.kind in {"global_label", "hierarchical_label"}:
            errors.append(f"{label.kind} {label.text!r}: forbidden signal label")
        if label.kind != "label" or graph is None:
            continue
        baseline_name = normalize_local_label(label.text)
        if baseline_name not in baseline_nets:
            errors.append(f"hidden local label {label.text!r}: no locked baseline net")
            continue
        try:
            component = graph.component(label.point)
        except (KeyError, ValueError):
            errors.append(f"hidden local label {label.text!r}: not attached to one wire graph")
            continue
        label_components.setdefault(baseline_name, []).append(component)

    for name, components in label_components.items():
        if len(set(components)) > 1:
            errors.append(f"{name}: disconnected labels")
        elif len(components) > 1:
            errors.append(f"{name}: duplicate hidden labels on one graph")

    if graph is not None:
        candidate_points = {
            point for start, end in wires for point in (start, end)
        } | junctions
        for point in sorted(candidate_points):
            try:
                degree = graph.degree(point)
            except (KeyError, ValueError):
                continue
            if degree >= 3 and point not in junctions:
                errors.append(f"missing junction at {point}")
            elif degree < 3 and point in junctions:
                errors.append(f"spurious junction at {point}")

    if errors:
        raise ValueError("; ".join(errors))

    roots = set()
    if graph is not None:
        roots = {graph.union.find(node) for node in graph.union.parent}
    return ConnectivityReport(checked, len(roots), len(junctions), sum(label.hidden for label in labels))


def read_baseline(path: Path) -> dict[str, frozenset[PinRef]]:
    root = ET.parse(path).getroot()
    nets: dict[str, frozenset[PinRef]] = {}
    for net in root.findall("./nets/net"):
        name = net.attrib["name"]
        nets[name] = frozenset((node.attrib["ref"], node.attrib["pin"]) for node in net.findall("node"))
    return nets


def _child(node, kind: str):
    for child in node[1:]:
        if isinstance(child, list) and child and child[0] == kind:
            return child
    return None


def _point_from(node) -> Point:
    return Decimal(str(node[1])), Decimal(str(node[2]))


def read_drawing(path: Path):
    document = parse_document(path.read_text(encoding="utf-8"))
    pins = pin_endpoints(document)
    wires: list[tuple[Point, Point]] = []
    junctions: set[Point] = set()
    labels: list[Label] = []
    forbidden: list[str] = []
    for obj in document.objects:
        if obj.kind == "wire":
            node = parse_sexpr(obj.source)
            pts = _child(node, "pts")
            if pts is None:
                continue
            points = [_point_from(child) for child in pts[1:] if isinstance(child, list) and child and child[0] == "xy"]
            wires.extend(zip(points, points[1:]))
        elif obj.kind == "junction":
            node = parse_sexpr(obj.source)
            at = _child(node, "at")
            if at is not None:
                junctions.add(_point_from(at))
        elif obj.kind in {"label", "global_label", "hierarchical_label"}:
            node = parse_sexpr(obj.source)
            at = _child(node, "at")
            if at is None or len(node) < 2:
                continue
            labels.append(Label(obj.kind, str(node[1]), _point_from(at), "(hide yes)" in obj.source))
        elif obj.kind in {"bus", "bus_entry", "bus_alias", "hierarchical_sheet"}:
            forbidden.append(obj.kind)
    return pins, wires, junctions, labels, forbidden


def check_visible_connectivity(baseline: Path, schematic: Path) -> ConnectivityReport:
    nets = read_baseline(baseline)
    pins, wires, junctions, labels, forbidden = read_drawing(schematic)
    return validate_connectivity(nets, pins, wires, junctions, labels, forbidden)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("schematic", type=Path)
    args = parser.parse_args(argv[1:])
    try:
        report = check_visible_connectivity(args.baseline, args.schematic)
    except (ET.ParseError, OSError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print(
        "PASS: "
        f"{report.checked_signal_nets} signal nets, "
        f"{report.connected_graphs} wire graphs, "
        f"{report.junctions} junctions, "
        f"{report.hidden_labels} hidden labels"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
