#!/usr/bin/env python3
"""Guard the DAC translator's four local source-termination links (pcbnew).

The length bounds are project-local regression limits, NOT manufacturer limits
or evidence of signal integrity, timing compliance, or laser safety.
"""

import json
import math
import sys

import pcbnew


def point(value):
    return value.x, value.y


def check(path):
    board = pcbnew.LoadBoard(path)
    footprints = {f.GetReference(): f for f in board.GetFootprints()}
    tracks = board.Tracks()
    result = {}
    for signal, output, pin, limit in (
        ("SCLK", 1, "21", 3.2), ("DIN", 2, "20", 3.2),
        ("SYNC", 3, "19", 4.8), ("CLR", 4, "18", 6.6),
    ):
        net = f"Net-(U_DACLV1-B{output})"
        resistor = footprints[f"R_DAC{signal}1"]
        if resistor.GetValue() != "22":
            raise ValueError(f"{signal}: expected 22 ohm source termination")
        pads = (footprints["U_DACLV1"].FindPadByNumber(pin),
                resistor.FindPadByNumber("2"))
        if any(pad is None or pad.GetNetname() != net for pad in pads):
            raise ValueError(f"{signal}: wrong source-link pad/net")
        endpoints = [point(pad.GetPosition()) for pad in pads]
        graph = {}
        length = 0
        edges = set()
        for index in range(len(tracks)):
            track = tracks[index]
            if track.GetNetname() != net:
                continue
            if track.GetClass() != "PCB_TRACK" or track.GetLayer() != pcbnew.F_Cu:
                raise ValueError(f"{signal}: source link must be F.Cu without vias")
            if track.GetWidth() < 200000:
                raise ValueError(f"{signal}: source trace below 0.20 mm")
            a, b = point(track.GetStart()), point(track.GetEnd())
            edge = tuple(sorted((a, b)))
            if a == b or edge in edges:
                raise ValueError(f"{signal}: zero-length or duplicate segment")
            edges.add(edge)
            graph.setdefault(a, set()).add(b)
            graph.setdefault(b, set()).add(a)
            length += math.dist(a, b) / 1e6
        if not graph or endpoints[0] == endpoints[1]:
            raise ValueError(f"{signal}: missing source link")
        for endpoint in endpoints:
            if len(graph.get(endpoint, ())) != 1:
                raise ValueError(f"{signal}: source pad is not a trace endpoint")
        if any(len(neighbors) != (1 if node in endpoints else 2)
               for node, neighbors in graph.items()):
            raise ValueError(f"{signal}: branched or dangling source link")
        pending = [endpoints[0]]
        visited = set()
        while pending:
            node = pending.pop()
            if node not in visited:
                visited.add(node)
                pending.extend(graph[node] - visited)
        if visited != set(graph) or endpoints[1] not in visited:
            raise ValueError(f"{signal}: disconnected source copper")
        if length > limit:
            raise ValueError(f"{signal}: {length:.3f} mm exceeds draft bound {limit} mm")
        result[signal] = {"source_length_mm": round(length, 3), "vias": 0}
    return result


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("Usage: /usr/bin/python check_dac_source_layout.py BOARD.kicad_pcb")
    try:
        print(json.dumps(check(sys.argv[1]), indent=2))
    except (OSError, KeyError, ValueError) as error:
        sys.exit(f"FAIL: {error}")
