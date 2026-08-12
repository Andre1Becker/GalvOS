#!/usr/bin/env python3
"""
GalvOS per-channel color-ramp linearity calibration.

Host: Windows 11, OV9281 global-shutter USB camera (mono, global shutter, UVC).
Target: GalvOS ESP32-S3 controller via REST (/api/calib-pattern, calibRampR/G/B).

Drives the firmware's 3 color-ramp calibration patterns (calib_patterns.cpp, idx
18/19/20) one channel at a time -- 32 equal-width fields, PWM duty 0..255 linear
left to right, with gain/dimmer/gamma/threshold all bypassed on the firmware side
(gState.calib_raw_duty) so the commanded duty IS the wire duty -- and measures
per-field mean camera luminance through whatever welding-glass ND filter sits
between the laser and the OV9281.

Unlike optimizeGalvo.py's measure/optimize/diagnose commands, this needs no
pixel<->DAC homography: only the RATIO between fields matters (relative
linearity, not absolute position), so a manually-clicked 4-corner rectangle
around the ramp (same click picker optimizeGalvo.py's 'calibrate-warp' uses) is
enough to rectify perspective and split the result into 32 equal column bins.
The ramp geometry is identical for all 3 channels, so the rectangle is only
clicked once, against whichever channel is measured first.

Independent of optimizeGalvo.py's own subcommand suite -- imports its Camera/
EspClient/LiveView/target-rect-picker machinery (same camConfig.json, same
manual-exposure handling) instead of duplicating camera setup.

Usage:
  python calibrateColor.py                              interactive: click ramp
                                                          corners once, measure R/G/B
  python calibrateColor.py --channels r,g                only measure red + green
  python calibrateColor.py --target-rect 100,80,540,260  skip the click picker
  python calibrateColor.py --no-view                     headless (no live window)

requirements: opencv-python, numpy, matplotlib, requests (see
scripts/optimizeGalvo/requirements.txt -- shares that file).
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

import cv2
import numpy as np

# ── reuse the shared camera/ESP32 machinery from optimizeGalvo.py instead of
# duplicating it -- same directory tree, no package __init__.py, so the module
# is imported directly off sys.path. optimizeGalvo.py is import-safe: its CLI
# only runs under `if __name__ == "__main__"`.
sys.path.insert(0, str(Path(__file__).resolve().parent / "optimizeGalvo"))
import optimizeGalvo as og  # noqa: E402

SCRIPT_VERSION = "1.0.0"

# Must match calib_patterns.h's CALIB_RAMP_BASE/CALIB_RAMP_FIELDS exactly -- the
# firmware draws the fields, this script only reads them back.
RAMP_FIELDS = 32
RAMP_IDX = {1: 18, 2: 19, 3: 20}   # channel (R=1/G=2/B=3) -> calib_patterns.cpp idx

CANVAS_COL_PX = 32                        # rectified pixels per field column
CANVAS_W = RAMP_FIELDS * CANVAS_COL_PX    # 1024
CANVAS_H = 256

SATURATION_LEVEL = 250        # raw sensor units considered "clipped"
SATURATION_WARN_FRAC = 0.05   # warn if > 5% of the brightest field's pixels clip
NOISE_FLOOR_MARGIN = 4.0      # lowest-nonzero field must clear background std by this multiple
PLATEAU_EPS_FRAC = 0.01       # consecutive fields within this fraction of the
                              # channel's own dynamic range count as a plateau
PLATEAU_MIN_RUN = 3           # minimum consecutive fields to report as a plateau


def _perspectiveMatrix(quad: np.ndarray) -> np.ndarray:
    """quad: [TL,TR,BR,BL] pixel corners (see og.orderCorners/_clickTargetRect).
    Maps them onto a straightened CANVAS_W x CANVAS_H rectangle so the 32 fields
    become exactly RAMP_FIELDS equal-width column bins, independent of the
    camera's viewing angle/rotation."""
    dst = np.array([[0, 0], [CANVAS_W - 1, 0],
                    [CANVAS_W - 1, CANVAS_H - 1], [0, CANVAS_H - 1]], dtype=np.float32)
    return cv2.getPerspectiveTransform(quad.astype(np.float32), dst)


