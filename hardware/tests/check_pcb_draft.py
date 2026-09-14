#!/usr/bin/env python3
"""Validate draft DRC evidence, NOT fabrication readiness.

Usage: python hardware/tests/check_pcb_draft.py DRC.json PROJECT.kicad_pro
Run native KiCad DRC with --schematic-parity --severity-all first.
Unconnected items and warnings are reported, not waived as a release pass.
"""

import json
import sys
from collections import Counter

from check_dac_interface import require


REQUIRED_CHECKS = {
    "clearance", "shorting_items", "tracks_crossing", "unconnected_items",
    "courtyards_overlap", "missing_courtyard", "invalid_outline",
    "footprint_filters_mismatch", "footprint_type_mismatch",
    "footprint_symbol_mismatch", "footprint_symbol_field_mismatch",
    "track_not_centered_on_via", "silk_overlap", "silk_over_copper",
}

# Provisional project minima, not a fabricator or current-capacity approval.
MINIMA = {
    "min_clearance": 0.2,
    "min_track_width": 0.2,
    "min_via_diameter": 0.6,
    "min_through_hole_diameter": 0.3,
    "min_copper_edge_clearance": 0.5,
    "min_silk_clearance": 0.1,
}

# These native warnings remain visible and block production release.
KNOWN_PACKAGE_WARNINGS = {
    ("footprint_type_mismatch", "Footprint U_BUCK1"),
    ("footprint_filters_mismatch", "Footprint D_TRIGCL_SCAN1"),
    ("footprint_filters_mismatch", "Footprint D_TRIGCL_WD1"),
}


def check(report, project):
    settings = project["board"]["design_settings"]
    require(not settings["drc_exclusions"], "Project contains DRC exclusions")
    require(settings["rule_severities"]["unconnected_items"] == "error",
            "Unconnected-item checks must remain errors")
    for name in REQUIRED_CHECKS:
        require(settings["rule_severities"][name] in ("warning", "error"),
                f"Project disables required check: {name}")
    for name, minimum in MINIMA.items():
        require(settings["rules"][name] >= minimum,
                f"Project minimum relaxed: {name}")
    require(set(report["included_severities"]) == {"error", "warning", "exclusion"},
            "Report must include all severities")
    ignored = {item["key"] for item in report["ignored_checks"]}
    require(not ignored.intersection(REQUIRED_CHECKS),
            "Native DRC skipped a required check; regenerate with current project")
    require(all(isinstance(report[key], list) for key in
                ("violations", "unconnected_items", "schematic_parity")),
            "Incomplete native report")
    findings = report["violations"] + report["schematic_parity"]
    require(all(item["severity"] == "warning" for item in findings),
            "PCB has non-routing errors or excluded findings; inspect native report")
    require(all(len(item["items"]) == 1 and
                (item["type"], item["items"][0]["description"]) in KNOWN_PACKAGE_WARNINGS
                for item in findings),
            "Unexpected PCB warning; inspect native report before accepting draft")
    require(all(item["severity"] == "error" for item in report["unconnected_items"]),
            "Unconnected items must remain enabled as errors")
    return len(report["unconnected_items"]), Counter(item["type"] for item in findings)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    try:
        with open(sys.argv[1]) as stream:
            report = json.load(stream)
        with open(sys.argv[2]) as stream:
            project = json.load(stream)
        unconnected, warnings = check(report, project)
        print(f"PASS: draft evidence only; {unconnected} unconnected items; "
              f"warnings={dict(warnings)}; NOT a release approval")
    except (OSError, ValueError, KeyError, TypeError) as error:
        sys.exit(f"FAIL: {error}")
