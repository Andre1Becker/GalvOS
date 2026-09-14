#!/usr/bin/env python3
"""Guard the draft's explicit, local buck input connections (requires pcbnew).

Usage: /usr/bin/python hardware/tests/check_buck_input_layout.py BOARD.kicad_pcb
This geometry check does not establish capacitance, current capacity or EMI.
"""

import math
import sys

import pcbnew


def check(path):
    board = pcbnew.LoadBoard(path)
    if not board:
        raise ValueError("Cannot load board")
    fps = {f.GetReference(): f for f in board.GetFootprints()}
    tracks = board.Tracks()
    edges = {}
    ids = set()
    for i in range(len(tracks)):
        track = tracks[i]
        uid = track.m_Uuid.AsString()
        if uid in ids:
            raise ValueError("Duplicate copper object ID")
        ids.add(uid)
        if track.GetClass() != "PCB_TRACK" or track.GetLayer() != pcbnew.F_Cu:
            continue
        ends = tuple(sorted(((track.GetStart().x, track.GetStart().y),
                             (track.GetEnd().x, track.GetEnd().y))))
        key = (track.GetNetname(), ends)
        edges[key] = max(edges.get(key, 0), track.GetWidth())
    lengths = {}
    for cap_pin, regulator_pin, name in (("1", "2", "VIN"), ("2", "1", "GND")):
        pads = [fps["C_IN2"].FindPadByNumber(cap_pin),
                fps["C_INHF1"].FindPadByNumber(cap_pin),
                fps["U_BUCK1"].FindPadByNumber(regulator_pin)]
        if any(pad is None for pad in pads):
            raise ValueError(f"{name}: missing pad")
        net = pads[0].GetNetname()
        if not net or any(pad.GetNetname() != net for pad in pads):
            raise ValueError(f"{name}: wrong input-loop net")
        points = [(p.GetPosition().x, p.GetPosition().y) for p in pads]
        length = 0
        for start, end in zip(points, points[1:]):
            key = (net, tuple(sorted((start, end))))
            if edges.get(key, 0) < 600000:
                raise ValueError(f"{name}: missing direct F.Cu path or width below 0.60 mm")
            length += math.dist(start, end) / 1e6
        # Project-local routing bound, not a manufacturer limit or current rating.
        if length > 8:
            raise ValueError(f"{name}: input path exceeds this draft's 8 mm bound")
        lengths[name] = length
    return lengths


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    try:
        lengths = check(sys.argv[1])
        print(f"PASS: direct F.Cu input paths: {lengths}; no supply-rating claim")
    except (OSError, KeyError, ValueError) as error:
        sys.exit(f"FAIL: {error}")
