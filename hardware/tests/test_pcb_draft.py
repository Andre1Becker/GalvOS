"""Negative controls for the draft evidence gate; no KiCad process required."""

import copy
import unittest

from check_pcb_draft import MINIMA, REQUIRED_CHECKS, check


class DraftEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.project = {"board": {"design_settings": {
            "drc_exclusions": [],
            "rule_severities": dict.fromkeys(REQUIRED_CHECKS, "error"),
            "rules": dict(MINIMA),
        }}}
        self.report = {
            "included_severities": ["error", "warning", "exclusion"],
            "ignored_checks": [], "violations": [], "schematic_parity": [],
            "unconnected_items": [{"severity": "error"}],
        }

    def test_reports_incomplete_routing(self):
        unconnected, warnings = check(self.report, self.project)
        self.assertEqual(unconnected, 1)
        self.assertFalse(warnings)

    def test_rejects_disabled_or_stale_checks(self):
        for name in REQUIRED_CHECKS:
            with self.subTest(name=name):
                project = copy.deepcopy(self.project)
                project["board"]["design_settings"]["rule_severities"][name] = "ignore"
                with self.assertRaises(ValueError):
                    check(self.report, project)
                report = copy.deepcopy(self.report)
                report["ignored_checks"] = [{"key": name}]
                with self.assertRaises(ValueError):
                    check(report, self.project)

    def test_rejects_weakened_minima(self):
        for name in MINIMA:
            with self.subTest(name=name):
                project = copy.deepcopy(self.project)
                project["board"]["design_settings"]["rules"][name] = 0
                with self.assertRaises(ValueError):
                    check(self.report, project)

    def test_rejects_copper_and_parity_errors(self):
        for section in ("violations", "schematic_parity"):
            report = copy.deepcopy(self.report)
            report[section] = [{"severity": "error", "type": "shorting_items"}]
            with self.assertRaises(ValueError):
                check(report, self.project)

    def test_rejects_hidden_unconnected_items(self):
        self.report["unconnected_items"][0]["severity"] = "exclusion"
        with self.assertRaises(ValueError):
            check(self.report, self.project)
        self.report["unconnected_items"][0]["severity"] = "error"
        self.project["board"]["design_settings"]["rule_severities"]["unconnected_items"] = "warning"
        with self.assertRaises(ValueError):
            check(self.report, self.project)

    def test_rejects_unexpected_warning(self):
        self.report["violations"] = [{
            "severity": "warning", "type": "isolated_copper",
            "items": [{"description": "Zone [<no net>] on B.Cu"}],
        }]
        with self.assertRaises(ValueError):
            check(self.report, self.project)

    def test_reports_known_warning_without_hiding_it(self):
        self.report["violations"] = [{
            "severity": "warning", "type": "footprint_type_mismatch",
            "items": [{"description": "Footprint U_BUCK1"}],
        }]
        unconnected, warnings = check(self.report, self.project)
        self.assertEqual(unconnected, 1)
        self.assertEqual(warnings, {"footprint_type_mismatch": 1})

    def test_rejects_resolved_diode_package_warnings(self):
        for ref in ("D_TRIGCL_SCAN1", "D_TRIGCL_WD1"):
            with self.subTest(ref=ref):
                self.report["schematic_parity"] = [{
                    "severity": "warning", "type": "footprint_filters_mismatch",
                    "items": [{"description": f"Footprint {ref}"}],
                }]
                with self.assertRaises(ValueError):
                    check(self.report, self.project)

    def test_rejects_exclusions(self):
        self.project["board"]["design_settings"]["drc_exclusions"] = ["test exclusion"]
        with self.assertRaises(ValueError):
            check(self.report, self.project)

    def test_rejects_filtered_or_incomplete_report(self):
        self.report["included_severities"] = ["error"]
        with self.assertRaises(ValueError):
            check(self.report, self.project)
        self.report["included_severities"] = ["error", "warning", "exclusion"]
        del self.report["schematic_parity"]
        with self.assertRaises(KeyError):
            check(self.report, self.project)


if __name__ == "__main__":
    unittest.main()
