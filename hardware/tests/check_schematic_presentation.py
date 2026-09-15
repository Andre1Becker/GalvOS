#!/usr/bin/env python3
"""Check the drawing contract for the canonical GalvOS V2 schematic."""

from argparse import ArgumentParser
from dataclasses import dataclass
from decimal import Decimal
from pathlib import Path
import re
import sys

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from hardware.tools.kicad_schematic_geometry import (
    segment_intersects_box,
    text_field_box,
)


GRID = Decimal("1.27")
GRID_TOLERANCE = Decimal("0.001")
VISIBLE_COPY_DX = Decimal("100.33")
VISIBLE_COPY_DY = Decimal("76.20")
NUMBER = r"[-+]?\d+(?:\.\d+)?"
AT_RE = re.compile(rf"\(at\s+({NUMBER})\s+({NUMBER})(?:\s+({NUMBER}))?")
XY_RE = re.compile(rf"\(xy\s+({NUMBER})\s+({NUMBER})\)")
PROPERTY_RE = re.compile(r'^\(property\s+"([^"]+)"\s+"([^"]*)"')
LIB_ID_RE = re.compile(r'^\(lib_id\s+"([^"]+)"')
FONT_SIZE_RE = re.compile(rf"\(size\s+({NUMBER})\s+({NUMBER})\)")

COLUMNS = {
    "input": (Decimal("25.40"), Decimal("139.70")),
    "process": (Decimal("165.10"), Decimal("406.40")),
    "output": (Decimal("431.80"), Decimal("571.50")),
}
ROWS = {
    "power": (Decimal("25.40"), Decimal("88.90")),
    "io": (Decimal("101.60"), Decimal("165.10")),
    "analog": (Decimal("177.80"), Decimal("241.30")),
    "laser": (Decimal("254.00"), Decimal("309.88")),
    "safety": (Decimal("317.50"), Decimal("381.00")),
}
BLOCK_PLACEMENT = {
    "power": {
        "input": ("J2", "J3", "J4", "J6"),
        "process": ("D1", "D2", "FB1", "U_BUCK1"),
        "output": ("L1",),
    },
    "io": {
        "input": ("J1", "J_DMX1", "U6", "U7", "U8", "U9", "U10"),
        "process": ("U_DMXLV1", "U1"),
        "output": ("J5", "J7"),
    },
    "analog": {
        "process": ("U_DACLV1", "U2", "U12"),
        "output": ("U13",),
    },
    "laser": {
        "process": ("U15", "U16", "U17"),
        "output": ("U4", "U5", "U3"),
    },
    "safety": {
        "input": ("J_ESTOP1",),
        "process": ("U_WD1", "U_SCAN1", "U_SCANLV1"),
        "output": ("J_SSR1",),
    },
}
CHAINS = {
    "DMX": ("J_DMX1", "U_DMXLV1", "U1"),
    "DAC": ("U1", "U_DACLV1", "U2", "U12", "U13"),
    "LASER_RED": ("U1", "U15", "U4"),
    "LASER_GREEN": ("U1", "U16", "U5"),
    "LASER_BLUE": ("U1", "U17", "U3"),
    "BUCK": ("J2", "U_BUCK1", "L1"),
}
BLOCK_CHAINS = {
    "power": ("BUCK",),
    "io": ("DMX",),
    "analog": ("DAC",),
    "laser": ("LASER_RED", "LASER_GREEN", "LASER_BLUE"),
    "safety": (),
}
ELECTRICAL_ANCHORS = {
    "symbol",
    "label",
    "global_label",
    "hierarchical_label",
    "junction",
    "no_connect",
}
BUS_OBJECTS = {"bus", "bus_entry"}


@dataclass(frozen=True)
class SExprObject:
    kind: str
    source: str


def _kind(source: str) -> str:
    match = re.match(r"\(\s*([^\s()]+)", source)
    if match is None:
        raise ValueError("Malformed S-expression object")
    return match.group(1)


def _direct_children(source: str) -> list[SExprObject]:
    children = []
    depth = 0
    child_start = None
    in_string = False
    escaped = False

    for index, char in enumerate(source):
        if in_string:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
            continue

        if char == '"':
            in_string = True
        elif char == "(":
            if depth == 1:
                child_start = index
            depth += 1
        elif char == ")":
            if depth <= 0:
                raise ValueError("Unbalanced closing parenthesis")
            if depth == 2 and child_start is not None:
                child = source[child_start : index + 1]
                children.append(SExprObject(_kind(child), child))
                child_start = None
            depth -= 1

    if in_string or depth != 0:
        raise ValueError("Unbalanced S-expression")
    return children


def parse_root_objects(text: str) -> list[SExprObject]:
    stripped = text.strip()
    if _kind(stripped) != "kicad_sch":
        raise ValueError("Expected kicad_sch root object")
    return _direct_children(stripped)


def _placement(source: str) -> tuple[Decimal, Decimal, Decimal]:
    match = AT_RE.search(source)
    if match is None:
        raise ValueError(f"Object {_kind(source)} has no placement")
    angle = match.group(3) or "0"
    return Decimal(match.group(1)), Decimal(match.group(2)), Decimal(angle)


