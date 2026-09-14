#!/usr/bin/env python3
"""Check native KiCad SMD CSV coverage, NOT placement geometry or release.

Export with --format csv --units mm --side both --smd-only --exclude-dnp.
Do not use --exclude-fp-th: it drops SMD packages with plated pad holes.
"""

import csv
from pathlib import Path
import sys


def expected_parts(board_path):
    # Lazy import keeps the CSV contract tests independent of KiCad bindings.
    import pcbnew

    if not Path(board_path).is_file():
        raise ValueError(f"Missing board: {board_path}")
    board = pcbnew.LoadBoard(str(board_path))
    if board is None:
        raise ValueError("Cannot load board")
    expected = {}
    seen = set()
    for footprint in board.GetFootprints():
        ref = footprint.GetReference()
        if not ref or ref in seen:
            raise ValueError(f"Missing/duplicate board reference: {ref}")
        seen.add(ref)
        attributes = footprint.GetAttributes()
        if not attributes & pcbnew.FP_SMD:
            continue
        if attributes & (pcbnew.FP_DNP | pcbnew.FP_EXCLUDE_FROM_POS_FILES):
            continue
        expected[ref] = (
            footprint.GetValue(),
            str(footprint.GetFPID().GetLibItemName()),
            "bottom" if footprint.IsFlipped() else "top",
        )
    return expected


def check_rows(rows, expected):
    if not expected:
        raise ValueError("No populated SMD parts in board inventory")
    actual = {}
    for row in rows:
        if any(row.get(key) is None for key in ("Ref", "Val", "Package", "Side")):
            raise ValueError("Missing native KiCad CSV coverage fields")
        ref = row["Ref"]
        if not ref or ref in actual:
            raise ValueError(f"Missing/duplicate CSV reference: {ref}")
        actual[ref] = row["Val"], row["Package"], row["Side"]
    missing = sorted(expected.keys() - actual.keys())
    extra = sorted(actual.keys() - expected.keys())
    if missing or extra:
        raise ValueError(f"SMD coverage mismatch: missing={missing}, extra={extra}")
    for ref in expected:
        if actual[ref] != expected[ref]:
            raise ValueError(f"{ref}: value, package or side mismatch")
    return len(actual)


def check(csv_path, board_path):
    expected = expected_parts(board_path)
    with open(csv_path, newline="", encoding="utf-8-sig") as stream:
        return check_rows(csv.DictReader(stream), expected)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("Usage: /usr/bin/python check_smd_placement_coverage.py CSV BOARD")
    try:
        count = check(*sys.argv[1:])
        print(f"PASS: {count} SMD placements covered; NOT geometry/release approval")
    except (OSError, ValueError, csv.Error) as error:
        sys.exit(f"FAIL: {error}")
