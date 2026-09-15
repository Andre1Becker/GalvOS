#!/usr/bin/env python3
"""Prove a drawing-only schematic edit preserved its KiCad XML netlist."""

from pathlib import Path
import sys
import xml.etree.ElementTree as ET


SECTIONS = ("components", "libparts", "libraries", "nets")


def snapshot(path: Path) -> dict[str, bytes]:
    root = ET.parse(path).getroot()
    sections = {}
    for name in SECTIONS:
        section = root.find(f"./{name}")
        if section is None:
            raise ValueError(f"Missing required netlist section: {name}")
        sections[name] = ET.tostring(section, encoding="utf-8")
    return sections


def check(before: Path, after: Path) -> None:
    old = snapshot(Path(before))
    new = snapshot(Path(after))
    changed = [name for name in SECTIONS if old[name] != new[name]]
    if changed:
        raise ValueError(f"Electrical netlist changed: {', '.join(changed)}")


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(f"Usage: {argv[0]} BEFORE.xml AFTER.xml", file=sys.stderr)
        return 2

    try:
        check(Path(argv[1]), Path(argv[2]))
    except (ET.ParseError, OSError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1

    print("PASS: electrical components, libraries and nets are identical")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
