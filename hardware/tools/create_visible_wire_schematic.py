#!/usr/bin/env python3
"""Create a deterministic A1 copy of the GalvOS schematic with visible wires."""

from __future__ import annotations

from dataclasses import dataclass
from decimal import Decimal
from pathlib import Path
import re
import sys
import uuid
import xml.etree.ElementTree as ET

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from hardware.tools.kicad_schematic_geometry import (
    graph_for,
    parse_document,
    parse_sexpr,
    pin_endpoints,
    segment_intersects_box,
    text_field_box,
)


Point = tuple[Decimal, Decimal]
Segment = tuple[Point, Point]
GRID = Decimal("1.27")
DX = Decimal("100.33")
DY = Decimal("76.20")
NUMBER = r"[-+]?\d+(?:\.\d+)?"
COORD_RE = re.compile(rf"(\((?:at|xy)\s+)({NUMBER})(\s+)({NUMBER})")
AT_RE = re.compile(rf"\(at\s+({NUMBER})\s+({NUMBER})(?:\s+({NUMBER}))?\)")
PROPERTY_RE = re.compile(r'^\(property\s+"(Reference|Value)"\s+"([^"]*)"')
FONT_SIZE_RE = re.compile(rf"\(size\s+({NUMBER})\s+({NUMBER})\)")
UUID_NAMESPACE = uuid.UUID("c0d5b3f8-6b55-5d5c-a82e-c286d91e315c")
POWER_NET_NAMES = frozenset(
    {
        "+3V3", "+15V", "-15V", "/+5V_ANA", "/+5V_MCU",
        "/FAN1_12V", "/FAN2_12V", "/VCC_BUCK", "/VIN_BUCK",
        "5V GND Buck", "AGND (from Galvo Board)", "Buck +5V", "VIN_RAW",
    }
)


def _format(value: Decimal) -> str:
    rendered = format(value, "f").rstrip("0").rstrip(".")
    return rendered if rendered not in {"", "-0"} else "0"


def translate_object(source: str, dx: Decimal, dy: Decimal) -> str:
    def replace(match: re.Match[str]) -> str:
        x = Decimal(match.group(2)) + dx
        y = Decimal(match.group(4)) + dy
        return f"{match.group(1)}{_format(x)}{match.group(3)}{_format(y)}"

    return COORD_RE.sub(replace, source)


def add_hidden_effect(source: str) -> str:
    if "(hide yes)" in source:
        return source
    start = source.find("(effects")
    if start < 0:
        raise ValueError("label has no effects block")
    depth = 0
    in_string = False
    escaped = False
    for index in range(start, len(source)):
        char = source[index]
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
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                return source[:index] + "\n\t\t\t(hide yes)" + source[index:]
    raise ValueError("unbalanced effects block")


def _segment(start: Point, end: Point) -> Segment | None:
    if start == end:
        return None
    return (start, end) if start <= end else (end, start)


@dataclass(frozen=True)
class RouteResult:
    net: str
    segments: tuple[Segment, ...]
    junctions: frozenset[Point]
    conflicts: int = 0


def _occupied_segments(prior: list[RouteResult]) -> set[Segment]:
    return {segment for route in prior for segment in route.segments}


def _between(value: Decimal, left: Decimal, right: Decimal) -> bool:
    low, high = sorted((left, right))
    return low <= value <= high


def _point_on_segment(point: Point, segment: Segment) -> bool:
    start, end = segment
    if start[0] == end[0]:
        return point[0] == start[0] and _between(point[1], start[1], end[1])
    return point[1] == start[1] and _between(point[0], start[0], end[0])


def _segment_covers(outer: Segment, inner: Segment) -> bool:
    if outer[0][1] == outer[1][1] == inner[0][1] == inner[1][1]:
        outer_low, outer_high = sorted((outer[0][0], outer[1][0]))
        inner_low, inner_high = sorted((inner[0][0], inner[1][0]))
    elif outer[0][0] == outer[1][0] == inner[0][0] == inner[1][0]:
        outer_low, outer_high = sorted((outer[0][1], outer[1][1]))
        inner_low, inner_high = sorted((inner[0][1], inner[1][1]))
    else:
        return False
    return outer_low <= inner_low and inner_high <= outer_high