def _on_grid(value: Decimal) -> bool:
    units = value / GRID
    return abs(units - units.to_integral_value()) <= GRID_TOLERANCE


def _same_coordinate(left: Decimal, right: Decimal) -> bool:
    return abs(left - right) <= GRID_TOLERANCE


def _object_id(obj: SExprObject) -> str:
    uuid = re.search(r'\(uuid\s+"([^"]+)"\)', obj.source)
    if uuid:
        return uuid.group(1)
    reference = _property(obj, "Reference")
    return reference[0] if reference else obj.kind


def _property(
    obj: SExprObject, name: str
) -> tuple[str, Decimal, Decimal, Decimal, bool] | None:
    for child in _direct_children(obj.source):
        if child.kind != "property":
            continue
        header = PROPERTY_RE.match(child.source)
        if header is None or header.group(1) != name:
            continue
        x, y, angle = _placement(child.source)
        hidden = re.search(r"\(hide\s+yes\)", child.source) is not None
        return header.group(2), x, y, angle, hidden
    return None


def _lib_id(obj: SExprObject) -> str:
    for child in _direct_children(obj.source):
        if child.kind == "lib_id":
            match = LIB_ID_RE.match(child.source)
            if match:
                return match.group(1)
    return ""


def _placed_symbols(objects: list[SExprObject]) -> dict[str, SExprObject]:
    symbols = {}
    for obj in objects:
        if obj.kind != "symbol":
            continue
        reference = _property(obj, "Reference")
        if reference is not None:
            symbols[reference[0]] = obj
    return symbols


def placements(objects: list[SExprObject]) -> dict[str, tuple[Decimal, Decimal]]:
    return {
        reference: _placement(obj.source)[:2]
        for reference, obj in _placed_symbols(objects).items()
    }


def check_grid(objects: list[SExprObject]) -> None:
    violations = []
    for obj in objects:
        identifier = _object_id(obj)
        if obj.kind in BUS_OBJECTS:
            violations.append(f"{identifier}: electrical bus is not allowed")
            continue

        coordinates = []
        if obj.kind == "wire":
            coordinates = [
                (Decimal(x), Decimal(y)) for x, y in XY_RE.findall(obj.source)
            ]
            for start, end in zip(coordinates, coordinates[1:]):
                if not _same_coordinate(start[0], end[0]) and not _same_coordinate(
                    start[1], end[1]
                ):
                    violations.append(f"{identifier}: wire is not orthogonal")
        elif obj.kind in ELECTRICAL_ANCHORS:
            x, y, _ = _placement(obj.source)
            coordinates = [(x, y)]

        for x, y in coordinates:
            if not _on_grid(x) or not _on_grid(y):
                violations.append(
                    f"{identifier}: ({x}, {y}) is not on the 1.27 mm grid"
                )

    if violations:
        raise ValueError("; ".join(violations))


def _is_helper(reference: str, lib_id: str) -> bool:
    return reference.startswith("#") or lib_id.startswith("power:")


def _is_two_pin_passive(reference: str, lib_id: str) -> bool:
    prefixes = (
        "Device:R",
        "Device:C",
        "Device:L",
        "Device:D",
        "Device:Ferrite_Bead",
    )
    return lib_id.startswith(prefixes) or reference.startswith(("R", "C", "D", "L", "FB"))


def _horizontal(angle: Decimal) -> bool:
    return angle % Decimal("180") == 0


def _field_is_rendered_horizontal(
    symbol_angle: Decimal, field_angle: Decimal
) -> bool:
    # KiCad stores symbol-property angles in the symbol's local frame.
    # Mirroring can reverse the angle, but cannot change its axis parity.
    return _horizontal(symbol_angle + field_angle)


def check_fields(objects: list[SExprObject], refs: set[str] | None = None) -> None:
    violations = []
    for reference, obj in _placed_symbols(objects).items():
        if refs is not None and reference not in refs:
            continue
        lib_id = _lib_id(obj)
        if _is_helper(reference, lib_id):
            continue

        sx, sy, symbol_angle = _placement(obj.source)
        reference_field = _property(obj, "Reference")
        value_field = _property(obj, "Value")
        if reference_field is None or value_field is None:
            violations.append(f"{reference}: missing Reference or Value field")
            continue

        _, rx, ry, reference_angle, reference_hidden = reference_field
        _, vx, vy, value_angle, value_hidden = value_field
        for field_name, angle, hidden in (
            ("Reference", reference_angle, reference_hidden),
            ("Value", value_angle, value_hidden),
        ):
            if not hidden and not _field_is_rendered_horizontal(symbol_angle, angle):
                violations.append(f"{reference} {field_name} must be horizontal")

        if reference_hidden or value_hidden:
            continue
        if ry >= sy:
            violations.append(f"{reference} Reference must be above symbol")
        if vy <= sy:
            violations.append(f"{reference} Value must be below symbol")

    if violations:
        raise ValueError("; ".join(violations))


