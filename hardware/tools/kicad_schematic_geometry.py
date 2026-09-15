#!/usr/bin/env python3
"""Lossless top-level KiCad schematic access and orthogonal wire geometry."""

from __future__ import annotations

from dataclasses import dataclass, field
from decimal import Decimal
import re
from typing import Iterable


Point = tuple[Decimal, Decimal]
Segment = tuple[Point, Point]
Box = tuple[Decimal, Decimal, Decimal, Decimal]
PinRef = tuple[str, str]


def text_field_box(
    value: str,
    x: Decimal,
    y: Decimal,
    font_x: Decimal,
    font_y: Decimal,
    justify: frozenset[str] = frozenset(),
) -> Box:
    width = max(font_x, font_x * Decimal("0.6") * max(len(value), 1))
    height = font_y

    if "left" in justify:
        left, right = x, x + width
    elif "right" in justify:
        left, right = x - width, x
    else:
        left, right = x - width / 2, x + width / 2

    if "top" in justify:
        top, bottom = y, y + height
    elif "bottom" in justify:
        top, bottom = y - height, y
    else:
        top, bottom = y - height / 2, y + height / 2
    return left, top, right, bottom


def segment_intersects_box(segment: Segment, box: Box) -> bool:
    start, end = segment
    left, top, right, bottom = box
    if start[1] == end[1]:
        low, high = sorted((start[0], end[0]))
        return top <= start[1] <= bottom and high >= left and low <= right
    low, high = sorted((start[1], end[1]))
    return left <= start[0] <= right and high >= top and low <= bottom


@dataclass(frozen=True)
class RootObject:
    kind: str
    start: int
    end: int
    source: str


@dataclass
class Document:
    source: str
    objects: list[RootObject]
    root_end: int
    replacements: dict[tuple[int, int], str] = field(default_factory=dict)

    def replace(self, obj: RootObject, source: str) -> None:
        self.replacements[(obj.start, obj.end)] = source

    def remove(self, obj: RootObject) -> None:
        self.replace(obj, "")

    def append(self, source: str) -> None:
        span = (self.root_end, self.root_end)
        self.replacements[span] = self.replacements.get(span, "") + "\n  " + source

    def render(self) -> str:
        spans = sorted(self.replacements)
        for left, right in zip(spans, spans[1:]):
            if left[1] > right[0]:
                raise ValueError(f"overlapping replacements: {left}, {right}")
        rendered = self.source
        for (start, end), replacement in sorted(self.replacements.items(), reverse=True):
            rendered = rendered[:start] + replacement + rendered[end:]
        return rendered


def _kind(source: str) -> str:
    index = 1
    while index < len(source) and source[index].isspace():
        index += 1
    end = index
    while end < len(source) and not source[end].isspace() and source[end] not in "()":
        end += 1
    if end == index:
        raise ValueError("malformed S-expression object")
    return source[index:end]


def parse_document(text: str) -> Document:
    """Return direct root children while retaining exact source spans."""
    root_start = text.find("(")
    if root_start < 0 or _kind(text[root_start:]) != "kicad_sch":
        raise ValueError("expected kicad_sch root")

    objects: list[RootObject] = []
    depth = 0
    child_start: int | None = None
    in_string = False
    escaped = False
    root_end: int | None = None

    for index in range(root_start, len(text)):
        char = text[index]
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
                raise ValueError("unbalanced closing parenthesis")
            if depth == 2 and child_start is not None:
                end = index + 1
                source = text[child_start:end]
                objects.append(RootObject(_kind(source), child_start, end, source))
                child_start = None
            depth -= 1
            if depth == 0:
                root_end = index
                break

    if in_string or root_end is None or depth != 0:
        raise ValueError("unbalanced S-expression")
    if text[root_end + 1 :].strip():
        raise ValueError("content after kicad_sch root")
    return Document(text, objects, root_end)


def transform_point(
    local: Point,
    at: Point,
    angle: Decimal,
    mirror: str | None = None,
) -> Point:
    """Apply KiCad symbol mirror and quadrant rotation to a local point."""
    x, y = local
    y = -y  # library coordinates are Y-up; schematic coordinates are Y-down
    normalized = angle % Decimal("360")
    if normalized == 0:
        rotated = (x, y)
    elif normalized == 90:
        rotated = (y, -x)
    elif normalized == 180:
        rotated = (-x, -y)
    elif normalized == 270:
        rotated = (-y, x)
    else:
        raise ValueError(f"non-quadrant symbol angle: {angle}")
    if mirror == "x":
        rotated = (rotated[0], -rotated[1])
    elif mirror == "y":
        rotated = (-rotated[0], rotated[1])
    elif mirror is not None:
        raise ValueError(f"unsupported mirror axis: {mirror}")
    return at[0] + rotated[0], at[1] + rotated[1]


SExpr = str | list["SExpr"]