def _split_segments_at_points(
    segments: tuple[Segment, ...], points: set[Point]
) -> tuple[Segment, ...]:
    split: set[Segment] = set()
    for segment in segments:
        start, end = segment
        cuts = {start, end} | {
            point for point in points if _point_on_segment(point, segment)
        }
        ordered = sorted(cuts, key=(lambda point: point[0]) if start[1] == end[1]
                         else (lambda point: point[1]))
        for left, right in zip(ordered, ordered[1:]):
            piece = _segment(left, right)
            if piece is not None:
                split.add(piece)
    return tuple(sorted(split))


def _merge_collinear_segments(segments: tuple[Segment, ...]) -> tuple[Segment, ...]:
    horizontal: dict[Decimal, list[tuple[Decimal, Decimal]]] = {}
    vertical: dict[Decimal, list[tuple[Decimal, Decimal]]] = {}
    for start, end in segments:
        if start[1] == end[1]:
            horizontal.setdefault(start[1], []).append(
                tuple(sorted((start[0], end[0])))
            )
        else:
            vertical.setdefault(start[0], []).append(
                tuple(sorted((start[1], end[1])))
            )

    merged: set[Segment] = set()
    for y, intervals in horizontal.items():
        current_low, current_high = sorted(intervals)[0]
        for low, high in sorted(intervals)[1:]:
            if low <= current_high:
                current_high = max(current_high, high)
            else:
                merged.add(((current_low, y), (current_high, y)))
                current_low, current_high = low, high
        merged.add(((current_low, y), (current_high, y)))
    for x, intervals in vertical.items():
        current_low, current_high = sorted(intervals)[0]
        for low, high in sorted(intervals)[1:]:
            if low <= current_high:
                current_high = max(current_high, high)
            else:
                merged.add(((x, current_low), (x, current_high)))
                current_low, current_high = low, high
        merged.add(((x, current_low), (x, current_high)))
    return tuple(sorted(merged))


def _prune_segment_leaves(
    segments: tuple[Segment, ...],
    protected_points: set[Point],
    junctions: set[Point],
) -> tuple[Segment, ...]:
    connection_points = protected_points | junctions | set(
        _junctions_for_segments(segments)
    )
    remaining = set(
        _split_segments_at_points(segments, connection_points)
    )
    while remaining:
        degree: dict[Point, int] = {}
        for start, end in remaining:
            degree[start] = degree.get(start, 0) + 1
            degree[end] = degree.get(end, 0) + 1
        leaves = {
            segment
            for segment in remaining
            if any(
                degree[point] == 1 and point not in protected_points
                for point in segment
            )
        }
        if not leaves:
            break
        remaining.difference_update(leaves)
    return tuple(sorted(remaining))


def _junctions_for_segments(
    segments: tuple[Segment, ...],
    explicit: frozenset[Point] = frozenset(),
    *,
    connect_crossings: bool = True,
) -> frozenset[Point]:
    """Derive required junction dots from the final routed geometry."""
    candidates = {point for segment in segments for point in segment}
    for index, left in enumerate(segments):
        left_horizontal = left[0][1] == left[1][1]
        for right in segments[index + 1:]:
            right_horizontal = right[0][1] == right[1][1]
            if left_horizontal == right_horizontal:
                continue
            horizontal, vertical = (left, right) if left_horizontal else (right, left)
            crossing = (vertical[0][0], horizontal[0][1])
            if _point_on_segment(crossing, horizontal) and _point_on_segment(crossing, vertical):
                candidates.add(crossing)

    junctions: set[Point] = set()
    for point in candidates:
        directions: set[str] = set()
        endpoint_axes: set[str] = set()
        for start, end in segments:
            if not _point_on_segment(point, (start, end)):
                continue
            if start[1] == end[1]:
                if point in {start, end}:
                    endpoint_axes.add("h")
                low, high = sorted((start[0], end[0]))
                if low < point[0]:
                    directions.add("left")
                if point[0] < high:
                    directions.add("right")
            else:
                if point in {start, end}:
                    endpoint_axes.add("v")
                low, high = sorted((start[1], end[1]))
                if low < point[1]:
                    directions.add("up")
                if point[1] < high:
                    directions.add("down")
        if len(directions) >= 3 and (
            connect_crossings
            or point in explicit
            or endpoint_axes == {"h", "v"}
        ):
            junctions.add(point)
    return frozenset(junctions)