def measureRamp(esp: "og.EspClient", cam: "og.Camera", cfg: dict, channel: int,
                perspective: np.ndarray, background: np.ndarray) -> dict:
    """Arms one channel's ramp, captures + rectifies it, and reduces the
    rectified image to per-field (mean luminance, saturation fraction)."""
    name = og.CALIB_CHANNEL_NAME[channel]
    idx = RAMP_IDX[channel]
    og.pr(f"  channel {name}: arming ramp (calib-pattern idx {idx}) ...")
    esp.calibPattern(idx, channel=0, bright=200, active=True)
    time.sleep(cfg["settleSeconds"])
    cam.statusText = f"calibrateColor: ramp {name}"
    raw = cam.grabAccumulated(cfg["accumFrames"])
    esp.stopCalibPattern()

    diff = cv2.subtract(raw, background)
    rectRaw = cv2.warpPerspective(raw, perspective, (CANVAS_W, CANVAS_H))
    rectDiff = cv2.warpPerspective(diff, perspective, (CANVAS_W, CANVAS_H))

    fieldsRaw = rectRaw.reshape(CANVAS_H, RAMP_FIELDS, CANVAS_COL_PX)
    fieldsDiff = rectDiff.reshape(CANVAS_H, RAMP_FIELDS, CANVAS_COL_PX)
    luminance = fieldsDiff.mean(axis=(0, 2)).astype(float)
    saturationFrac = (fieldsRaw >= SATURATION_LEVEL).mean(axis=(0, 2)).astype(float)

    duty = [round(i * 255 / (RAMP_FIELDS - 1)) for i in range(RAMP_FIELDS)]
    return {"channel": name, "idx": idx, "duty": duty,
           "luminance": luminance.tolist(), "saturationFrac": saturationFrac.tolist()}


def analyzeRamp(result: dict, backgroundStd: float) -> dict:
    """Saturation/noise-floor guard + plateau/non-monotonic diagnostics (the
    signal for PWM-duty->brightness collapse this tool exists to catch) plus
    the per-channel welding-glass transmission estimate (topmost/duty-255
    field's measured luminance as a fraction of the full 0..255 duty scale)."""
    means = np.array(result["luminance"])
    duty = result["duty"]
    warnings = []

    satFracTop = result["saturationFrac"][-1]
    if satFracTop > SATURATION_WARN_FRAC:
        warnings.append(
            f"brightest field (duty {duty[-1]}) is {satFracTop * 100:.0f}% sensor-"
            f"saturated -- reduce exposure or use stronger welding glass")

    lowestNonzero = means[1]   # duty index 0 is always 0 by construction; index 1
                                # is the lowest field that should show any signal
    noiseFloor = NOISE_FLOOR_MARGIN * max(backgroundStd, 0.5)
    if lowestNonzero < noiseFloor:
        warnings.append(
            f"lowest nonzero field (duty {duty[1]}) reads {lowestNonzero:.1f}, too "
            f"close to the background noise floor (std {backgroundStd:.1f}) -- "
            f"increase exposure or use weaker welding glass")

    diffs = np.diff(means)
    nonMonotonicAt = [i + 1 for i, d in enumerate(diffs) if d < -1.0]

    dynRange = max(float(means.max() - means.min()), 1e-6)
    plateauEps = PLATEAU_EPS_FRAC * dynRange
    plateaus: list[tuple[int, int]] = []
    runStart = None
    for i, d in enumerate(diffs):
        if abs(d) <= plateauEps:
            if runStart is None:
                runStart = i
        else:
            if runStart is not None and (i - runStart) >= (PLATEAU_MIN_RUN - 1):
                plateaus.append((runStart, i))
            runStart = None
    if runStart is not None and (len(diffs) - runStart) >= (PLATEAU_MIN_RUN - 1):
        plateaus.append((runStart, len(diffs)))

    if nonMonotonicAt:
        warnings.append(f"non-monotonic at duty step(s) {[duty[i] for i in nonMonotonicAt]} "
                        f"-- brightness DROPS as commanded duty rises, a sign of intensity "
                        f"collapse (driver current-limit fold-back or laser thermal rollback)")
    for a, b in plateaus:
        warnings.append(f"plateau across duty {duty[a]}..{duty[b]} -- brightness stops "
                        f"tracking duty over this range")

    return {"warnings": warnings, "nonMonotonicAt": nonMonotonicAt,
           "plateaus": plateaus, "transmission": float(means[-1] / 255.0)}