def parse_sexpr(source: str) -> list[SExpr]:
    """Parse the small S-expression subset used by KiCad schematics."""
    tokens: list[str] = []
    index = 0
    while index < len(source):
        char = source[index]
        if char.isspace():
            index += 1
        elif char in "()":
            tokens.append(char)
            index += 1
        elif char == '"':
            start = index
            index += 1
            escaped = False
            while index < len(source):
                current = source[index]
                if escaped:
                    escaped = False
                elif current == "\\":
                    escaped = True
                elif current == '"':
                    index += 1
                    break
                index += 1
            else:
                raise ValueError("unterminated string")
            token = source[start + 1 : index - 1]
            tokens.append(bytes(token, "utf-8").decode("unicode_escape"))
        else:
            start = index
            while index < len(source) and not source[index].isspace() and source[index] not in "()":
                index += 1
            tokens.append(source[start:index])

    stack: list[list[SExpr]] = []
    root: list[SExpr] | None = None
    for token in tokens:
        if token == "(":
            node: list[SExpr] = []
            if stack:
                stack[-1].append(node)
            stack.append(node)
        elif token == ")":
            if not stack:
                raise ValueError("unbalanced closing parenthesis")
            root = stack.pop()
        else:
            if not stack:
                raise ValueError("atom outside expression")
            stack[-1].append(token)
    if stack or root is None:
        raise ValueError("unbalanced S-expression")
    return root


def _children(node: list[SExpr], kind: str) -> list[list[SExpr]]:
    return [
        child
        for child in node[1:]
        if isinstance(child, list) and child and child[0] == kind
    ]


def _first(node: list[SExpr], kind: str) -> list[SExpr] | None:
    matches = _children(node, kind)
    return matches[0] if matches else None


def _value(node: list[SExpr], kind: str, default: str | None = None) -> str:
    child = _first(node, kind)
    if child is None or len(child) < 2 or not isinstance(child[1], str):
        if default is not None:
            return default
        raise ValueError(f"missing {kind}")
    return child[1]


def pin_endpoints(document: Document) -> dict[PinRef, Point]:
    """Resolve component pin endpoints against the embedded symbol library."""
    library_root = next((obj for obj in document.objects if obj.kind == "lib_symbols"), None)
    if library_root is None:
        raise ValueError("schematic has no lib_symbols")
    library_node = parse_sexpr(library_root.source)
    libraries: dict[str, list[SExpr]] = {}
    for symbol in _children(library_node, "symbol"):
        if len(symbol) > 1 and isinstance(symbol[1], str):
            libraries[symbol[1]] = symbol

    endpoints: dict[PinRef, Point] = {}
    for obj in document.objects:
        if obj.kind != "symbol":
            continue
        symbol = parse_sexpr(obj.source)
        lib_id = _value(symbol, "lib_id", "")
        if not lib_id or lib_id.startswith("power:"):
            continue
        reference = ""
        for prop in _children(symbol, "property"):
            if len(prop) >= 3 and prop[1] == "Reference" and isinstance(prop[2], str):
                reference = prop[2]
                break
        if not reference or reference.startswith("#"):
            continue
        at_node = _first(symbol, "at")
        if at_node is None or len(at_node) < 3:
            raise ValueError(f"{reference}: missing placement")
        at = Decimal(str(at_node[1])), Decimal(str(at_node[2]))
        angle = Decimal(str(at_node[3])) if len(at_node) > 3 else Decimal(0)
        unit = int(_value(symbol, "unit", "1"))
        mirror_node = _first(symbol, "mirror")
        mirror = str(mirror_node[1]) if mirror_node and len(mirror_node) > 1 else None
        placed_numbers = {
            str(pin[1]) for pin in _children(symbol, "pin") if len(pin) > 1
        }

        library = libraries.get(lib_id)
        if library is None:
            raise ValueError(f"{reference}: embedded library symbol {lib_id!r} missing")
        pin_definitions: dict[str, tuple[Point, int]] = {}
        for unit_symbol in _children(library, "symbol"):
            name = str(unit_symbol[1]) if len(unit_symbol) > 1 else ""
            match = re.search(r"_(\d+)_(\d+)$", name)
            definition_unit = int(match.group(1)) if match else unit
            if definition_unit not in (0, unit):
                continue
            for pin in _children(unit_symbol, "pin"):
                number_node = _first(pin, "number")
                pin_at = _first(pin, "at")
                if number_node is None or pin_at is None or len(number_node) < 2 or len(pin_at) < 3:
                    continue
                number = str(number_node[1])
                local = Decimal(str(pin_at[1])), Decimal(str(pin_at[2]))
                prior = pin_definitions.get(number)
                if prior is None or definition_unit == unit:
                    pin_definitions[number] = (local, definition_unit)

        if not placed_numbers:
            placed_numbers = set(pin_definitions)
        missing = placed_numbers - pin_definitions.keys()
        if missing:
            raise ValueError(f"{reference}: pin definitions missing for {sorted(missing)}")
        for number in placed_numbers:
            local, _ = pin_definitions[number]
            endpoints[(reference, number)] = transform_point(local, at, angle, mirror)
    return endpoints