def segments_conflict(left: Segment, right: Segment) -> bool:
    """Return true for overlap or endpoint contact, not a clean X crossing."""
    left_h = left[0][1] == left[1][1]
    right_h = right[0][1] == right[1][1]
    if left_h == right_h:
        if left_h and left[0][1] != right[0][1]:
            return False
        if not left_h and left[0][0] != right[0][0]:
            return False
        left_values = (left[0][0], left[1][0]) if left_h else (left[0][1], left[1][1])
        right_values = (right[0][0], right[1][0]) if right_h else (right[0][1], right[1][1])
        return max(min(left_values), min(right_values)) <= min(max(left_values), max(right_values))
    horizontal, vertical = (left, right) if left_h else (right, left)
    cross = (vertical[0][0], horizontal[0][1])
    if not _point_on_segment(cross, horizontal) or not _point_on_segment(cross, vertical):
        return False
    return cross in {left[0], left[1], right[0], right[1]}


def _candidate_score(
    segments: tuple[Segment, ...],
    ports: set[Point],
    blocked_segments: list[Segment],
    blocked_points: set[Point],
    prior: list[RouteResult],
) -> tuple[int, Decimal]:
    conflicts = 0
    for segment in segments:
        for obstacle in blocked_segments:
            if segments_conflict(segment, obstacle):
                contacts = {
                    point for point in ports if _point_on_segment(point, segment) and _point_on_segment(point, obstacle)
                }
                if not contacts or any(
                    _point_on_segment(point, segment) and _point_on_segment(point, obstacle)
                    for point in ({segment[0], segment[1], obstacle[0], obstacle[1]} - contacts)
                ):
                    conflicts += 1
        for route in prior:
            conflicts += sum(segments_conflict(segment, obstacle) for obstacle in route.segments)
        conflicts += sum(
            _point_on_segment(point, segment) and point not in ports
            for point in blocked_points
        )
    length = sum(abs(a[0] - b[0]) + abs(a[1] - b[1]) for a, b in segments)
    return conflicts, length


def _detour_segment(
    segment: Segment,
    ports: set[Point],
    blocked_segments: list[Segment],
    blocked_points: set[Point],
    prior: list[RouteResult],
) -> tuple[Segment, ...]:
    direct = (segment,)
    direct_score = _candidate_score(direct, ports, blocked_segments, blocked_points, prior)
    if direct_score[0] == 0:
        return direct
    start, end = segment
    horizontal = start[1] == end[1]
    candidates: list[tuple[int, Decimal, tuple[Segment, ...]]] = []
    for step in range(1, 81):
        for direction in (-1, 1):
            offset = GRID * step * direction
            if horizontal:
                shifted_start = (start[0], start[1] + offset)
                shifted_end = (end[0], end[1] + offset)
            else:
                shifted_start = (start[0] + offset, start[1])
                shifted_end = (end[0] + offset, end[1])
            pieces = tuple(
                piece
                for piece in (
                    _segment(start, shifted_start),
                    _segment(shifted_start, shifted_end),
                    _segment(shifted_end, end),
                )
                if piece is not None
            )
            conflicts, length = _candidate_score(
                pieces, ports, blocked_segments, blocked_points, prior
            )
            candidates.append((conflicts, length, pieces))
            for start_direction in (-1, 1):
                for end_direction in (-1, 1):
                    if horizontal:
                        escaped_start = (
                            start[0] + GRID * start_direction,
                            start[1],
                        )
                        escaped_end = (
                            end[0] + GRID * end_direction,
                            end[1],
                        )
                        shifted_escaped_start = (escaped_start[0], start[1] + offset)
                        shifted_escaped_end = (escaped_end[0], end[1] + offset)
                    else:
                        escaped_start = (
                            start[0],
                            start[1] + GRID * start_direction,
                        )
                        escaped_end = (
                            end[0],
                            end[1] + GRID * end_direction,
                        )
                        shifted_escaped_start = (start[0] + offset, escaped_start[1])
                        shifted_escaped_end = (end[0] + offset, escaped_end[1])
                    escaped_pieces = tuple(
                        piece
                        for piece in (
                            _segment(start, escaped_start),
                            _segment(escaped_start, shifted_escaped_start),
                            _segment(shifted_escaped_start, shifted_escaped_end),
                            _segment(shifted_escaped_end, escaped_end),
                            _segment(escaped_end, end),
                        )
                        if piece is not None
                    )
                    escaped_conflicts, escaped_length = _candidate_score(
                        escaped_pieces,
                        ports,
                        blocked_segments,
                        blocked_points,
                        prior,
                    )
                    candidates.append(
                        (escaped_conflicts, escaped_length, escaped_pieces)
                    )
    for point in blocked_points - ports:
        if not _point_on_segment(point, segment):
            continue
        for direction in (-1, 1):
            offset = GRID * direction
            if horizontal:
                segment_low, segment_high = sorted((start[0], end[0]))
                before = (max(segment_low, point[0] - GRID), point[1])
                after = (min(segment_high, point[0] + GRID), point[1])
                shifted_before = (before[0], before[1] + offset)
                shifted_after = (after[0], after[1] + offset)
            else:
                segment_low, segment_high = sorted((start[1], end[1]))
                before = (point[0], max(segment_low, point[1] - GRID))
                after = (point[0], min(segment_high, point[1] + GRID))
                shifted_before = (before[0] + offset, before[1])
                shifted_after = (after[0] + offset, after[1])
            local_pieces = tuple(
                piece
                for piece in (
                    _segment(start, before),
                    _segment(before, shifted_before),
                    _segment(shifted_before, shifted_after),
                    _segment(shifted_after, after),
                    _segment(after, end),
                )
                if piece is not None
            )
            local_conflicts, local_length = _candidate_score(
                local_pieces,
                ports,
                blocked_segments,
                blocked_points,
                prior,
            )
            candidates.append((local_conflicts, local_length, local_pieces))
    best = min(candidates)
    return best[2] if best[:2] < direct_score else direct