def printResults(results: dict[int, dict], analysis: dict[int, dict]):
    channels = sorted(results)
    names = [og.CALIB_CHANNEL_NAME[c] for c in channels]
    og.pr()
    og.pr(og.bold("=== calibrateColor: duty -> measured luminance ==="))
    header = "duty  " + "".join(f"{n:>10}" for n in names)
    og.pr(header)
    for i in range(RAMP_FIELDS):
        row = f"{results[channels[0]]['duty'][i]:>4}  "
        row += "".join(f"{results[c]['luminance'][i]:>10.1f}" for c in channels)
        og.pr(row)
    og.pr()
    rows = [(og.CALIB_CHANNEL_NAME[c], f"{analysis[c]['transmission'] * 100:.1f}%")
           for c in channels]
    og.prTable(rows, headers=("channel", "transmission (duty-255 field / 255)"))
    for c in channels:
        for w in analysis[c]["warnings"]:
            og.prWarn(f"channel {og.CALIB_CHANNEL_NAME[c]}: {w}")
        if not analysis[c]["warnings"]:
            og.prOk(f"channel {og.CALIB_CHANNEL_NAME[c]}: no plateau/clipping/noise-floor "
                   f"issues detected")


def plotRamp(results: dict[int, dict], analysis: dict[int, dict], outPath: Path):
    import matplotlib
    matplotlib.use("Agg")   # headless -- this script never needs an interactive plot window
    import matplotlib.pyplot as plt

    style = {1: ("R", "#e03131"), 2: ("G", "#2f9e44"), 3: ("B", "#1971c2")}
    fig, ax = plt.subplots(figsize=(8, 5))
    duty = results[next(iter(results))]["duty"]
    peakLuminance = max(max(r["luminance"]) for r in results.values())

    for c, result in sorted(results.items()):
        name, color = style[c]
        ax.plot(duty, result["luminance"], marker="o", markersize=3, color=color,
                label=f"{name} (transmission {analysis[c]['transmission'] * 100:.0f}%)")
        for i in analysis[c]["nonMonotonicAt"]:
            ax.plot(duty[i], result["luminance"][i], marker="x", markersize=9,
                    markeredgewidth=2, color="black", zorder=5)

    if peakLuminance > 0:
        idealScale = peakLuminance / 255.0
        ax.plot(duty, [d * idealScale for d in duty], "--", color="gray",
                linewidth=1, label="ideal linear")

    ax.set_xlabel("commanded PWM duty (0-255)")
    ax.set_ylabel("measured luminance (camera units)")
    ax.set_title("GalvOS color-ramp linearity")
    ax.legend(loc="upper left", fontsize=8)
    ax.grid(alpha=0.3)
    fig.tight_layout()
    fig.savefig(outPath, dpi=150)
    plt.close(fig)


def runCalibrateColor(cfg: dict, esp: "og.EspClient", cam: "og.Camera",
                      channels: list[int], targetRect) -> None:
    og.requireLaserReady(esp)

    og.pr("capturing background (laser off) ...")
    esp.stopCalibPattern()
    time.sleep(0.3)
    background = cam.grabBackground()
    backgroundStd = float(np.std(background))

    firstChannel = channels[0]
    og.pr(og.bold(f"\narming the {og.CALIB_CHANNEL_NAME[firstChannel]} ramp so its "
                 f"rectangle is visible for corner selection ..."))
    esp.calibPattern(RAMP_IDX[firstChannel], channel=0, bright=200, active=True)
    time.sleep(cfg["settleSeconds"])
    og.waitWhilePaused(cam)
    # _resolveTargetRect: click-4-corners picker (interactive) or a straight
    # --target-rect rectangle (non-interactive) -- same helper 'calibrate-warp'
    # uses. Geometry is identical for all 3 ramps, so this is done once.
    quad = og._resolveTargetRect(cam, targetRect)
    esp.stopCalibPattern()
    perspective = _perspectiveMatrix(quad)

    results: dict[int, dict] = {}
    for c in channels:
        og.waitWhilePaused(cam)
        results[c] = measureRamp(esp, cam, cfg, c, perspective, background)

    analysis = {c: analyzeRamp(results[c], backgroundStd) for c in channels}
    printResults(results, analysis)

    try:
        og.RESULTS_DIR.mkdir(exist_ok=True)
        timestamp = time.strftime("%Y-%m-%d_%H-%M-%S")
        plotPath = og.RESULTS_DIR / f"colorRamp_{timestamp}.png"
        plotRamp(results, analysis, plotPath)
        og.prOk(f"saved plot -> {plotPath.relative_to(Path(__file__).resolve().parent)}")

        jsonPath = og.RESULTS_DIR / f"colorRamp_{timestamp}.json"
        jsonPath.write_text(json.dumps({
            "timestamp": timestamp,
            "channels": [og.CALIB_CHANNEL_NAME[c] for c in channels],
            "backgroundStd": backgroundStd,
            "results": {og.CALIB_CHANNEL_NAME[c]: results[c] for c in results},
            "analysis": {og.CALIB_CHANNEL_NAME[c]: analysis[c] for c in analysis},
        }, indent=2))
        og.prOk(f"saved data -> {jsonPath.relative_to(Path(__file__).resolve().parent)}")
    except OSError as e:
        og.prWarn(f"could not save results: {e}")


