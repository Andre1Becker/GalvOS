#!/usr/bin/env python3
"""streak.py -- independent cross-check for the v6.75.0 blankLeakage claim.

Sweeps blank_samples on the 'segments' calibration pattern (MultiObject profile)
and, for each capture, prints THREE numbers side by side:

  - blankLeakage           the ORIGINAL metric (still summed into `cost` at
                            weight 2.0 in optimizeGalvo.py's computeMetrics(),
                            see costWeights). v6.75.0's commit claimed this
                            "stays, but nothing load-bearing rests on it any
                            more" -- untrue if the weight is still nonzero.
  - blankCorridorLitPct    the CURRENT (v2.25.0) load-bearing anti-streak
                            metric, computeMetrics()'s own replacement.
  - directCorridorMean     a from-scratch re-implementation: same ideal gap
                            polylines (ground-truth geometry, not the metric
                            under test) from idealPolylines(), but its own
                            mask-building (plain cv2.line, fixed width) and
                            its own statistic (mean warped-DAC brightness in
                            that mask) -- does not call _darkCorridorMaskFor()
                            or gapMask, so it can't inherit either one's bugs.

A metric that tracks real streaking should FALL as blank_samples rises (more
settle time before the beam turns back on -> less bleed into the gap). One
that's anti-correlated (like the original blankLeakage, per the v6.75.0
commit's own 0.947->2.56 numbers) RISES instead.

Requires: homography.npz (run 'calibrate' first), laser armed, E-Stop/scan-
fail clear. Never arms the laser itself -- same convention as
requireLaserReady() in optimizeGalvo.py.
"""
import sys
import time
from pathlib import Path

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
import optimizeGalvo as og

PATTERN = "segments"
BLANK_SAMPLES_SWEEP = [1, 4, 10, 25, 60, 100]
CORRIDOR_HALF_WIDTH_PX = 5  # cv2.line thickness/2 in DAC-canvas px, independent
                            # of _darkCorridorMaskFor's own guard/margin logic


def directCorridorMean(dacImage: np.ndarray, gaps: list[np.ndarray], r: int) -> float:
    """Mean brightness of dacImage along the ideal blank-gap polylines, using a
    hand-rolled mask (cv2.line) instead of computeMetrics()'s gapMask/darkMask
    machinery. gaps: list of (2,2) arrays in DAC-unit coords, as returned by
    idealPolylines()'s second element."""
    scale = og.CANVAS / (2 * r)
    mask = np.zeros(dacImage.shape[:2], dtype=np.uint8)
    for seg in gaps:
        (x0, y0), (x1, y1) = seg
        p0 = (int((x0 + r) * scale), int((y0 + r) * scale))
        p1 = (int((x1 + r) * scale), int((y1 + r) * scale))
        cv2.line(mask, p0, p1, 255, thickness=CORRIDOR_HALF_WIDTH_PX * 2)
    if not mask.any():
        return float("nan")
    return float(dacImage[mask > 0].mean())


def main():
    cfg = og.loadConfig()
    esp = og.EspClient.fromConfig(cfg)
    og.requireLaserReady(esp, allowUnarmed=False)
    cam = og.Camera(cfg)
    try:
        cfg = og.resolveExposure(cfg, esp, cam, [PATTERN])
        homography, background = og.loadHomography()
        r = cfg["dacRange"]
        _, gaps = og.idealPolylines(PATTERN, r)
        if not gaps:
            raise og.OptimizerError(f"{PATTERN} has no ideal gap geometry to sweep")

        rows = []
        print(f"{'blank_samples':>13} | {'blankLeakage':>12} | "
              f"{'corridorLitPct':>14} | {'directMean':>10} | valid")
        print("-" * 70)
        for bs in BLANK_SAMPLES_SWEEP:
            esp.startPattern(PATTERN, channel=cfg["camPatternChannel"])
            time.sleep(cfg["patternSwitchSettleSeconds"])
            effective = esp.setParams({"blank_samples": bs})
            if effective.get("ignored"):
                raise og.OptimizerError(f"ESP32 ignored blank_samples override: {effective}")
            time.sleep(cfg["settleSeconds"])
            cam.statusText = f"streak: {PATTERN} blank_samples={bs}"
            capture = cam.grabAccumulated(cfg["accumFrames"])
            esp.stop()
            time.sleep(cfg["patternSwitchSettleSeconds"])

            metrics, debug = og.computeMetrics(capture, background, homography, PATTERN, cfg)
            dacImage = debug["dacImage"]
            direct = directCorridorMean(dacImage, gaps, r)

            rows.append((bs, metrics.blankLeakage, metrics.blankCorridorLitPct, direct, metrics.valid))
            print(f"{bs:>13} | {metrics.blankLeakage:>12.3f} | "
                  f"{metrics.blankCorridorLitPct:>14.3f} | {direct:>10.3f} | {metrics.valid}")

        def trend(vals):
            diffs = np.diff(vals)
            if np.all(diffs <= 0):
                return "monotonic DOWN (correlates with cleaner beam)"
            if np.all(diffs >= 0):
                return "monotonic UP (anti-correlates with cleaner beam)"
            return "non-monotonic"

        bsVals = [row[0] for row in rows]
        print()
        print(f"blankLeakage      trend: {trend([row[1] for row in rows])} "
              f"({rows[0][1]:.3f} -> {rows[-1][1]:.3f})")
        print(f"blankCorridorLitPct trend: {trend([row[2] for row in rows])} "
              f"({rows[0][2]:.3f} -> {rows[-1][2]:.3f})")
        print(f"directCorridorMean trend: {trend([row[3] for row in rows])} "
              f"({rows[0][3]:.3f} -> {rows[-1][3]:.3f})")
    finally:
        try:
            esp.stop()
        except og.OptimizerError:
            pass
        cam.cap.release()


if __name__ == "__main__":
    main()