def _detour_route(
    route: RouteResult,
    ports: set[Point],
    blocked_segments: list[Segment],
    blocked_points: set[Point],
    prior: list[RouteResult],
) -> RouteResult:
    segments: list[Segment] = []
    for segment in route.segments:
        segments.extend(
            _detour_segment(
                segment, ports, blocked_segments, blocked_points, prior
            )
        )
    normalized = tuple(sorted(set(segments)))
    conflicts, _ = _candidate_score(
        normalized, ports, blocked_segments, blocked_points, prior
    )
    return RouteResult(
        route.net,
        normalized,
        _junctions_for_segments(normalized),
        conflicts,
    )


def route_tree(
    net: str,
    ports: list[Point],
    prior: list[RouteResult],
    *,
    blocked_segments: list[Segment] | None = None,
    blocked_points: set[Point] | None = None,
) -> RouteResult:
    """Create a deterministic orthogonal trunk for one net's label ports."""
    unique = sorted(set(ports))
    if len(unique) < 2:
        return RouteResult(net, (), frozenset(), 0)
    blocked_segments = blocked_segments or []
    blocked_points = blocked_points or set()
    port_set = set(unique)
    ys = sorted({point[1] for point in unique})
    xs = sorted({point[0] for point in unique})
    median_y = ys[len(ys) // 2]
    median_x = xs[len(xs) // 2]
    page_ys = [GRID * step for step in range(10, 459)]
    page_xs = [GRID * step for step in range(10, 653)]
    candidate_ys = sorted(set(ys + page_ys), key=lambda value: (abs(value - median_y), value))
    candidate_xs = sorted(set(xs + page_xs), key=lambda value: (abs(value - median_x), value))

    choices: list[tuple[int, Decimal, int, Decimal, tuple[Segment, ...], frozenset[Point]]] = []
    for trunk_y in candidate_ys:
        min_x = min(point[0] for point in unique)
        max_x = max(point[0] for point in unique)
        segments: list[Segment] = []
        trunk = _segment((min_x, trunk_y), (max_x, trunk_y))
        if trunk:
            segments.append(trunk)
        junctions: set[Point] = set()
        for point in unique:
            branch = _segment(point, (point[0], trunk_y))
            if branch:
                segments.append(branch)
            if min_x < point[0] < max_x and branch:
                junctions.add((point[0], trunk_y))
        normalized = tuple(sorted(set(segments)))
        conflicts, length = _candidate_score(
            normalized, port_set, blocked_segments, blocked_points, prior
        )
        choices.append((conflicts, length, 0, trunk_y, normalized, frozenset(junctions)))

    for trunk_x in candidate_xs:
        min_y = min(point[1] for point in unique)
        max_y = max(point[1] for point in unique)
        segments = []
        trunk = _segment((trunk_x, min_y), (trunk_x, max_y))
        if trunk:
            segments.append(trunk)
        junctions = set()
        for point in unique:
            branch = _segment(point, (trunk_x, point[1]))
            if branch:
                segments.append(branch)
            if min_y < point[1] < max_y and branch:
                junctions.add((trunk_x, point[1]))
        normalized = tuple(sorted(set(segments)))
        conflicts, length = _candidate_score(
            normalized, port_set, blocked_segments, blocked_points, prior
        )
        choices.append((conflicts, length, 1, trunk_x, normalized, frozenset(junctions)))

    conflicts, _, _, _, segments, junctions = min(choices)
    route = RouteResult(net, segments, junctions, conflicts)
    for _ in range(8):
        if not route.conflicts:
            break
        detoured = _detour_route(
            route, port_set, blocked_segments, blocked_points, prior
        )
        if detoured.segments == route.segments:
            break
        route = detoured
    merged_segments = _merge_collinear_segments(route.segments)
    split_segments = _split_segments_at_points(merged_segments, port_set)
    split_segments = _prune_segment_leaves(
        split_segments, port_set, set(route.junctions)
    )
    split_segments = _split_segments_at_points(
        _merge_collinear_segments(split_segments), port_set
    )
    route = RouteResult(
        route.net,
        split_segments,
        _junctions_for_segments(split_segments, route.junctions),
        route.conflicts,
    )
    return route


@dataclass(frozen=True)
class BuildReport:
    routed_by_region: dict[str, frozenset[str]]
    unrouted_multi_pin_signal_nets: frozenset[str]
    laser_channel_order: tuple[tuple[str, str, str, str], ...]


def _child(node, kind: str):
    for child in node[1:]:
        if isinstance(child, list) and child and child[0] == kind:
            return child
    return None


def _label_data(source: str, kind: str) -> tuple[str, Point]:
    node = parse_sexpr(source)
    at = _child(node, "at")
    if len(node) < 2 or at is None:
        raise ValueError(f"malformed {kind}")
    text = str(node[1])
    point = Decimal(str(at[1])) + DX, Decimal(str(at[2])) + DY
    net = f"/{text}" if kind == "label" and not text.startswith("/") else text
    return net, point


def _wire_segments(source: str) -> list[Segment]:
    node = parse_sexpr(source)
    pts = _child(node, "pts")
    if pts is None:
        return []
    points = [
        (Decimal(str(child[1])), Decimal(str(child[2])))
        for child in pts[1:]
        if isinstance(child, list) and child and child[0] == "xy"
    ]
    return [segment for start, end in zip(points, points[1:]) if (segment := _segment(start, end))]


def _balanced_end(source: str, start: int) -> int:
    depth = 0
    in_string = False
    escaped = False
    for index in range(start, len(source)):
        char = source[index]
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
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                return index + 1
    raise ValueError("unbalanced property object")


@dataclass(frozen=True)
class _FieldRecord:
    symbol: object
    key: tuple[int, str]
    name: str
    value: str
    source: str
    span: tuple[int, int]
    point: Point
    symbol_point: Point
    font: Point
    justify: frozenset[str]


def _field_records(document) -> list[_FieldRecord]:
    records = []
    for symbol in (obj for obj in document.objects if obj.kind == "symbol"):
        symbol_node = parse_sexpr(symbol.source)
        symbol_at = _child(symbol_node, "at")
        if symbol_at is None:
            continue
        symbol_point = Decimal(str(symbol_at[1])), Decimal(str(symbol_at[2]))
        for match in re.finditer(r'\(property\s+"(?:Reference|Value)"', symbol.source):
            end = _balanced_end(symbol.source, match.start())
            source = symbol.source[match.start():end]
            header = PROPERTY_RE.match(source)
            at = AT_RE.search(source)
            if header is None or at is None or "(hide yes)" in source:
                continue
            font_match = FONT_SIZE_RE.search(source)
            font = (
                max(GRID, Decimal(font_match.group(1))) if font_match else GRID,
                max(GRID, Decimal(font_match.group(2))) if font_match else GRID,
            )
            justify = frozenset(
                word
                for word in ("left", "right", "top", "bottom")
                if re.search(rf"\(justify\s+[^)]*\b{word}\b", source)
            )
            records.append(
                _FieldRecord(
                    symbol,
                    (symbol.start, header.group(1)),
                    header.group(1),
                    header.group(2),
                    source,
                    (match.start(), end),
                    (Decimal(at.group(1)), Decimal(at.group(2))),
                    symbol_point,
                    font,
                    justify,
                )
            )
    return records


def _boxes_overlap(left, right) -> bool:
    return not (
        left[2] < right[0]
        or right[2] < left[0]
        or left[3] < right[1]
        or right[3] < left[1]
    )


def _reposition_visible_fields(document) -> int:
    wires = tuple(
        segment
        for obj in document.objects
        if obj.kind == "wire"
        for segment in _wire_segments(obj.source)
    )
    records = _field_records(document)
    boxes = {
        record.key: text_field_box(
            record.value,
            *record.point,
            *record.font,
            record.justify,
        )
        for record in records
    }
    positions: dict[tuple[int, str], Point] = {}
    moved = 0

    for record in records:
        own_box = boxes[record.key]
        collides = any(segment_intersects_box(wire, own_box) for wire in wires)
        collides = collides or any(
            key != record.key and _boxes_overlap(own_box, box)
            for key, box in boxes.items()
        )
        if not collides:
            positions[record.key] = record.point
            continue

        candidates = []
        for dx in range(-24, 25):
            for dy in range(-24, 25):
                point = record.point[0] + GRID * dx, record.point[1] + GRID * dy
                if record.name == "Reference" and point[1] >= record.symbol_point[1]:
                    continue
                if record.name == "Value" and point[1] <= record.symbol_point[1]:
                    continue
                candidates.append((abs(dx) + abs(dy), abs(dy), abs(dx), dy, dx, point))

        for *_, point in sorted(candidates):
            candidate_box = text_field_box(
                record.value,
                *point,
                *record.font,
                record.justify,
            )
            if any(segment_intersects_box(wire, candidate_box) for wire in wires):
                continue
            if any(
                key != record.key and _boxes_overlap(candidate_box, box)
                for key, box in boxes.items()
            ):
                continue
            positions[record.key] = point
            boxes[record.key] = candidate_box
            moved += 1
            break
        else:
            raise ValueError(
                f"cannot place {record.value} {record.name} without wire collision"
            )

    replacements: dict[object, list[tuple[tuple[int, int], Point]]] = {}
    for record in records:
        if positions[record.key] != record.point:
            replacements.setdefault(record.symbol, []).append(
                (record.span, positions[record.key])
            )
    for symbol, changes in replacements.items():
        source = symbol.source
        for (start, end), point in sorted(changes, reverse=True):
            field_source = source[start:end]
            field_source = AT_RE.sub(
                lambda match: (
                    f"(at {_format(point[0])} {_format(point[1])}"
                    f" {match.group(3) or '0'})"
                ),
                field_source,
                count=1,
            )
            source = source[:start] + field_source + source[end:]
        document.replace(symbol, source)
    return moved


def _remove_redundant_wires(document) -> int:
    """Remove single-segment wires fully covered by another wire."""
    entries = [
        (obj, _wire_segments(obj.source))
        for obj in document.objects
        if obj.kind == "wire"
    ]
    removed = 0
    for index, (obj, segments) in enumerate(entries):
        if len(segments) != 1:
            continue
        segment = segments[0]
        redundant = any(
            len(other_segments) == 1
            and _segment_covers(other_segments[0], segment)
            and (other_segments[0] != segment or other_index < index)
            for other_index, (_, other_segments) in enumerate(entries)
            if other_index != index
        )
        if redundant:
            document.remove(obj)
            removed += 1
    return removed


def _uuid(kind: str, key: str) -> str:
    return str(uuid.uuid5(UUID_NAMESPACE, f"galvos-visible-wire:{kind}:{key}"))


def _wire_source(net: str, index: int, segment: Segment) -> str:
    start, end = segment
    return (
        "(wire\n"
        "\t\t(pts\n"
        f"\t\t\t(xy {_format(start[0])} {_format(start[1])}) "
        f"(xy {_format(end[0])} {_format(end[1])})\n"
        "\t\t)\n"
        "\t\t(stroke\n\t\t\t(width 0)\n\t\t\t(type default)\n\t\t)\n"
        f'\t\t(uuid "{_uuid("wire", f"{net}:{index}")}")\n\t)'
    )


def _normalize_wires(document, protected_points: set[Point]) -> int:
    """Replace the wire layer with merged lines split at real pin endpoints."""
    wire_objects = [obj for obj in document.objects if obj.kind == "wire"]
    segments = tuple(
        segment
        for obj in wire_objects
        for segment in _wire_segments(obj.source)
    )
    normalized = _split_segments_at_points(
        _merge_collinear_segments(segments), protected_points
    )
    for obj in wire_objects:
        document.remove(obj)
    for index, segment in enumerate(normalized):
        document.append(_wire_source("normalized", index, segment))
    return len(wire_objects) - len(normalized)


def _junction_source(net: str, index: int, point: Point) -> str:
    return (
        f"(junction (at {_format(point[0])} {_format(point[1])}) "
        "(diameter 0) (color 0 0 0 0) "
        f'(uuid "{_uuid("junction", f"{net}:{index}")}"))'
    )


def _hidden_label_source(net: str, point: Point) -> str:
    text = net[1:]
    return (
        f'(label "{text}"\n'
        f"\t\t(at {_format(point[0])} {_format(point[1])} 0)\n"
        "\t\t(effects\n\t\t\t(font\n\t\t\t\t(size 1.27 1.27)\n"
        "\t\t\t)\n\t\t\t(hide yes)\n\t\t)\n"
        f'\t\t(uuid "{_uuid("label", net)}")\n\t)'
    )


def _region_for(points: list[Point]) -> str:
    original_y = max(point[1] - DY for point in points)
    if original_y < Decimal("100"):
        return "power"
    if original_y < Decimal("170"):
        return "io"
    if original_y < Decimal("245"):
        return "analog"
    if original_y < Decimal("320"):
        return "laser"
    return "safety"


def build_text(
    source: str,
    baseline_nets: dict[str, frozenset[tuple[str, str]]],
) -> tuple[str, BuildReport]:
    """Transform schematic source without changing its component payload."""
    document = parse_document(source)
    placed_pins = {
        pin: (point[0] + DX, point[1] + DY)
        for pin, point in pin_endpoints(document).items()
    }
    blocked_segments: list[Segment] = []
    blocked_points: set[Point] = set(placed_pins.values())
    translated_kinds = {
        "symbol", "wire", "junction", "no_connect", "text", "text_box",
        "polyline", "rectangle",
    }
    signal_nets = {
        name
        for name, pins in baseline_nets.items()
        if len(pins) >= 2
        and name not in POWER_NET_NAMES
        and not name.startswith("unconnected-(")
    }

    ports: dict[str, list[Point]] = {
        net: [placed_pins[pin] for pin in baseline_nets[net] if pin in placed_pins]
        for net in signal_nets
    }
    missing_pins = {
        net: sorted(baseline_nets[net] - placed_pins.keys())
        for net in signal_nets
        if baseline_nets[net] - placed_pins.keys()
    }
    if missing_pins:
        raise ValueError(f"signal pin coordinates missing: {missing_pins}")

    original_wires: list[tuple[object, Segment]] = []
    original_junctions: list[tuple[object, Point]] = []
    original_labels: list[Point] = []
    for obj in document.objects:
        if obj.kind == "wire":
            for segment in _wire_segments(translate_object(obj.source, DX, DY)):
                original_wires.append((obj, segment))
        elif obj.kind == "junction":
            node = parse_sexpr(translate_object(obj.source, DX, DY))
            at = _child(node, "at")
            if at is not None:
                original_junctions.append(
                    (obj, (Decimal(str(at[1])), Decimal(str(at[2]))))
                )
        elif obj.kind in {"label", "global_label", "hierarchical_label"}:
            _, point = _label_data(obj.source, obj.kind)
            original_labels.append(point)

    original_graph = graph_for(
        [
            ((str(start[0]), str(start[1])), (str(end[0]), str(end[1])))
            for _, (start, end) in original_wires
        ],
        [(str(point[0]), str(point[1])) for _, point in original_junctions],
        [
            (str(point[0]), str(point[1]))
            for point in blocked_points | set(original_labels)
        ],
    )
    component_nets: dict[tuple[Point, str], set[str]] = {}
    for net in signal_nets:
        for pin in baseline_nets[net]:
            point = placed_pins[pin]
            try:
                component = original_graph.component(point)
            except (KeyError, ValueError):
                continue
            component_nets.setdefault(component, set()).add(net)

    signal_spans: set[tuple[int, int]] = set()
    for obj, (start, end) in original_wires:
        axis = "h" if start[1] == end[1] else "v"
        component = original_graph.component(start, axis=axis)
        if component_nets.get(component):
            signal_spans.add((obj.start, obj.end))
    for obj, point in original_junctions:
        try:
            component = original_graph.component(point)
        except (KeyError, ValueError):
            continue
        if component_nets.get(component):
            signal_spans.add((obj.start, obj.end))

    for obj in document.objects:
        if obj.kind == "paper":
            document.replace(obj, '(paper "A1")')
        elif obj.kind in {"label", "global_label", "hierarchical_label"}:
            net, point = _label_data(obj.source, obj.kind)
            blocked_points.add(point)
            if net in signal_nets:
                document.remove(obj)
            else:
                document.replace(obj, add_hidden_effect(translate_object(obj.source, DX, DY)))
        elif obj.kind in translated_kinds:
            if (obj.start, obj.end) in signal_spans:
                document.remove(obj)
                continue
            translated = translate_object(obj.source, DX, DY)
            document.replace(obj, translated)
            if obj.kind == "wire":
                blocked_segments.extend(_wire_segments(translated))
        elif obj.kind in {"bus", "bus_entry", "bus_alias", "hierarchical_sheet"}:
            document.remove(obj)

    routed: list[RouteResult] = []
    by_region: dict[str, set[str]] = {name: set() for name in ("power", "io", "analog", "laser", "safety")}
    unrouted: set[str] = set()
    for net in sorted(
        signal_nets,
        key=lambda name: (
            {"/SD_MISO": 0, "/FAN2_PWM": 1}.get(name, 2),
            -len(set(ports.get(name, []))),
            name,
        ),
    ):
        net_ports = sorted(set(ports.get(net, [])))
        if len(net_ports) < 2:
            unrouted.add(net)
            continue
        route = route_tree(
            net,
            net_ports,
            routed,
            blocked_segments=blocked_segments,
            blocked_points=blocked_points,
        )
        if route.conflicts:
            unrouted.add(net)
            continue
        routed.append(route)
        by_region[_region_for(net_ports)].add(net)
        for index, segment in enumerate(route.segments):
            document.append(_wire_source(net, index, segment))
        for index, point in enumerate(sorted(route.junctions)):
            document.append(_junction_source(net, index, point))
        if net.startswith("/"):
            document.append(_hidden_label_source(net, net_ports[0]))

    draft = parse_document(document.render())
    pre_dedupe_segments: list[Segment] = []
    pre_dedupe_junctions: set[Point] = set()
    for obj in draft.objects:
        if obj.kind == "wire":
            pre_dedupe_segments.extend(_wire_segments(obj.source))
        elif obj.kind == "junction":
            node = parse_sexpr(obj.source)
            at = _child(node, "at")
            if at is not None:
                pre_dedupe_junctions.add(
                    (Decimal(str(at[1])), Decimal(str(at[2])))
                )
    preserved_connections = _junctions_for_segments(
        tuple(pre_dedupe_segments),
        frozenset(pre_dedupe_junctions),
        connect_crossings=False,
    )
    _normalize_wires(draft, set(placed_pins.values()))
    final_document = parse_document(draft.render())
    final_segments: list[Segment] = []
    existing_junctions: set[Point] = set()
    for obj in final_document.objects:
        if obj.kind == "wire":
            final_segments.extend(_wire_segments(obj.source))
        elif obj.kind == "junction":
            node = parse_sexpr(obj.source)
            at = _child(node, "at")
            if at is not None:
                existing_junctions.add((Decimal(str(at[1])), Decimal(str(at[2]))))
    required_junctions = _junctions_for_segments(
        tuple(final_segments),
        frozenset(existing_junctions | preserved_connections),
        connect_crossings=False,
    )
    for index, point in enumerate(sorted(required_junctions - existing_junctions)):
        final_document.append(_junction_source("derived", index, point))

    _reposition_visible_fields(final_document)

    report = BuildReport(
        {name: frozenset(nets) for name, nets in by_region.items()},
        frozenset(unrouted),
        (
            ("/GPIO7", "/TTL_RGB_RED", "U15", "U4"),
            ("/GPIO8", "/TTL_RGB_GREEN", "U16", "U5"),
            ("/GPIO21", "/TTL_RGB_BLUE", "U17", "U3"),
        ),
    )
    rendered = final_document.render()
    rendered = "\n".join(line.rstrip() for line in rendered.splitlines())
    if source.endswith("\n"):
        rendered += "\n"
    return rendered, report


def read_baseline(path: Path) -> dict[str, frozenset[tuple[str, str]]]:
    root = ET.parse(path).getroot()
    return {
        net.attrib["name"]: frozenset(
            (node.attrib["ref"], node.attrib["pin"]) for node in net.findall("node")
        )
        for net in root.findall("./nets/net")
    }


def build(source: Path, baseline: Path, output: Path) -> BuildReport:
    rendered, report = build_text(source.read_text(encoding="utf-8"), read_baseline(baseline))
    if report.unrouted_multi_pin_signal_nets:
        raise ValueError(
            "unrouted multi-pin signal nets: "
            + ", ".join(sorted(report.unrouted_multi_pin_signal_nets))
        )
    output.write_text(rendered, encoding="utf-8")
    return report


def main(argv: list[str]) -> int:
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv[1:])
    try:
        report = build(args.source, args.baseline, args.output)
    except (ET.ParseError, OSError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    routed_count = sum(len(nets) for nets in report.routed_by_region.values())
    print(f"PASS: generated A1 visible-wire copy with {routed_count} signal nets")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
