#!/usr/bin/env python3
"""Measure DAC input trace overlap with opposite-layer filled ground zones.

Requires KiCad pcbnew. Refill zones with native DRC before measuring.
This geometric diagnostic is NOT an impedance, return-current, timing or EMC
test. It counts midpoint samples (<= 0.1 mm spacing) of all trace branches,
including bias stubs; vias, pads and coplanar ground are not counted as coverage.
"""
import json
import math
import sys

import pcbnew

SIGNALS = (
    "/DAC_SCLK_3V3", "/DAC_DIN_3V3", "/DAC_SYNC_3V3", "/DAC_CLR_3V3",
)
GROUNDS = ("AGND (from Galvo Board)", "5V GND Buck")
SAMPLE_STEP_MM = 0.1


def measure(path):
    board = pcbnew.LoadBoard(path)
    if not board:
        raise ValueError("Cannot load board")
    footprints = {f.GetReference(): f for f in board.GetFootprints()}
    bridge = footprints["R26"]
    polygons = {layer: [] for layer in (pcbnew.F_Cu, pcbnew.B_Cu)}
    # Keep the native zone owners alive while their polygon proxies are used.
    zones = [z for z in board.Zones() if z.GetNetname() in GROUNDS]
    for layer in polygons:
        polygons[layer] = [z.GetFilledPolysList(layer) for z in zones
                           if z.GetLayerSet().Contains(layer)]
    result = {net: {"trace_length_mm": 0.0, "over_ground_mm": 0.0}
              for net in SIGNALS}
    tracks = board.Tracks()
    for index in range(len(tracks)):
        track = tracks[index]
        net = track.GetNetname()
        if net not in result or track.GetClass() != "PCB_TRACK":
            continue
        if track.GetLayer() not in polygons:
            raise ValueError("Measurement expects a two-copper-layer board")
        start, end = track.GetStart(), track.GetEnd()
        length = math.hypot(end.x - start.x, end.y - start.y) / 1e6
        opposite = (pcbnew.B_Cu if track.GetLayer() == pcbnew.F_Cu
                    else pcbnew.F_Cu)
        samples = max(1, math.ceil(length / SAMPLE_STEP_MM))
        covered = 0
        for index in range(samples):
            fraction = (index + 0.5) / samples
            point = pcbnew.VECTOR2I(
                round(start.x + (end.x - start.x) * fraction),
                round(start.y + (end.y - start.y) * fraction))
            covered += any(poly.Contains(point) for poly in polygons[opposite])
        result[net]["trace_length_mm"] += length
        result[net]["over_ground_mm"] += length * covered / samples
    for net, row in result.items():
        if not row["trace_length_mm"]:
            raise ValueError(f"No measured traces for {net}")
        row["coverage_percent"] = 100 * row["over_ground_mm"] / row["trace_length_mm"]
        for key in row:
            row[key] = round(row[key], 3)
    return {
        "scope": "Geometric samples only; NOT a safety or production gate",
        "sample_step_mm": SAMPLE_STEP_MM,
        "R26": {
            "value": bridge.GetValue(),
            "position_mm": [bridge.GetPosition().x / 1e6,
                            bridge.GetPosition().y / 1e6],
            "pad_nets": {pad.GetNumber(): pad.GetNetname() for pad in bridge.Pads()},
        },
        "signals": result,
    }


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("Usage: /usr/bin/python measure_dac_return_layout.py BOARD.kicad_pcb")
    try:
        print(json.dumps(measure(sys.argv[1]), indent=2))
    except (OSError, KeyError, ValueError) as error:
        sys.exit(f"Measurement failed: {error}")
