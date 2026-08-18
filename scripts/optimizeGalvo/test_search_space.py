#!/usr/bin/env python3
"""
test_search_space.py -- host-side regression test for mergeSearchSpace()
(scripts/optimizeGalvo/optimizeGalvo.py). No device, no camera, no NVS - pure
dict-in/dict-out logic. Run with:

  python3 test_search_space.py
  python3 -m unittest test_search_space

Covers the merge contract required by docs/optimizer-range-audit-2026-08-17.md
block 0: an existing file's own tuned values (whole profiles and individual
per-parameter ranges) survive byte-for-byte; only genuinely absent keys are
filled in from defaultSearchSpace(); a file that already has everything is
left unchanged.
"""

import copy
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import optimizeGalvo as og  # noqa: E402


class MergeSearchSpaceTests(unittest.TestCase):
    def setUp(self):
        # Real defaultSearchSpace() output, not a hand-rolled stand-in - if the
        # shape of that function ever changes, this test should feel it too.
        self.defaults = og.defaultSearchSpace()

    def test_existing_values_survive_byte_for_byte(self):
        """A user's own tuned min/max on a parameter that already exists in both
        the file and the defaults must not be touched, even if it differs from
        (or is wildly outside) the shipped default range."""
        existing = {
            "Vector": {
                "patterns": ["square", "star"],
                "params": {
                    "corner_angle_deg": {"type": "float", "min": 99.0, "max": 100.0},
                },
            },
        }
        untouchedSnapshot = copy.deepcopy(existing)
        merged, changes = og.mergeSearchSpace(existing, self.defaults)

        self.assertEqual(
            merged["Vector"]["params"]["corner_angle_deg"],
            {"type": "float", "min": 99.0, "max": 100.0},
        )
        # mergeSearchSpace must not mutate its input in place either.
        self.assertEqual(existing, untouchedSnapshot)
        # corner_angle_deg itself was untouched, but Vector has other default
        # params this file doesn't - those are legitimately new additions, and
        # none of them should be reported as touching corner_angle_deg.
        self.assertTrue(len(changes) > 0)
        for c in changes:
            self.assertNotIn("Vector.corner_angle_deg", c)

    def test_missing_profile_is_added_whole(self):
        """A file predating a profile (e.g. Wireframe/Text/Particles landing in
        fw v6.75.0) gets that whole block copied in from defaults."""
        existing = {
            "Vector": self.defaults["Vector"],
        }
        merged, changes = og.mergeSearchSpace(existing, self.defaults)

        for name in ("Wireframe", "Text", "Particles"):
            self.assertIn(name, merged)
            self.assertEqual(merged[name], self.defaults[name])
            self.assertTrue(any(f"'{name}'" in c for c in changes))

    def test_missing_param_in_existing_profile_is_added(self):
        """A profile the file already has, but that gained a parameter later,
        gets just that parameter filled in - not the whole block replaced."""
        existing = {
            "Vector": {
                "patterns": ["square", "star"],
                "params": {
                    "corner_angle_deg": {"type": "float", "min": 12.0, "max": 55.0},
                },
            },
        }
        merged, changes = og.mergeSearchSpace(existing, self.defaults)

        # The one param that was already there keeps its user-tuned range.
        self.assertEqual(
            merged["Vector"]["params"]["corner_angle_deg"],
            {"type": "float", "min": 12.0, "max": 55.0},
        )
        # Every other Vector param in defaultSearchSpace() got filled in.
        for paramName, spec in self.defaults["Vector"]["params"].items():
            if paramName == "corner_angle_deg":
                continue
            self.assertEqual(merged["Vector"]["params"][paramName], spec)
            self.assertTrue(
                any(f"Vector.{paramName}" in c for c in changes),
                f"expected a change entry for Vector.{paramName}, got {changes}",
            )

    def test_complete_file_is_left_unchanged(self):
        """A file that already has every profile and every parameter (mirrors the
        real searchSpace.json on this machine for Vector/Smooth/Waves/MultiObject
        before Wireframe/Text/Particles existed) reports no changes and comes back
        identical - not merely equal, but the same values, unrewritten."""
        existing = copy.deepcopy(self.defaults)
        merged, changes = og.mergeSearchSpace(existing, self.defaults)

        self.assertEqual(changes, [])
        self.assertEqual(merged, self.defaults)

    def test_malformed_profile_entry_is_left_alone(self):
        """A profile key that isn't a dict (hand-edited into something broken) is
        not this function's job to repair - it must not raise, and must not touch
        that entry, while still merging every other profile normally."""
        existing = {"Vector": "not a dict"}
        merged, changes = og.mergeSearchSpace(existing, self.defaults)

        self.assertEqual(merged["Vector"], "not a dict")
        self.assertIn("Smooth", merged)  # other profiles still merged in


if __name__ == "__main__":
    unittest.main()