def main():
    og._enableWindowsAnsi()
    parser = argparse.ArgumentParser(
        prog="calibrateColor.py",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description="GalvOS per-channel color-ramp (duty->luminance) linearity calibration.",
        epilog=f"calibrateColor.py v{SCRIPT_VERSION} -- shares camConfig.json/results/ "
              f"with optimizeGalvo.py (scripts/optimizeGalvo/).",
    )
    parser.add_argument("--version", action="version",
                        version=f"calibrateColor.py v{SCRIPT_VERSION}")
    parser.add_argument("--config", metavar="PATH",
                        help="path to camConfig.json (default: scripts/optimizeGalvo/"
                             "camConfig.json, shared with optimizeGalvo.py)")
    parser.add_argument("--channels", default="r,g,b",
                        help="comma-separated subset of r,g,b to measure (default: all three)")
    parser.add_argument("--target-rect", dest="targetRect", metavar="X0,Y0,X1,Y1",
                        help="axis-aligned pixel rectangle around the ramp, skips the "
                             "interactive click picker (required for non-interactive runs)")
    parser.add_argument("--no-view", action="store_true", dest="noView",
                        help="disable the live camera view window")
    parser.add_argument("--zoom", type=int, choices=[1, 2, 3], default=1,
                        help="initial digital zoom level for the camera view window")
    parser.add_argument("--debug", action="store_true", help="full traceback on error")
    args = parser.parse_args()

    if args.config:
        og.CONFIG_FILE = Path(args.config)

    try:
        cfg = og.loadConfig()
        channels = og._parseColorChannels(args.channels)
        targetRect = og._parseTargetRect(args.targetRect)

        esp = og.EspClient.fromConfig(cfg)
        showView = cfg.get("showCameraView", True) and not args.noView
        liveView = og.LiveView("GalvOS color-ramp calibration", cfg["frameWidth"],
                               cfg["frameHeight"], zoomIdx=args.zoom - 1) if showView else None
        og.pr("camera view: " + (liveView.hotkeys if liveView
                                 else "disabled (--no-view or showCameraView=false)"))
        cam = og.Camera(cfg, liveView=liveView)
        try:
            runCalibrateColor(cfg, esp, cam, channels, targetRect)
            if liveView:
                og.holdLiveView(cam, cfg.get("resultViewHoldSeconds",
                                             og.MIN_RESULT_VIEW_HOLD_SECONDS))
        finally:
            try:
                esp.stopCalibPattern()
            except og.OptimizerError:
                pass
            if liveView:
                liveView.close()
            cam.release()
    except og.OptimizerError as e:
        og.prWarn(e, file=sys.stderr)
        sys.exit(1)
    except (KeyboardInterrupt, EOFError):
        og.pr(file=sys.stderr)
        og.prWarn("interrupted", file=sys.stderr)
        sys.exit(130)
    except Exception as e:
        if args.debug:
            raise
        og.prWarn(f"unexpected {type(e).__name__}: {e}", file=sys.stderr)
        og.prTip("re-run with --debug for a full traceback", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