def _field_box(
    source: str, value: str, x: Decimal, y: Decimal
) -> tuple[Decimal, Decimal, Decimal, Decimal]:
    font_match = FONT_SIZE_RE.search(source)
    font_x = Decimal(font_match.group(1)) if font_match else GRID
    font_y = Decimal(font_match.group(2)) if font_match else GRID
    justify = frozenset(
        word
        for word in ("left", "right", "top", "bottom")
        if re.search(rf"\(justify\s+[^)]*\b{word}\b", source)
    )
    return text_field_box(
        value,
        x,
        y,
        max(GRID, font_x),
        max(GRID, font_y),
        justify,
    )


def check_field_wire_collisions(objects: list[SExprObject]) -> None:
    wires = []
    for obj in objects:
        if obj.kind != "wire":
            continue
        points = [(Decimal(x), Decimal(y)) for x, y in XY_RE.findall(obj.source)]
        wires.extend(zip(points, points[1:]))

    violations = []
    for reference, obj in _placed_symbols(objects).items():
        if _is_helper(reference, _lib_id(obj)):
            continue
        for child in _direct_children(obj.source):
            if child.kind != "property":
                continue
            header = PROPERTY_RE.match(child.source)
            if header is None or header.group(1) not in {"Reference", "Value"}:
                continue
            if re.search(r"\(hide\s+yes\)", child.source):
                continue
            x, y, _ = _placement(child.source)
            box = _field_box(child.source, header.group(2), x, y)
            if any(segment_intersects_box(segment, box) for segment in wires):
                violations.append(f"wire crosses {reference} {header.group(1)} field")

    if violations:
        raise ValueError("; ".join(violations))


def check_chain(
    name: str,
    chain: tuple[str, ...],
    symbol_positions: dict[str, tuple[Decimal, Decimal]],
) -> None:
    missing = [reference for reference in chain if reference not in symbol_positions]
    if missing:
        raise ValueError(f"{name}: missing symbols {', '.join(missing)}")
    x_positions = [symbol_positions[reference][0] for reference in chain]
    if any(left >= right for left, right in zip(x_positions, x_positions[1:])):
        raise ValueError(f"{name}: signal chain is not strictly left-to-right")


def check_block(
    name: str,
    objects: list[SExprObject],
    *,
    dx: Decimal = Decimal("0"),
    dy: Decimal = Decimal("0"),
) -> None:
    if name not in BLOCK_PLACEMENT:
        raise ValueError(f"Unknown presentation block: {name}")

    symbol_positions = placements(objects)
    violations = []
    row_min, row_max = (value + dy for value in ROWS[name])
    for column, references in BLOCK_PLACEMENT[name].items():
        column_min, column_max = (value + dx for value in COLUMNS[column])
        for reference in references:
            if reference not in symbol_positions:
                violations.append(f"{name}: missing principal symbol {reference}")
                continue
            x, y = symbol_positions[reference]
            if not column_min <= x <= column_max or not row_min <= y <= row_max:
                violations.append(
                    f"{name}: {reference} at ({x}, {y}) is outside {column} column/row"
                )

    for chain_name in BLOCK_CHAINS[name]:
        try:
            check_chain(chain_name, CHAINS[chain_name], symbol_positions)
        except ValueError as error:
            violations.append(str(error))

    if violations:
        raise ValueError("; ".join(violations))


def check_document(
    objects: list[SExprObject], *, profile: str = "canonical"
) -> None:
    if profile not in {"canonical", "visible-copy"}:
        raise ValueError(f"Unknown presentation profile: {profile}")
    check_grid(objects)
    if profile == "visible-copy":
        papers = [obj.source.strip() for obj in objects if obj.kind == "paper"]
        if papers != ['(paper "A1")']:
            raise ValueError("visible copy must use A1 landscape")
        check_field_wire_collisions(objects)
    check_fields(objects)


def check(path: Path, block: str, profile: str = "canonical") -> None:
    objects = parse_root_objects(Path(path).read_text(encoding="utf-8"))
    check_document(objects, profile=profile)

    selected = tuple(BLOCK_PLACEMENT) if block == "all" else (block,)
    dx = VISIBLE_COPY_DX if profile == "visible-copy" else Decimal("0")
    dy = VISIBLE_COPY_DY if profile == "visible-copy" else Decimal("0")
    for name in selected:
        check_block(name, objects, dx=dx, dy=dy)


def main(argv: list[str]) -> int:
    parser = ArgumentParser(description=__doc__)
    parser.add_argument(
        "--profile", choices=("canonical", "visible-copy"), default="canonical"
    )
    parser.add_argument("--block", choices=(*BLOCK_PLACEMENT, "all"), default="all")
    parser.add_argument("schematic", type=Path)
    args = parser.parse_args(argv[1:])

    try:
        check(args.schematic, args.block, args.profile)
    except (OSError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1

    print(f"PASS: schematic presentation profile={args.profile} block={args.block}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