class _UnionFind:
    def __init__(self) -> None:
        self.parent: dict[tuple[Point, str], tuple[Point, str]] = {}

    def add(self, item: tuple[Point, str]) -> None:
        self.parent.setdefault(item, item)

    def find(self, item: tuple[Point, str]) -> tuple[Point, str]:
        parent = self.parent[item]
        if parent != item:
            self.parent[item] = self.find(parent)
        return self.parent[item]

    def union(self, left: tuple[Point, str], right: tuple[Point, str]) -> None:
        self.add(left)
        self.add(right)
        left_root = self.find(left)
        right_root = self.find(right)
        if left_root != right_root:
            self.parent[right_root] = left_root


@dataclass
class WireGraph:
    union: _UnionFind
    adjacency: dict[tuple[Point, str], set[tuple[Point, str]]]

    def _node(self, point: Point, axis: str | None) -> tuple[Point, str]:
        candidates = [node for node in self.union.parent if node[0] == point]
        if axis is not None:
            candidate = (point, axis)
            if candidate not in self.union.parent:
                raise KeyError(point)
            return candidate
        if not candidates:
            raise KeyError(point)
        roots = {self.union.find(node) for node in candidates}
        if len(roots) != 1:
            raise ValueError(f"ambiguous unjoined crossing at {point}")
        return candidates[0]

    def component(self, point: Point, axis: str | None = None) -> tuple[Point, str]:
        return self.union.find(self._node(point, axis))

    def degree(self, point: Point) -> int:
        node = self._node(point, None)
        root = self.union.find(node)
        candidates = [
            candidate
            for candidate in self.union.parent
            if candidate[0] == point and self.union.find(candidate) == root
        ]
        neighbors: set[tuple[Point, str]] = set()
        for candidate in candidates:
            neighbors.update(self.adjacency[candidate])
        return len(neighbors)


def _point(raw: tuple[str, str]) -> Point:
    return Decimal(raw[0]), Decimal(raw[1])


def graph_for(
    wires: Iterable[tuple[tuple[str, str], tuple[str, str]]],
    junctions: Iterable[tuple[str, str]],
    anchors: Iterable[tuple[str, str]] = (),
) -> WireGraph:
    """Build an orthogonal graph with KiCad-style unjoined X crossings."""
    segments: list[tuple[Point, Point, str]] = []
    for raw_start, raw_end in wires:
        start, end = _point(raw_start), _point(raw_end)
        if start == end:
            raise ValueError(f"zero-length wire at {start}")
        if start[1] == end[1]:
            axis = "h"
        elif start[0] == end[0]:
            axis = "v"
        else:
            raise ValueError(f"diagonal wire: {start} -> {end}")
        segments.append((start, end, axis))

    junction_points = {_point(item) for item in junctions}
    anchor_points = {_point(item) for item in anchors}
    split_points: list[set[Point]] = [{start, end} for start, end, _ in segments]

    for index, (start, end, axis) in enumerate(segments):
        low_x, high_x = sorted((start[0], end[0]))
        low_y, high_y = sorted((start[1], end[1]))
        for point in junction_points | anchor_points:
            if low_x <= point[0] <= high_x and low_y <= point[1] <= high_y:
                split_points[index].add(point)
        for other_index, (other_start, other_end, other_axis) in enumerate(segments):
            if index >= other_index or axis == other_axis:
                continue
            horizontal = (start, end) if axis == "h" else (other_start, other_end)
            vertical = (other_start, other_end) if axis == "h" else (start, end)
            cross = (vertical[0][0], horizontal[0][1])
            h_low, h_high = sorted((horizontal[0][0], horizontal[1][0]))
            v_low, v_high = sorted((vertical[0][1], vertical[1][1]))
            if h_low <= cross[0] <= h_high and v_low <= cross[1] <= v_high:
                split_points[index].add(cross)
                split_points[other_index].add(cross)

    union = _UnionFind()
    adjacency: dict[tuple[Point, str], set[tuple[Point, str]]] = {}
    for (start, end, axis), points in zip(segments, split_points):
        ordered = sorted(points, key=(lambda point: point[0]) if axis == "h" else (lambda point: point[1]))
        for point in ordered:
            union.add((point, axis))
            adjacency.setdefault((point, axis), set())
        for left, right in zip(ordered, ordered[1:]):
            left_node, right_node = (left, axis), (right, axis)
            union.union(left_node, right_node)
            adjacency[left_node].add(right_node)
            adjacency[right_node].add(left_node)

    points_by_axis: dict[Point, set[str]] = {}
    for point, axis in union.parent:
        points_by_axis.setdefault(point, set()).add(axis)
    for point, axes in points_by_axis.items():
        endpoint_axes = {
            axis
            for start, end, axis in segments
            if point in {start, end}
        }
        if axes == {"h", "v"} and (
            point in junction_points or endpoint_axes == {"h", "v"}
        ):
            union.union((point, "h"), (point, "v"))

    return WireGraph(union, adjacency)
