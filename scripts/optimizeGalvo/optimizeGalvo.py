#!/usr/bin/env python3
"""
GalvOS camera-in-the-loop optimizer profile tuner.

Host: Windows 11, OV9281 global-shutter USB camera (mono, 720p, 120 fps).
Target: GalvOS ESP32-S3 controller via REST (/api/calib-cam/*, /api/status).

---

## Workflow

  0. wizard     - first-time setup (runs automatically if no config exists yet)
  1. check      - verify the ESP32 controller is reachable (do this first)
  2. preview    - live camera view to set focus / exposure / ND filter
  3. calibrate  - project 4 reference dots, compute pixel->DAC homography
  4. measure    - run one pattern with current params, print metrics
  5. optimize   - Optuna loop: propose params -> POST to ESP32 -> capture
                  -> score -> repeat; best params written to JSON, resumable
                  via persistent SQLite storage
  6. diagnose   - measure currently-live output, classify as OK / geometry
                  problem / optimizer-settings problem, optionally autotune
                  (= run 'optimize') on whatever's flagged fixable
  7. autotune-camera - Optuna loop over the camera's own capture settings
                  (exposure, gain, binaryThreshold, accumFrames) instead of
                  firmware parameters; run this once after 'preview' if the
                  hand-picked exposure/threshold aren't visibly reliable
  8. autotune-colors - camera-measured RGB visibility-threshold ("Basiswert")
                  + gain/brightness matching, automating the WebUI's two
                  manual per-channel calibration tools. No homography needed.
  9. analyze-live - structural (no-reference) read of whatever preset is
                  actually live right now - unlike measure/diagnose/optimize
                  this never starts/stops a pattern, so it works on any
                  preset, not just the 6 calib patterns. Flags a beam trace
                  that doesn't form one continuous piece, or doesn't close
                  when its category says it should, and always saves a
                  screenshot either way.
 10. calibrate-warp - solves the firmware's /api/warp/* keystone-correction
                  grid: commands exact DAC positions (bypassing warp/
                  calibration), measures where they land vs. a target
                  rectangle, and POSTs the corrected grid. Independent of
                  'calibrate'/homography.npz above (different purpose).
 11. measure-resonance - sweeps one galvo axis 50-2000Hz (firmware-generated
                  sine drive, POST /api/debug/resonance) and reads the
                  driven streak's spatial extent per frequency to build a
                  Bode magnitude curve, extracting fRes/Q -> ring_freq_hz/
                  ring_damping_ratio for the ZV-shaper ringing compensation.
                  No homography needed - relative amplitude only.
 12. tune-dac-range - projects the static 'square' test pattern and camera-
                  closed-loop auto-tunes galvo_x/y_gain + galvo_x/y_offset
                  (POST /api/calib-live) so its bounding box just fills the
                  camera frame without running off it - shrinks a clipped
                  side, expands an underscanning axis, freezes once
                  converged. No homography needed.

---

## Usage

  python optimizeGalvo.py wizard
  python optimizeGalvo.py check
  python optimizeGalvo.py preview
  python optimizeGalvo.py calibrate
  python optimizeGalvo.py measure --pattern square
  python optimizeGalvo.py optimize --profile default --trials 60
  python optimizeGalvo.py optimize --preset "Milky Way" --trials 60
  python optimizeGalvo.py diagnose --autotune
  python optimizeGalvo.py autotune-camera --trials 30
  python optimizeGalvo.py autotune-colors
  python optimizeGalvo.py analyze-live
  python optimizeGalvo.py calibrate-warp --grid-size 3 --target-rect 100,80,1180,720
  python optimizeGalvo.py calibrate-warp --grid-size 2 --dry-run
  python optimizeGalvo.py measure-resonance --axis x
  python optimizeGalvo.py tune-dac-range
  python optimizeGalvo.py tune-dac-range --max-iterations 10 --dry-run
  python optimizeGalvo.py --config myRig.json check

---

## Camera view window (calibrate/measure/optimize/diagnose/autotune-camera)

Opens by default (disable with --no-view or showCameraView:false in the config).
Hotkeys while the window has focus:
  1 / 2 / 3   1x/2x/3x digital zoom (centered crop, rescaled - window size
              stays constant)
  s           save the current frame to results/snapshot_<timestamp>.png

[i] Run `python optimizeGalvo.py <command> --help` for details on any command.

---

## Requirements

opencv-python, numpy, optuna, requests (see requirements.txt).
If wheels for your Python version are not yet published, use a Python
3.12/3.13 venv.
"""

import argparse
import collections
import csv
import functools
import json
import math
import os
import sys
import textwrap
import time
from dataclasses import dataclass, field
from pathlib import Path

import cv2
import numpy as np
import requests

# ── versioning ───────────────────────────────────────────────────────────────
# Semantic version of this script (independent of GalvOS firmware version).
# Bump on every behavioral change; see git log for change history.
SCRIPT_VERSION = "2.20.0"

# GalvOS firmware version that introduced /api/calib-cam/* (see firmware git log:
# "fw: v6.03.0 -- camera-in-the-loop calibration API (calib-cam)").
MIN_FW_VERSION_CALIB_CAM = (6, 3, 0)

# GalvOS firmware version that introduced /api/warp/* (Prompt 7a - Camera
# Closed-Loop Keystone). 'calibrate-warp' also relies on /api/debug/hw and
# /api/config's galvo_x/y_gain/offset/invert_x/invert_y/swap_xy/output_scale
# fields, all of which predate this.
MIN_FW_VERSION_WARP = (6, 55, 0)

# GalvOS firmware version that introduced /api/debug/resonance (Prompt 13 -
# galvo resonance measurement). See docs/feature-prompts/DECISIONS.md, Prompt
# 13: HTTP round trips are far too slow to synthesize a sweep from the Python
# side, so the sine drive itself is generated firmware-side.
MIN_FW_VERSION_RESONANCE = (6, 61, 0)

# Hard floor for resultViewHoldSeconds (camConfig.json) - the camera view window must
# stay open at least this long after a command finishes, so the last capture is
# actually visible instead of the window vanishing instantly. The config value can
# raise this, never lower it (see validateConfig).
MIN_RESULT_VIEW_HOLD_SECONDS = 5.0

# Minimum connected-component area (DAC-canvas px) for a blob to count as a dot
# rather than sensor noise that survived binaryThreshold - see the isolated-dot
# metrics on Metrics.blobExpected. This rig's imaged beam is ~4 px across, so a real
# dot is on the order of 12-30 px; 6 is safely below that and safely above
# single-pixel noise. Overridable via camConfig.json's minBlobAreaPx.
DEFAULT_MIN_BLOB_AREA_PX = 6

# Firmware optimizer profile names, in index order - mirrors the OPT_PROFILE_*
# constants in GalvOS's include/config.h. The firmware API (GET /api/config)
# reports profiles by index only, not by name, so this list is how the script
# turns that into something readable. Keep in sync with config.h if it changes.
FIRMWARE_PROFILE_NAMES = (
    "Vector", "Smooth", "Waves", "Wireframe",
    "MultiObject", "Particles", "Trails", "Text",
)

# Which camera pattern(s) tune each firmware optimizer profile - mirrors
# calib_patterns.cpp's profileOf() for the "Cam ..." pattern set. Seven of the
# eight firmware profiles are camera-tunable. The two exceptions:
#   corners4  - the homography reference itself; it holds a fixed dwell
#               regardless of the live config (CALIB_CAM_DOT_DWELL_PTS), so it
#               is not a measurement of anything tunable. cam_particles is the
#               Particles profile's real probe.
#   Trails    - no camera pattern exists and none is planned: its ground truth
#               is a trajectory over time (a decaying tail), and a camera frame
#               is an accumulation over time with the time axis integrated
#               away. See docs/06-camera-autotuning.md, "Why Trails has no
#               camera pattern".
FW_PROFILE_PATTERNS = {
    "Vector":      ["square", "star"],
    "Smooth":      ["circle"],
    "Waves":       ["spiral"],
    "MultiObject": ["segments"],
    "Wireframe":   ["wireframe"],
    "Particles":   ["particles"],
    "Text":        ["text"],
}

# Preset category (from GET /api/presets' "cat") -> whether its geometry is expected
# to be a closed loop. Used only to word 'analyze-live's report, never as a hard
# pass/fail - every bucket has exceptions ("Three Circles" is Geometry but
# multi-object, "Grid 3x3" is Lines but multi-segment by design). Categories not
# listed here (Spirals, Curves, 3D, Complex, Combo, Party, Scenes, Timers, Symbols
# other than Yin Yang, ...) are intentionally left with no expectation either way.
CATEGORY_EXPECTS_CLOSED: dict[str, bool] = {
    "Geometry": True,
    "Symbols": True,
    "Lines": False,
    "Waves": False,
}

# Presets that are structurally several disjoint strokes/objects joined only by
# blank (laser-off) travel jumps - componentsAtFloor > 1 is the correct, expected
# read for these, not a beam-path defect. Downgrades analyze-live's "POSSIBLE GAP"
# to an informational NOTE for exactly these names (checked in runAnalyzeLive) -
# everything else still gets the full warning. Verified against
# preset_patterns.cpp: "Three Circles"/p107 (3 separate closed circles),
# "Confetti Burst"/p63 (NP separate closed particle polygons), "Grid 3x3"
# (independent line segments), "Starfield"/p90 (per-star blank-jump + dwell,
# no connecting stroke at all), "Multi Wave"/p37 (3 independently-optimized
# sinewave() curves), "Radial Waves"/p44 (4 concentric closed rings, NR separate
# PathSegments), "Wave Field"/p48 (5 open row segments, NROW separate
# PathSegments) - each passes multiple PathSegments to one optimize() call, which
# only eases the blank jump between them, it does not connect them.
KNOWN_MULTI_PIECE_PRESETS: set[str] = {
    "Three Circles", "Confetti Burst", "Grid 3x3", "Starfield",
    "Multi Wave", "Radial Waves", "Wave Field",
}

# Numeric knobs that are no-ops while their boolean gate is false - used both to
# warn in the search space and to explain "unchanged" params in the final report.
GATED_PARAMS = {
    "max_step_units":         "vel_clamp_enabled",
    "max_accel_units":        "accel_clamp_enabled",
    "ring_freq_hz":           "ringing_comp_enabled",
    "ring_damping_ratio":     "ringing_comp_enabled",
    "resample_spacing_units": "resample_enabled",
}


def parseFwVersion(v: str) -> tuple[int, int, int] | None:
    try:
        parts = [int(p) for p in v.strip().split(".")[:3]]
        return tuple(parts + [0] * (3 - len(parts)))
    except (ValueError, AttributeError):
        return None


# ── errors ───────────────────────────────────────────────────────────────────

class OptimizerError(Exception):
    """User-facing, actionable error. Caught once at the top of main() and
    printed as a plain message - no Python traceback - unless --debug is set."""


# ── output helpers ───────────────────────────────────────────────────────────

TERM_WIDTH = 80  # hard-wrap terminal output to this width for readability


def pr(*values, sep: str = " ", **kwargs):
    """print() replacement that hard-wraps to TERM_WIDTH columns. Messages here are
    built from f-string concatenation with variable-length interpolated values (paths,
    hostnames, counts, ...) so the source's own line breaks don't reflect the actual
    rendered length - wrapping has to happen here, at print time, against the real
    string. Preserves embedded newlines and each line's own leading indentation;
    wrapped continuation lines get that same indentation so a multi-line message
    still reads as one block."""
    text = sep.join(str(v) for v in values)
    for line in text.split("\n"):
        if len(line) <= TERM_WIDTH:
            print(line, **kwargs)
            continue
        indent = line[:len(line) - len(line.lstrip(" "))]
        print(textwrap.fill(line, width=TERM_WIDTH, subsequent_indent=indent,
                            break_long_words=False, break_on_hyphens=False), **kwargs)


# ASCII status prefixes (no emojis - avoids Windows console code-page encoding
# issues). Pick by what the line means to the user, not by call site:
#   [!] prWarn  recoverable problem or user-facing error (skipped a step, degraded
#               operation, request retried/failed) - also used for the top-level
#               error/interrupted handlers in main()
#   [+] prOk    action completed successfully (file written, connection confirmed)
#   [*] prTip   actionable recommendation - what to run/check next
#   [i] prInfo  notable status worth calling out, not just routine progress text
def prWarn(*values, **kwargs):
    pr("[!]", *values, **kwargs)


def prOk(*values, **kwargs):
    pr("[+]", *values, **kwargs)


def prTip(*values, **kwargs):
    pr("[*]", *values, **kwargs)


def prInfo(*values, **kwargs):
    pr("[i]", *values, **kwargs)


# ── bold text / tables ───────────────────────────────────────────────────────
# Windows Terminal and modern PowerShell/conhost render ANSI SGR codes fine, but
# classic conhost needs ENABLE_VIRTUAL_TERMINAL_PROCESSING turned on explicitly
# first - _enableWindowsAnsi() does that once at startup (best-effort, silently
# no-ops on anything it doesn't understand). NO_COLOR / non-tty output (piped to
# a file, redirected in CI) falls back to plain text automatically.

def _enableWindowsAnsi():
    if sys.platform != "win32":
        return
    try:
        import ctypes
        kernel32 = ctypes.windll.kernel32
        ENABLE_VIRTUAL_TERMINAL_PROCESSING = 0x0004
        for stdHandle in (-11, -12):  # STD_OUTPUT_HANDLE, STD_ERROR_HANDLE
            handle = kernel32.GetStdHandle(stdHandle)
            mode = ctypes.c_uint32()
            if kernel32.GetConsoleMode(handle, ctypes.byref(mode)):
                kernel32.SetConsoleMode(handle, mode.value | ENABLE_VIRTUAL_TERMINAL_PROCESSING)
    except Exception:
        pass  # best-effort - worst case, ANSI codes show up as raw text


def _supportsColor() -> bool:
    if os.environ.get("NO_COLOR"):
        return False
    return sys.stdout.isatty()


_COLOR = _supportsColor()
_SGR_BOLD, _SGR_DIM, _SGR_RESET = ("\033[1m", "\033[2m", "\033[0m") if _COLOR else ("", "", "")


def bold(s) -> str:
    return f"{_SGR_BOLD}{s}{_SGR_RESET}" if _COLOR else str(s)


def dim(s) -> str:
    return f"{_SGR_DIM}{s}{_SGR_RESET}" if _COLOR else str(s)


def prTable(rows: list[tuple[str, object]], headers: tuple[str, str] | None = None):
    """Renders a bordered two-column (label, value) table sized to its content and
    capped at TERM_WIDTH. ASCII-only borders (+/-/|) - Unicode box-drawing chars
    hit UnicodeEncodeError on a non-UTF-8 Windows console codepage (e.g. output
    piped through another tool), the exact failure mode the no-emoji rule exists
    to avoid. Bypasses pr()'s hard-wrap (its length check would be thrown off by
    bold()'s invisible ANSI bytes) - lines are already sized to fit."""
    if not rows:
        return
    data = ([headers] if headers else []) + [(k, str(v)) for k, v in rows]
    col0 = max(len(r[0]) for r in data)
    col1 = min(max(len(r[1]) for r in data), TERM_WIDTH - col0 - 7)

    def border():
        print("+" + "-" * (col0 + 2) + "+" + "-" * (col1 + 2) + "+")

    border()
    if headers:
        print(f"| {bold(headers[0].ljust(col0))} | {bold(headers[1].ljust(col1))} |")
        border()
    for k, v in rows:
        v = str(v)[:col1]
        print(f"| {bold(str(k).ljust(col0))} | {v.ljust(col1)} |")
    border()


# ── interactive prompts ──────────────────────────────────────────────────────

def askYesNo(prompt: str, default: bool) -> bool:
    """y/n prompt that reprompts on anything unrecognized (e.g. 'y93', 'ya') instead
    of silently treating it as no - only an empty line (Enter) falls back to
    `default`. Used for every [y/N]/[Y/n] question in this script. A prompt longer
    than TERM_WIDTH has its question text wrapped via pr() first, leaving only the
    short trailing '[y/N]: ' marker as the actual input() prompt."""
    valid = {"y": True, "yes": True, "n": False, "no": False}
    if len(prompt) > TERM_WIDTH:
        head, sep, tail = prompt.rpartition(" [")
        if sep:
            pr(head)
            prompt = sep.lstrip() + tail
    while True:
        raw = input(prompt).strip().lower()
        if raw == "":
            return default
        if raw in valid:
            return valid[raw]
        pr(f"  please answer y or n (got '{raw}')")


# ── configuration ────────────────────────────────────────────────────────────

CONFIG_FILE = Path(__file__).parent / "camConfig.json"
HOMOGRAPHY_FILE = Path(__file__).parent / "homography.npz"
SEARCH_SPACE_FILE = Path(__file__).parent / "searchSpace.json"
ORIENTATION_FILE = Path(__file__).parent / "orientation.json"
RESULTS_DIR = Path(__file__).parent / "results"

DEFAULT_CONFIG = {
    "esp32BaseUrl": "http://galvos.local",
    "cameraIndex": 0,
    "frameWidth": 1280,
    "frameHeight": 800,        # OV9281's native active-array size - probed against a real
                                # unit (VID_1BCF&PID_28C4): 1280x800 applies cleanly under
                                # dshow and is the largest mode that isn't an ISP crop/scale
                                # of the full sensor, so it gives calibrate's homography and
                                # measure/diagnose's pixel-space metrics the most resolution
                                # to work with. 1280x720 also works if you need the extra
                                # margin for a different lens/mount.
    "cameraFps": None,          # explicitly request this capture fps from the driver
                                # (null = let the driver auto-select; see the printed
                                # "driver-reported ... fps" line and the live overlay's
                                # measured fps to check what you're actually getting)
    "cameraFourcc": "MJPG",    # request this pixel format at open time (null = leave
                                # whatever the driver defaults to). This matters a lot more
                                # than it sounds: confirmed on a real OV9281
                                # (VID_1BCF&PID_28C4) that leaving it unset at 1280x720/800
                                # under dshow lands on an uncompressed mode whose declared
                                # UVC frame interval caps out around 5fps - and the actual
                                # *measured* fps (live overlay) can sag well below even that
                                # under load, not just the nominal/driver-reported one. MJPG
                                # is a real capture mode this sensor's ISP offers at the same
                                # resolution with a much higher declared rate, and it round-
                                # tripped cleanly in testing (requested == applied, frame
                                # delivered). Only takes effect on cameras/backends that
                                # support switching it - set to null and see if "fourcc" in
                                # the printed camera table still shows something else if this
                                # doesn't fix a low-fps camera for you.
    "cameraBackend": "dshow",  # dshow (default) / msmf / any - try msmf if manual
                                # exposure won't stick under dshow (common on Windows
                                # with some UVC drivers) and fps stays low regardless
                                # of scene brightness. Probed against a real OV9281
                                # (VID_1BCF&PID_28C4): dshow is the one to use there -
                                # msmf opened fixed at 1280x720@120fps and ignored every
                                # resolution/FOURCC change request outright (repeated
                                # "Failed to select stream 0" warnings), while dshow
                                # negotiated every requested mode and manual exposure
                                # stuck immediately (see AUTO_EXPOSURE_MANUAL_CANDIDATES).
    "displaySmoothFrames": 3,  # rolling max-projection over this many raw frames,
                                # display-only - fixes preview flicker at high fps/
                                # short exposure without touching measurement accuracy
                                # (grabAccumulated's own accumFrames is separate). 1 = off
    "exposure": -11,            # DirectShow log2 scale, ~1/2048 s
    "gain": 0,                  # some UVC drivers (confirmed on a real OV9281,
                                # VID_1BCF&PID_28C4) don't expose IAMVideoProcAmp Gain at
                                # all - Camera probes this itself at startup (see
                                # self.gainSupported) and 'autotune-camera' drops gain from
                                # its search space automatically when it doesn't stick, so
                                # this is a safe no-op default either way
    "brightness": 0,            # IAMVideoProcAmp neutral point (this camera's range is
                                # -64..64) - pinned rather than left at driver/OS default so
                                # a stray change from another app (e.g. Windows Camera) can't
                                # silently shift every future measurement's baseline
    "contrast": 0,               # neutral point (this camera's range is 0..95) - same
                                # reasoning as brightness
    "gamma": 100,                # 100 = linear response on this camera (range 100..300,
                                # higher = more curve) - computeMetrics' background-diff/
                                # threshold pipeline assumes pixel value is proportional to
                                # real light intensity, which only holds at gamma=linear
    "sharpness": 1,              # minimum (this camera's range is 1..10, no true "off") -
                                # UVC edge-enhancement rings/haloes around a small bright
                                # point source (the laser dot), which biases threshold-based
                                # blob detection and homography corner-dot centroids
    "backlightCompensation": 0, # OFF (this camera's range is 0..1) - this use case (a small
                                # bright dot on a large dark background) is the textbook
                                # backlight-compensation trigger; left on, the driver
                                # auto-brightens the whole frame to compensate, inflating
                                # background brightness/noise right when the diff-then-
                                # threshold pipeline needs the background as dark as possible
    "accumFrames": 24,          # frames max()-accumulated per measurement (~1 s @ 24 fps effective)
    "settleSeconds": 0.6,       # wait after param change before capture
    "patternSwitchSettleSeconds": 1.0,  # extra grace period around a calib-cam PATTERN
                                # switch specifically (measureOnce: right after
                                # startPattern, and again right after stop()) - separate
                                # from settleSeconds because a full pattern switch means
                                # the firmware has to actually stop streaming the OLD
                                # shape's points and start the NEW one, which takes real
                                # time; settleSeconds alone was sometimes too short,
                                # letting the tail end of the previous calib pattern (or
                                # the start of the next one) bleed into the same
                                # accumulated capture as a visible extra shape. Only
                                # applies where a different pattern can follow another
                                # one back-to-back (optimize/diagnose/autotune-camera/
                                # measure) - sweepGain/autotune-colors keep one pattern
                                # running throughout and aren't affected.
    "binaryThreshold": 40,      # 0..255, beam trace threshold after background subtraction
    "liveAnalysisMinComponentPx": 60,  # analyze-live only: connected-component pixel
                                # area (on the 800x800 DAC canvas, after 1px dilation)
                                # below which a blob is treated as sensor noise/dust
                                # rather than a real disconnected piece of the beam
                                # trace, and dropped before counting pieces / testing
                                # closure. Too low -> noise specks inflate the piece
                                # count and can trigger false "possible gap" flags;
                                # too high -> a real small disconnected fragment gets
                                # silently merged away. 60 was picked by observing
                                # stray noise blobs top out around 50-57px on this
                                # rig's camera/exposure - retune if your rig is noisier
                                # or analyze-live's screenshots show it eating real gaps
    "dacRange": 30000,          # reference dot coordinate (+-) used for homography
    "camPatternChannel": 3,     # laser color for calib-cam patterns: 0=white 1=R 2=G
                                # 3=B (default). Sent as "channel" to POST /api/calib-
                                # cam/start. Blue avoids a combined-white dot smearing/
                                # offsetting on the mono camera if R/G/B aren't
                                # perfectly co-boresighted (see optimizeGalvo diagnose
                                # geometry issues); switch to 1/2 to check a specific
                                # channel's own alignment instead.
    "warpCalibFrames": 10,      # frames MEDIAN-stacked per calibrate-warp control point
                                # (deliberately separate from accumFrames: that's max()-
                                # accumulated for a fast-scanned pattern that only lights
                                # each pixel briefly per frame; a calibrate-warp point is a
                                # STATIC dwell dot, lit continuously, so median rejects
                                # transient sensor noise/reflections better than max would)
    "warpCalibMinBlobAreaPx": 20,     # calibrate-warp: reject a detected point if its
    "warpCalibMaxBlobAreaPx": 4000,   # blob area (px^2, after background subtraction +
                                # threshold) falls outside this range - too small is
                                # noise/dust, too large is a lens flare/reflection or the
                                # laser itself out of focus
    "warpCalibMinPeakVal": 60,        # calibrate-warp: reject a detected point if the
                                # background-subtracted capture's peak pixel value is
                                # below this (0..255) - the dot wasn't actually bright
                                # enough to trust, even if a blob of plausible size and
                                # position was still found by the threshold pass
    "warpCalibToleranceCameraPx": 3.0,  # calibrate-warp: gridSize>2's iterative per-point
                                # solve (measure -> correct -> measure, max 3 rounds)
                                # stops early once a point's camera-pixel residual is
                                # under this
    "dacRangeTuneStepUnits": 600,  # tune-dac-range: bounded per-iteration gain/offset
                                # adjustment, DAC-code units - see autoTuneDacRange().
                                # Small enough that a single overshoot step can't jump
                                # straight from clipped to badly underscanning.
    "resonanceAxis": "x",       # measure-resonance: which galvo axis to drive
    "resonanceAmpFraction": 0.15,  # measure-resonance: test-drive peak amplitude as a
                                # fraction of the safe DAC range (min(0x8000-dac_limit_min,
                                # dac_limit_max-0x8000)). Deliberately conservative -
                                # near resonance the SAME commanded amplitude produces the
                                # LARGEST mechanical excursion of this entire toolkit's
                                # test signals (see docs/feature-prompts/DECISIONS.md,
                                # Prompt 13) - this is not a "how big can the pattern be"
                                # knob like dacRange, raise it only deliberately.
    "resonanceMinFreqHz": 50.0,
    "resonanceMaxFreqHz": 2000.0,
    "resonanceCoarsePoints": 30,   # log-spaced points across the full min/max range
    "resonanceFinePoints": 15,     # linear-spaced points around the coarse peak, for
                                # an accurate -3dB bandwidth/Q
    "resonanceFineSpanFraction": 0.4,  # fine pass spans +-this fraction of the coarse
                                # peak frequency (e.g. 0.4 @ 300Hz peak -> 180-420Hz)
    "resonanceFineSpanRetries": 2,  # if the -3dB crossing isn't found on both sides
                                # (peak too sharp/narrow for the span, or a noisy edge),
                                # double resonanceFineSpanFraction and re-sweep the fine
                                # pass this many times before giving up on Q/
                                # ring_damping_ratio for the run
    "resonanceMinCycles": 3,    # per frequency step, the capture window (settle +
                                # accumulated frames) must span at least this many
                                # drive periods for a reliable streak-extent reading -
                                # frame count is scaled up automatically at low
                                # frequencies where the default accumFrames wouldn't
                                # otherwise span enough cycles (camera frame rate is
                                # far too slow to resolve the waveform itself at these
                                # frequencies - this only needs the EXPOSURE-INTEGRATED
                                # streak envelope to be complete, see DECISIONS.md)
    "resonanceSettleSeconds": 0.15,  # wait after commanding a new frequency before
                                # capturing, so the galvo's own transient response to
                                # the frequency CHANGE itself has died down first
    "resonanceMinExtentPx": 3.0,   # streak extent readings below this are treated as
                                # noise floor / no visible response, not a real
                                # (near-zero) amplitude data point
    "resonanceChannel": 3,      # laser color for the resonance test: 0=white 1=R 2=G
                                # 3=B (default, same reasoning as camPatternChannel)
    "showCameraView": True,     # live preview window during calibrate/measure/optimize
    "resultViewHoldSeconds": 5.0,  # minimum seconds the camera view window (if shown)
                                # stays open and live after calibrate/measure/optimize/
                                # diagnose/autotune-camera/analyze-live finishes, instead
                                # of vanishing the instant the last capture completes -
                                # long enough to actually look at what was just measured.
                                # Press 'q' to close early. Hard floor MIN_RESULT_VIEW_
                                # HOLD_SECONDS below - raise this in the config to hold
                                # it open longer, it can't go lower.
    "offPathGuardPx": 18,       # DAC-canvas pixels. Two uses, both keyed off "how far from
                                # the ideal polyline can a pixel be and still plausibly be
                                # the beam itself": (a) a lit pixel further than this from
                                # the ideal path is counted as off-path noise/stray light
                                # (Metrics.offPathLitPx), (b) the "should be dark" corridor
                                # mask for blankCorridorLeakage is everything inside the
                                # ideal shape's bounding box that is further than this from
                                # any ideal segment. 18px @ dacPerPixel=75 is ~1350 DAC
                                # units - comfortably wider than this rig's measured beam
                                # half-width (~4px) plus warp residual, narrow enough that
                                # the segments pattern's 133px lane spacing still leaves a
                                # real corridor between lines.
    "pathCoverageRadiusPx": 6,  # DAC-canvas pixels. Metrics.pathCoveragePct = the fraction
                                # of the rasterised ideal path that has at least one lit
                                # pixel within this radius. This is the "is the camera
                                # actually seeing the shape" signal - a blind capture
                                # (threshold too high / exposure too low / out of focus)
                                # scores near zero here while still producing plausible-
                                # looking numbers for every other metric.
    "minPathCoveragePct": 10.0, # below this, the measurement is flagged invalid rather than
                                # scored. Measured on this rig: a correctly-thresholded
                                # capture covers 60-95%, the worst legitimate one seen was
                                # 13.1%; every blind capture was under 6%.
    "minBlobAreaPx": DEFAULT_MIN_BLOB_AREA_PX,  # DAC-canvas px. Connected components
                                # smaller than this are noise, not dots - only used by the
                                # isolated-dot metrics ('particles'). See its constant.
    "maxInvalidTrialFraction": 0.25,  # 'optimize' aborts if more than this fraction of a
                                # profile's trials come back invalid - at that point the
                                # study is measuring the camera, not the galvo.
    "costWeights": {
        "pathDeviationRms": 1.0,
        "blankLeakage": 2.0,
        "blankCorridorLeakage": 2.0,  # see Metrics.blankCorridorLeakage - the ideal-gap-
                                # corridor version of blankLeakage, which only samples the
                                # ideal jump path and therefore misses a beam that streaks
                                # somewhere else entirely
        "cornerHotspot": 0.5,
        "brightnessNonUniformity": 0.7,
        "saturationFrac": 3.0,  # fraction (0..1) of the traced beam that's raw-sensor-
                                # saturated - usually a camera exposure/gain problem
                                # (blooming), not a scan/dwell one; see Metrics.saturationFrac
        # Isolated-dot terms - contribute only on 'particles' (every other pattern
        # reports blobExpected 0 and the three terms evaluate to exactly 0).
        "blobElongation": 1.0,  # applied to (meanRatio - 1), so a round dot costs 0
        "blobCountError": 0.5,  # per merged/split/missing dot
        "blobCentroidErrorUnits": 1.0   # divided by 100 like pathDeviationRms
    },
    "diagnoseThresholds": {
        # Above these: 'diagnose' flags the profile. Geometry ones (scale/offset) are
        # NOT fixable by autotune - they mean the projected shape's size/position is
        # off, which points at galvo gain/DAC calibration drift or a moved camera/
        # surface, not a scan-parameter problem. The rest mirror costWeights' metrics
        # and DO trigger an autotune offer.
        "geometryScalePct": 5.0,        # abs(scaleErrorX/YPct) above this -> geometry issue
        "geometryOffsetUnits": 600.0,   # abs(offsetX/YUnits), DAC units, above this -> geometry issue
        # pathDeviationRms is NOT a flat number: on any real rig the measured RMS distance
        # from the ideal path is dominated by the BEAM's own width, not by path error. This
        # rig measures a beam half-width of ~4 canvas px = ~300 DAC units, which alone puts
        # a perfectly-tuned square at ~285 RMS - a flat 150 threshold was unreachable and
        # reported a permanent false positive on every single profile. The bar is instead
        # derived per-measurement from Metrics.beamWidthUnits (2 x median distance of lit
        # pixels from the ideal path, in DAC units):
        #     bar = max(pathDeviationRmsMinUnits, pathDeviationBeamWidthFactor * beamWidth)
        "pathDeviationBeamWidthFactor": 1.5,  # measured on this rig at the working
                                         # threshold: square 283/675, star 160/315,
                                         # circle 291/720, spiral 316/810, segments 144/225
                                         # (actual/bar) - tightest margin 0.64 of the bar
        "pathDeviationRmsMinUnits": 150.0,   # absolute floor, so an implausibly narrow
                                         # measured beam can't drive the bar to zero
        "blankLeakage": 15.0,
        "blankCorridorLeakage": 0.30,   # mean brightness (0..255) of the "should be dark"
                                         # corridor. Clean captures on this rig: square
                                         # 0.032, star 0.035, circle 0.060, spiral 0.195,
                                         # segments 0.026. Set generously above the worst
                                         # clean case - this metric's real value is
                                         # RELATIVE comparison across a parameter sweep,
                                         # not an absolute pass/fail (it scales with beam
                                         # brightness and with how much dark area the
                                         # pattern happens to enclose).
        "cornerHotspot": 0.35,
        "brightnessNonUniformity": 0.5,
        "saturationFrac": 0.10,         # >10% of the traced beam clipped -> likely camera
                                         # blooming; 'diagnose' points at 'autotune-camera'
                                         # for this one specifically, not 'optimize'
        # Isolated-dot thresholds - only evaluated when blobExpected > 0 ('particles').
        # No clean-capture baseline exists for these yet (the pattern is new and has not
        # been run against hardware), so they are set from geometry rather than from
        # measurement: an in-focus dot images roughly round, and a dot that has been
        # smeared along a jump is many times longer than it is wide.
        "blobElongation": 2.0,          # mean major/minor axis ratio over all dots
        "blobCountError": 1.0,          # more than one merged/missing dot
        "blobCentroidErrorUnits": 900.0  # ~12 canvas px at dacPerPixel 75 - well past
                                         # beam width, so a dot landing short shows up
    },
    "cameraAutotuneRanges": {
        # Search bounds for 'autotune-camera'. Only knobs that (a) affect capture
        # quality and (b) can be changed trial-to-trial without reopening the camera
        # are tunable this way - frameWidth/frameHeight/cameraFps/cameraBackend are
        # negotiated once at VideoCapture-open time, so they're left to the wizard.
        "exposureMin": -13,          # DirectShow log2 scale - more negative = shorter.
                                     # -13 is this camera's actual hardware floor (probed:
                                     # IAMCameraControl Exposure range is [-13, -1]) - a
                                     # lower bound the driver can't reach just wastes Optuna
                                     # trials on a value that silently clamps to -13 anyway
        "exposureMax": -2,
        "gainMin": 0,                # only used if Camera detects gain actually sticks on
                                     # your unit (self.gainSupported) - see the "gain" note
                                     # in DEFAULT_CONFIG above
        "gainMax": 80,
        "binaryThresholdMin": 10,
        "binaryThresholdMax": 200,
        "accumFramesMin": 6,
        "accumFramesMax": 40
    },
    "cameraAutotuneWeights": {
        # Extra penalty terms 'autotune-camera' adds on top of the normal costWeights-
        # scored path/leakage/uniformity metrics - those alone don't reliably punish
        # a washed-out (overexposed/high-gain) capture, since an all-white frame can
        # look perfectly "uniform".
        #
        # The signal-vs-noise terms below exist because without them the search actively
        # REWARDS blindness: raising binaryThreshold keeps only the brightest core pixels,
        # which SHRINKS pathDeviationRms and improves brightnessNonUniformity. That is how
        # this rig's config ended up at binaryThreshold=133 - a value that left 5 lit pixels
        # in the entire warped canvas while scoring better than the usable 50. The criterion
        # is now "maximise the amount of the ideal path actually seen, subject to no lit
        # pixels outside the beam trace".
        "saturation": 2.0,             # weight on fraction of pixels >=250 in the raw capture
        "backgroundBrightness": 1.0,   # weight on mean(background)/255 (laser-off frame)
        "pathCoverage": 3.0,           # weight on (1 - pathCoveragePct/100) - rewards seeing
                                       # more of the ideal path
        "offPathNoise": 4.0,           # weight on offPathLitPx/traceLitPx - punishes lit
                                       # pixels further than offPathGuardPx from the ideal
                                       # path (stray light, reflections, sensor noise). Rated
                                       # above pathCoverage so the search cannot buy coverage
                                       # by dropping the threshold into the noise floor.
        "blindCapture": 5.0            # flat penalty per pattern whose measurement came back
                                       # invalid (see Metrics.valid) - a hard wall in front
                                       # of the whole blind region of the search space, so a
                                       # trial that sees nothing can never win on the smooth
                                       # terms alone
    },
    "requestTimeoutSeconds": 5,
    "requestRetries": 2,             # extra attempts on ESP32 timeout/connection error
    "requestRetryDelaySeconds": 1.0  # wait between retries (transient WiFi hiccups)
}


def validateConfig(cfg: dict):
    """Catches config problems here, once, with one clear message - instead of a
    division-by-zero / negative-index / cv2 error surfacing deep in a capture loop."""
    problems = []
    if not isinstance(cfg.get("dacRange"), (int, float)) or cfg["dacRange"] <= 0:
        problems.append("dacRange must be a positive number")
    if not isinstance(cfg.get("camPatternChannel"), int) or not (0 <= cfg["camPatternChannel"] <= 3):
        problems.append("camPatternChannel must be an integer 0-3 (0=white 1=R 2=G 3=B)")
    if not isinstance(cfg.get("requestTimeoutSeconds"), (int, float)) or cfg["requestTimeoutSeconds"] <= 0:
        problems.append("requestTimeoutSeconds must be a positive number")
    if not isinstance(cfg.get("requestRetries"), int) or cfg["requestRetries"] < 0:
        problems.append("requestRetries must be a non-negative integer")
    if not isinstance(cfg.get("requestRetryDelaySeconds"), (int, float)) or cfg["requestRetryDelaySeconds"] < 0:
        problems.append("requestRetryDelaySeconds must be a non-negative number")
    if not isinstance(cfg.get("frameWidth"), int) or cfg["frameWidth"] <= 0:
        problems.append("frameWidth must be a positive integer")
    if not isinstance(cfg.get("frameHeight"), int) or cfg["frameHeight"] <= 0:
        problems.append("frameHeight must be a positive integer")
    if cfg.get("cameraFps") is not None and (
            not isinstance(cfg["cameraFps"], (int, float)) or cfg["cameraFps"] <= 0):
        problems.append("cameraFps must be null or a positive number")
    if cfg.get("cameraFourcc") is not None and (
            not isinstance(cfg["cameraFourcc"], str) or len(cfg["cameraFourcc"]) != 4):
        problems.append("cameraFourcc must be null or a 4-character FOURCC string (e.g. 'MJPG')")
    if cfg.get("cameraBackend") not in ("dshow", "msmf", "any"):
        problems.append("cameraBackend must be 'dshow', 'msmf', or 'any'")
    if not isinstance(cfg.get("displaySmoothFrames"), int) or cfg["displaySmoothFrames"] < 1:
        problems.append("displaySmoothFrames must be an integer >= 1")
    if (not isinstance(cfg.get("resultViewHoldSeconds"), (int, float))
            or cfg["resultViewHoldSeconds"] < MIN_RESULT_VIEW_HOLD_SECONDS):
        problems.append(f"resultViewHoldSeconds must be a number >= "
                        f"{MIN_RESULT_VIEW_HOLD_SECONDS}")
    if not isinstance(cfg.get("cameraIndex"), int) or cfg["cameraIndex"] < 0:
        problems.append("cameraIndex must be a non-negative integer")
    if not isinstance(cfg.get("accumFrames"), int) or cfg["accumFrames"] < 1:
        problems.append("accumFrames must be an integer >= 1")
    for key in ("brightness", "contrast", "gamma", "sharpness", "backlightCompensation"):
        if not isinstance(cfg.get(key), (int, float)):
            problems.append(f"{key} must be a number (hardware range varies by camera - "
                            f"an out-of-range value is simply clamped/ignored by the driver)")
    if not isinstance(cfg.get("binaryThreshold"), (int, float)) or not (0 <= cfg["binaryThreshold"] <= 255):
        problems.append("binaryThreshold must be a number between 0 and 255")
    if not isinstance(cfg.get("warpCalibFrames"), int) or cfg["warpCalibFrames"] < 1:
        problems.append("warpCalibFrames must be an integer >= 1")
    if (not isinstance(cfg.get("warpCalibMinBlobAreaPx"), (int, float))
            or not isinstance(cfg.get("warpCalibMaxBlobAreaPx"), (int, float))
            or cfg["warpCalibMinBlobAreaPx"] < 0
            or cfg["warpCalibMinBlobAreaPx"] >= cfg["warpCalibMaxBlobAreaPx"]):
        problems.append("warpCalibMinBlobAreaPx must be a non-negative number less than "
                        "warpCalibMaxBlobAreaPx")
    if not isinstance(cfg.get("warpCalibMinPeakVal"), (int, float)) or not (0 <= cfg["warpCalibMinPeakVal"] <= 255):
        problems.append("warpCalibMinPeakVal must be a number between 0 and 255")
    if not isinstance(cfg.get("warpCalibToleranceCameraPx"), (int, float)) or cfg["warpCalibToleranceCameraPx"] <= 0:
        problems.append("warpCalibToleranceCameraPx must be a positive number")
    if not isinstance(cfg.get("dacRangeTuneStepUnits"), (int, float)) or cfg["dacRangeTuneStepUnits"] <= 0:
        problems.append("dacRangeTuneStepUnits must be a positive number")
    if not isinstance(cfg.get("liveAnalysisMinComponentPx"), (int, float)) or cfg["liveAnalysisMinComponentPx"] < 0:
        problems.append("liveAnalysisMinComponentPx must be a non-negative number")
    if cfg.get("resonanceAxis") not in ("x", "y"):
        problems.append("resonanceAxis must be 'x' or 'y'")
    if not isinstance(cfg.get("resonanceAmpFraction"), (int, float)) or not (0 < cfg["resonanceAmpFraction"] <= 1):
        problems.append("resonanceAmpFraction must be a number between 0 (exclusive) and 1")
    if (not isinstance(cfg.get("resonanceMinFreqHz"), (int, float))
            or not isinstance(cfg.get("resonanceMaxFreqHz"), (int, float))
            or cfg["resonanceMinFreqHz"] <= 0
            or cfg["resonanceMinFreqHz"] >= cfg["resonanceMaxFreqHz"]):
        problems.append("resonanceMinFreqHz must be a positive number less than resonanceMaxFreqHz")
    if not isinstance(cfg.get("resonanceCoarsePoints"), int) or cfg["resonanceCoarsePoints"] < 3:
        problems.append("resonanceCoarsePoints must be an integer >= 3")
    if not isinstance(cfg.get("resonanceFinePoints"), int) or cfg["resonanceFinePoints"] < 3:
        problems.append("resonanceFinePoints must be an integer >= 3")
    if not isinstance(cfg.get("resonanceFineSpanFraction"), (int, float)) or not (0 < cfg["resonanceFineSpanFraction"] < 1):
        problems.append("resonanceFineSpanFraction must be a number between 0 and 1 (exclusive)")
    if not isinstance(cfg.get("resonanceFineSpanRetries"), int) or cfg["resonanceFineSpanRetries"] < 0:
        problems.append("resonanceFineSpanRetries must be a non-negative integer")
    if not isinstance(cfg.get("resonanceMinCycles"), int) or cfg["resonanceMinCycles"] < 1:
        problems.append("resonanceMinCycles must be an integer >= 1")
    if not isinstance(cfg.get("resonanceSettleSeconds"), (int, float)) or cfg["resonanceSettleSeconds"] < 0:
        problems.append("resonanceSettleSeconds must be a non-negative number")
    if not isinstance(cfg.get("resonanceMinExtentPx"), (int, float)) or cfg["resonanceMinExtentPx"] < 0:
        problems.append("resonanceMinExtentPx must be a non-negative number")
    if not isinstance(cfg.get("resonanceChannel"), int) or not (0 <= cfg["resonanceChannel"] <= 3):
        problems.append("resonanceChannel must be an integer 0-3 (0=white 1=R 2=G 3=B)")
    if not isinstance(cfg.get("offPathGuardPx"), (int, float)) or not (1 <= cfg["offPathGuardPx"] < CANVAS / 2):
        problems.append(f"offPathGuardPx must be a number between 1 and {CANVAS // 2}")
    if not isinstance(cfg.get("pathCoverageRadiusPx"), (int, float)) or cfg["pathCoverageRadiusPx"] < 1:
        problems.append("pathCoverageRadiusPx must be a number >= 1")
    if not isinstance(cfg.get("minPathCoveragePct"), (int, float)) or not (0 <= cfg["minPathCoveragePct"] <= 100):
        problems.append("minPathCoveragePct must be a number between 0 and 100")
    if not isinstance(cfg.get("maxInvalidTrialFraction"), (int, float)) or not (0 <= cfg["maxInvalidTrialFraction"] <= 1):
        problems.append("maxInvalidTrialFraction must be a number between 0 and 1")
    costWeights = cfg.get("costWeights")
    if not isinstance(costWeights, dict) or not all(
            isinstance(v, (int, float)) for v in costWeights.values()):
        problems.append("costWeights must be an object of numeric weights")
    diagnoseThresholds = cfg.get("diagnoseThresholds")
    if not isinstance(diagnoseThresholds, dict) or not all(
            isinstance(v, (int, float)) and v >= 0 for v in diagnoseThresholds.values()):
        problems.append("diagnoseThresholds must be an object of non-negative numeric thresholds")
    ranges = cfg.get("cameraAutotuneRanges")
    if not isinstance(ranges, dict) or not all(
            isinstance(v, (int, float)) for v in ranges.values()):
        problems.append("cameraAutotuneRanges must be an object of numeric bounds")
    else:
        for lo, hi in (("exposureMin", "exposureMax"), ("gainMin", "gainMax"),
                      ("binaryThresholdMin", "binaryThresholdMax"),
                      ("accumFramesMin", "accumFramesMax")):
            if lo in ranges and hi in ranges and ranges[lo] >= ranges[hi]:
                problems.append(f"cameraAutotuneRanges: {lo} ({ranges[lo]}) must be < "
                                f"{hi} ({ranges[hi]})")
    autotuneWeights = cfg.get("cameraAutotuneWeights")
    if not isinstance(autotuneWeights, dict) or not all(
            isinstance(v, (int, float)) for v in autotuneWeights.values()):
        problems.append("cameraAutotuneWeights must be an object of numeric weights")
    if problems:
        raise OptimizerError(
            f"invalid {CONFIG_FILE.name}: " + "; ".join(problems) +
            f". Fix the file by hand or run 'optimizeGalvo.py wizard' to reconfigure."
        )


def loadConfig() -> dict:
    if CONFIG_FILE.exists():
        try:
            onDisk = json.loads(CONFIG_FILE.read_text())
        except json.JSONDecodeError as e:
            raise OptimizerError(
                f"{CONFIG_FILE.name} is not valid JSON ({e}). Fix it by hand, or delete it "
                f"and run 'optimizeGalvo.py wizard' to recreate it."
            ) from e
        except OSError as e:
            raise OptimizerError(f"cannot read {CONFIG_FILE}: {e}") from e

        if not isinstance(onDisk, dict):
            raise OptimizerError(
                f"{CONFIG_FILE.name} must contain a JSON object, found a "
                f"{type(onDisk).__name__} instead"
            )
        nestedDictKeys = [k for k, v in DEFAULT_CONFIG.items() if isinstance(v, dict)]
        for key in nestedDictKeys:
            if key in onDisk and not isinstance(onDisk[key], dict):
                raise OptimizerError(f"{CONFIG_FILE.name}: '{key}' must be a JSON object")

        unknownKeys = sorted(set(onDisk) - set(DEFAULT_CONFIG))
        if unknownKeys:
            prWarn(f"{CONFIG_FILE.name} has unrecognized key(s), check for typos: "
                  f"{unknownKeys}")
        cfg = {**DEFAULT_CONFIG, **onDisk}
        for key in nestedDictKeys:
            if key in onDisk:
                unknownSub = sorted(set(onDisk[key]) - set(DEFAULT_CONFIG[key]))
                if unknownSub:
                    prWarn(f"{CONFIG_FILE.name} '{key}' has unrecognized key(s): "
                          f"{unknownSub}")
                cfg[key] = {**DEFAULT_CONFIG[key], **onDisk[key]}

        # Migrate: an older camConfig.json predating a newly-added setting (like
        # displaySmoothFrames, or a whole new nested block like diagnoseThresholds)
        # would otherwise silently use the in-memory default forever without it ever
        # showing up in the file to edit. Write the missing key(s) - with their
        # default values - back into the file on disk.
        missingKeys = sorted(set(DEFAULT_CONFIG) - set(onDisk))
        missingSub = []
        for key in nestedDictKeys:
            if isinstance(onDisk.get(key), dict):
                missingSub += [f"{key}.{k}" for k in sorted(set(DEFAULT_CONFIG[key]) - set(onDisk[key]))]
        if missingKeys or missingSub:
            try:
                CONFIG_FILE.write_text(json.dumps(cfg, indent=2))
            except OSError as e:
                prWarn(f"could not add missing default key(s) to {CONFIG_FILE.name}: {e}")
            else:
                added = missingKeys + missingSub
                prOk(f"added missing default key(s) to {CONFIG_FILE.name}: {added}")

        validateConfig(cfg)
        return cfg

    if sys.stdin.isatty():
        prInfo(f"no {CONFIG_FILE.name} found - running first-time setup wizard\n")
        cfg = runWizard()
        validateConfig(cfg)
        return cfg

    cfg = dict(DEFAULT_CONFIG)
    try:
        CONFIG_FILE.write_text(json.dumps(cfg, indent=2))
    except OSError as e:
        raise OptimizerError(f"cannot write {CONFIG_FILE}: {e}") from e
    prOk(f"created {CONFIG_FILE.name} with defaults (non-interactive session, skipped "
          f"the setup wizard - run 'optimizeGalvo.py wizard' to configure it)")
    return cfg


# Files in results/ that must survive a cleanup - the Optuna SQLite study database(s)
# (plus the -wal/-shm/-journal sidecar files SQLite creates alongside an open db), since
# deleting them would silently wipe the resumable trial history 'optimize' and
# 'autotune-camera' rely on (see storageUrl in runOptimize/runAutotuneCamera).
RESULTS_PERSISTENT_GLOBS = ("*.db", "*.db-wal", "*.db-shm", "*.db-journal")


def cleanupResultsDir():
    """Offers to delete stale results/ files left over from previous runs (snapshots,
    per-trial .jsonl logs, best_*.json, ...) so they don't quietly pile up run after run.
    Never touches the Optuna study database(s) - those are load-bearing, not disposable.
    Silently does nothing if results/ doesn't exist yet, nothing stale is found, or the
    session isn't interactive (no one to answer the confirmation prompt, and deleting
    without asking would be the one truly irreversible thing this script could do)."""
    if not RESULTS_DIR.exists() or not sys.stdin.isatty():
        return
    persistent = set()
    for pattern in RESULTS_PERSISTENT_GLOBS:
        persistent.update(RESULTS_DIR.glob(pattern))
    stale = sorted((p for p in RESULTS_DIR.iterdir() if p.is_file() and p not in persistent),
                  key=lambda p: p.name)
    if not stale:
        return

    totalBytes = sum(p.stat().st_size for p in stale)
    pr(f"found {len(stale)} old file(s) in {RESULTS_DIR.name}/ from previous runs "
       f"({totalBytes / 1024:.0f} KB) - optuna_study.db (search history) is always kept:")
    for p in stale[:10]:
        pr(f"  {p.name}")
    if len(stale) > 10:
        pr(f"  ... and {len(stale) - 10} more")

    if not askYesNo(f"delete these {len(stale)} old file(s)? [y/N]: ", default=False):
        return
    deleted = 0
    for p in stale:
        try:
            p.unlink()
            deleted += 1
        except OSError as e:
            prWarn(f"could not delete {p.name}: {e}")
    prOk(f"deleted {deleted} old file(s) from {RESULTS_DIR.name}/")


# ── ESP32 REST client ────────────────────────────────────────────────────────

class EspClient:
    def __init__(self, baseUrl: str, timeoutSeconds: float, retries: int = 2,
                retryDelaySeconds: float = 1.0):
        self.baseUrl = baseUrl.rstrip("/")
        self.timeout = timeoutSeconds
        self.retries = retries
        self.retryDelaySeconds = retryDelaySeconds
        # Reused across every call - avoids a fresh TCP handshake per request, which adds
        # up over an optimize run's hundreds of start/params/stop round trips.
        self.session = requests.Session()

    def _request(self, method: str, path: str, payload: dict | None = None) -> dict:
        url = f"{self.baseUrl}{path}"
        attempt = 0
        while True:
            try:
                if method == "POST":
                    resp = self.session.post(url, json=payload or {}, timeout=self.timeout)
                else:
                    resp = self.session.get(url, timeout=self.timeout)
                break
            except (requests.exceptions.Timeout, requests.exceptions.ConnectionError) as e:
                # Transient network hiccups (WiFi drop, momentary ESP32 busy) shouldn't
                # abort a long unattended optimize run over a single blip - retry a few
                # times before giving up. A laser-safety-relevant call (like /stop) still
                # ends up raising if the controller stays unreachable, since we then
                # genuinely can't confirm the laser's state.
                attempt += 1
                if attempt > self.retries:
                    if isinstance(e, requests.exceptions.Timeout):
                        raise OptimizerError(
                            f"ESP32 request to {path} timed out after {self.timeout}s "
                            f"({attempt} attempt(s)) - controller unreachable or "
                            f"esp32BaseUrl wrong in {CONFIG_FILE.name}. Run "
                            f"'optimizeGalvo.py check' for a full diagnostic."
                        ) from e
                    raise OptimizerError(
                        f"cannot connect to ESP32 at {self.baseUrl} ({path}) after "
                        f"{attempt} attempt(s): {e}. Check esp32BaseUrl in "
                        f"{CONFIG_FILE.name}, WiFi, and that the controller is powered "
                        f"on. mDNS names (*.local) need Bonjour/iTunes on Windows - try "
                        f"the controller's IP address instead if unsure. Run "
                        f"'optimizeGalvo.py check' for a full diagnostic."
                    ) from e
                prWarn(f"{path} {'timed out' if isinstance(e, requests.exceptions.Timeout) else 'connection failed'} "
                      f"(attempt {attempt}/{self.retries + 1}) - retrying in "
                      f"{self.retryDelaySeconds:.0f}s ...")
                time.sleep(self.retryDelaySeconds)
            except requests.exceptions.RequestException as e:
                raise OptimizerError(f"ESP32 request to {path} failed: {e}") from e

        if not resp.ok:
            hint = ""
            if resp.status_code == 404:
                minStr = ".".join(map(str, MIN_FW_VERSION_CALIB_CAM))
                hint = (f" - the ESP32 firmware may predate the calib-cam API "
                        f"(needs >= v{minStr}); run 'optimizeGalvo.py check' to see "
                        f"its fw_version")
            elif resp.text:
                hint = f" - {resp.text.strip()[:200]}"
            raise OptimizerError(
                f"ESP32 rejected {method} {path} (HTTP {resp.status_code}){hint}"
            )
        try:
            return resp.json() if resp.content else {}
        except ValueError:
            # Several firmware endpoints answer a bare "OK"/"saved" text/plain on
            # success (/api/preset, /api/optimizer-live, /api/optimizer-save) -
            # a non-JSON 2xx body is fine, not an error.
            return {"_raw": resp.text}

    def _post(self, path: str, payload: dict | None = None) -> dict:
        return self._request("POST", path, payload)

    def _get(self, path: str) -> dict:
        return self._request("GET", path)

    @classmethod
    def fromConfig(cls, cfg: dict) -> "EspClient":
        return cls(cfg["esp32BaseUrl"], cfg["requestTimeoutSeconds"],
                   retries=cfg.get("requestRetries", 2),
                   retryDelaySeconds=cfg.get("requestRetryDelaySeconds", 1.0))

    def startPattern(self, pattern: str, channel: int | None = None) -> dict:
        """channel: 0=white 1=R 2=G 3=B - omit to keep the ESP32's own default (blue)."""
        payload = {"pattern": pattern}
        if channel is not None:
            payload["channel"] = channel
        return self._post("/api/calib-cam/start", payload)

    def setParams(self, params: dict) -> dict:
        """Returns effective values as applied by the server (server-authoritative)."""
        return self._post("/api/calib-cam/params", params)

    def stop(self) -> dict:
        return self._post("/api/calib-cam/stop")

    def getStatus(self) -> dict:
        """GET /api/status - general dashboard status, not calib-cam specific.
        Used as a lightweight reachability/identity check for the controller."""
        return self._get("/api/status")

    def getConfig(self) -> dict:
        """GET /api/config - includes opt_active_profile and opt_profile_members,
        the live list of optimizer profiles the connected firmware actually has."""
        return self._get("/api/config")

    def getState(self) -> dict:
        """GET /api/state - live pattern-engine state (preset_idx, ilda_active,
        playlist_active, calib_active, frame_lit/frame_blank, fps, ...). Used by
        'analyze-live' to describe what's actually on screen without touching it."""
        return self._get("/api/state")

    def getPresets(self) -> list[dict]:
        """GET /api/presets - [{idx, name, cat}, ...] for every built-in preset.
        Returns a bare JSON array (not an object), unlike every other endpoint here."""
        return self._get("/api/presets")   # type: ignore[return-value]

    def applyOptimizerLive(self, profileIndex: int, params: dict) -> None:
        """POST /api/optimizer-live - permanently applies params to a firmware profile
        (until reboot). Needed after tuning: the calib-cam session restores a snapshot
        of the profile on /stop, so values tuned inside the session don't stick."""
        # profile applied last so a param literally named "profile" (from searchSpace.json)
        # can't silently clobber which firmware profile this gets applied to.
        self._post("/api/optimizer-live", {**params, "profile": profileIndex})

    def saveOptimizer(self) -> None:
        """POST /api/optimizer-save - persists all optimizer profiles to NVS."""
        self._post("/api/optimizer-save")

    def setCalibLive(self, **fields) -> dict:
        """POST /api/calib-live - sets any of gain_r/g/b, thresh_r/g/b (plus the galvo
        geometry fields the WebUI's Calibration card also uses) live, with NO NVS
        write and NO session-snapshot/rollback - unlike optimizer-live params, this is
        an immediate, permanent (until changed again) mutation of gConfig. Only
        non-None kwargs are sent, so callers can set a single field without touching
        the others. Callers that change these values must restore them themselves if
        the change shouldn't stick (see runAutotuneColors)."""
        payload = {k: v for k, v in fields.items() if v is not None}
        return self._post("/api/calib-live", payload)

    def calibSave(self) -> None:
        """POST /api/calib-save - persists current gConfig (gain_r/g/b, thresh_r/g/b,
        galvo geometry, ...) to NVS. The calib-live values set above are lost on
        reboot until this is called."""
        self._post("/api/calib-save")

    def calibThreshTest(self, active: bool, channel: int) -> None:
        """POST /api/calib-thresh-test - drives a static, minimal-level (logical=1)
        single-point beam with gain/gamma/dimmer bypassed, so the configured
        thresh_r/g/b directly controls the output duty almost 1:1. channel must be
        1(R)/2(G)/3(B) individually for a single-channel test - channel=0 lights all
        three at once (see calib_thresh_ch decode in galvo_out.cpp)."""
        self._post("/api/calib-thresh-test", {"active": active, "channel": channel})

    def calibPattern(self, idx: int, channel: int = 0, bright: int = 200,
                     active: bool = True) -> None:
        """POST /api/calib-pattern - idx-based calibration/test pattern select
        (calib_patterns.cpp). Distinct from the name-based /api/calib-cam/* family
        used by startPattern() above - this is the plain idx route every calib
        pattern (including the color-ramp linearity patterns, idx 18-20) is
        reachable through."""
        self._post("/api/calib-pattern",
                   {"idx": idx, "channel": channel, "bright": bright, "active": active})

    def stopCalibPattern(self) -> None:
        """POST /api/calib-pattern/stop"""
        self._post("/api/calib-pattern/stop")

    def debugHw(self, x: int, y: int, r: int, g: int, b: int) -> dict:
        """POST /api/debug/hw - direct single-point galvo+laser control. x/y are RAW
        DAC-space coordinates (-32767..32767, centered at 0) written straight to the
        DAC8562 as x+32768 - this BYPASSES pattern generation, the warp stage, AND
        applyCalibration()'s gain/offset/outputScale/mirror entirely, unlike every
        other pattern path. 'calibrate-warp' uses this precisely because it needs to
        command an EXACT DAC position and see where it physically lands, independent
        of whatever warp grid is currently active. Requires laser_armed (or
        gDebugNoHW on the firmware side)."""
        return self._post("/api/debug/hw", {"x": x, "y": y, "r": r, "g": g, "b": b})

    def debugHwOff(self) -> None:
        """POST /api/debug/hw {cmd:off} - blanks the beam and releases debug-output
        mode (galvoTask resumes normal ring-buffer consumption)."""
        self._post("/api/debug/hw", {"cmd": "off"})

    def resonanceTest(self, axis: int, freqHz: float, amp: int,
                      r: int, g: int, b: int) -> dict:
        """POST /api/debug/resonance - free-runs a sine wave on one axis (axis:
        0=X, 1=Y), generated firmware-side (galvoTask, per-tick) since HTTP round
        trips are far too slow to synthesize a waveform anywhere near the 50-2000Hz
        sweep range from here. amp is the DAC-space peak (-32767..32767); the
        firmware clamps it server-side against dac_limit_min/max before arming and
        reports {"clamped": true} if it had to. Requires laser_armed (or
        gDebugNoHW), same guard as debugHw(). See docs/feature-prompts/
        DECISIONS.md, Prompt 13."""
        return self._post("/api/debug/resonance",
                          {"axis": axis, "freq_hz": freqHz, "amp": amp,
                           "r": r, "g": g, "b": b})

    def resonanceOff(self) -> None:
        """POST /api/debug/resonance {cmd:off} - blanks the beam and stops the
        sine drive (galvoTask resumes normal ring-buffer consumption)."""
        self._post("/api/debug/resonance", {"cmd": "off"})

    def resonanceStatus(self) -> dict:
        """GET /api/debug/resonance - {"active": bool, "armed": bool}."""
        return self._get("/api/debug/resonance")

    def warpGet(self) -> dict:
        """GET /api/warp/get - current warp grid: {enabled, gridSize, points}."""
        return self._get("/api/warp/get")

    def warpSet(self, gridSize: int, points: list, enabled: bool | None = None) -> dict:
        """POST /api/warp/set - full grid replace. points must be gridSize x gridSize
        of [x,y] pairs, normalized [-1..1] (firmware validates -1.5..1.5)."""
        payload = {"gridSize": gridSize, "points": points}
        if enabled is not None:
            payload["enabled"] = enabled
        return self._post("/api/warp/set", payload)

    def warpReset(self) -> dict:
        """POST /api/warp/reset - grid back to identity (enabled untouched)."""
        return self._post("/api/warp/reset")

    def warpTest(self, active: bool) -> dict:
        """POST /api/warp/test - toggles the WARP_GRID_TEST calibration pattern
        (border + gWarp.gridSize interior lines), which goes through the real
        optimizer pipeline including the live warp stage."""
        return self._post("/api/warp/test", {"active": active})


# ── live camera view ─────────────────────────────────────────────────────────

ZOOM_LEVELS = (1.0, 2.0, 3.0)   # selected with keys '1'/'2'/'3' while a view window is open


def applyZoom(frame: np.ndarray, zoomIdx: int) -> np.ndarray:
    """Digital zoom: crop a centered region and resize back to the original frame
    size, so the display window stays a constant size while showing more detail."""
    zoom = ZOOM_LEVELS[zoomIdx]
    if zoom == 1.0:
        return frame
    h, w = frame.shape[:2]
    cropW, cropH = int(w / zoom), int(h / zoom)
    x0, y0 = (w - cropW) // 2, (h - cropH) // 2
    cropped = frame[y0:y0 + cropH, x0:x0 + cropW]
    return cv2.resize(cropped, (w, h), interpolation=cv2.INTER_LINEAR)


class LiveView:
    """Non-blocking cv2 window that mirrors every frame grabbed through Camera
    during preview/calibrate/measure/optimize/diagnose: digital zoom ('1'/'2'/'3'),
    pause ('space'), save the current frame to disk ('s'), quit ('q'), a measured-fps
    readout, an optional progress bar (setProgress), and a bottom-row hotkey legend."""

    DEFAULT_HOTKEYS = "[1/2/3] zoom   [s] save   [space] pause   [q] quit"

    def __init__(self, windowName: str, width: int, height: int, zoomIdx: int = 0,
                hotkeys: str = DEFAULT_HOTKEYS):
        self.windowName = windowName
        self.zoomIdx = zoomIdx
        self.quitRequested = False
        self.paused = False
        self.hotkeys = hotkeys
        self.progress: tuple[int, int] | None = None
        self.lastKey = 255
        self._frameTimes: collections.deque = collections.deque(maxlen=30)
        self._snapCounter = 0
        # Set by Camera.grabAccumulated() after a full accumFrames-accumulation
        # completes (measure/optimize/diagnose/calibrate) - 's' prefers this over the
        # partial per-frame rolling buffer passed into update(), since a single raw
        # frame (or a few smoothed together) often only catches part of a fast-
        # scanned pattern. None in 'preview', where there's no accumulation at all.
        self.lastFullFrame: np.ndarray | None = None
        cv2.namedWindow(self.windowName, cv2.WINDOW_NORMAL)
        cv2.resizeWindow(self.windowName, width, height)

    def setProgress(self, current: int, total: int):
        self.progress = (current, total)

    def clearProgress(self):
        self.progress = None

    def _measureFps(self) -> float:
        now = time.monotonic()
        self._frameTimes.append(now)
        if len(self._frameTimes) < 2:
            return 0.0
        span = self._frameTimes[-1] - self._frameTimes[0]
        return (len(self._frameTimes) - 1) / span if span > 0 else 0.0

    def _saveFrame(self, frame: np.ndarray):
        """Saves a raw (un-zoomed, un-annotated) frame to results/ - full native
        resolution, so it's actually useful for offline inspection rather than just
        a copy of what the overlay shows. Prefers lastFullFrame (the complete
        accumFrames-accumulation from the most recent measurement) over the
        just-displayed partial rolling-buffer frame, so the saved image shows the
        whole projected shape instead of whatever fragment happened to be on
        screen the instant 's' was pressed."""
        frame = self.lastFullFrame if self.lastFullFrame is not None else frame
        try:
            RESULTS_DIR.mkdir(exist_ok=True)
            self._snapCounter += 1
            path = RESULTS_DIR / (f"snapshot_{time.strftime('%Y-%m-%d_%H-%M-%S')}"
                                  f"_{self._snapCounter}.png")
            ok = cv2.imwrite(str(path), frame)
        except (OSError, cv2.error) as e:
            prWarn(f"could not save snapshot: {e}")
            return
        if ok:
            prOk(f"saved snapshot -> {path.relative_to(Path(__file__).parent)}")
        else:
            prWarn(f"cv2.imwrite reported failure for {path.name}")

    def update(self, frame: np.ndarray, text: str = "") -> int:
        """Draws frame + overlay, polls for a keypress, returns the masked key code
        from cv2.waitKey(1) & 0xFF (255 if none) so callers can handle extra keys
        beyond zoom/pause/quit."""
        fps = self._measureFps()
        display = cv2.cvtColor(frame, cv2.COLOR_GRAY2BGR) if frame.ndim == 2 else frame.copy()
        display = applyZoom(display, self.zoomIdx)
        h, w = display.shape[:2]

        label = f"zoom {ZOOM_LEVELS[self.zoomIdx]:.0f}x   {fps:.1f} fps   {text}".strip()
        if self.paused:
            label = "PAUSED - " + label
        cv2.putText(display, label, (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2)

        if self.progress:
            current, total = self.progress
            pct = (current / total) if total else 0.0
            barX, barY, barW, barH = 10, 45, w - 20, 18
            cv2.rectangle(display, (barX, barY), (barX + barW, barY + barH), (90, 90, 90), 1)
            cv2.rectangle(display, (barX, barY),
                         (barX + int(barW * min(pct, 1.0)), barY + barH), (0, 200, 0), -1)
            cv2.putText(display, f"trial {current}/{total}  ({pct * 100:.0f}%)",
                       (barX + 6, barY + barH - 4), cv2.FONT_HERSHEY_SIMPLEX, 0.5,
                       (255, 255, 255), 1)

        if self.hotkeys:
            cv2.putText(display, self.hotkeys, (10, h - 12), cv2.FONT_HERSHEY_SIMPLEX, 0.5,
                       (0, 255, 255), 1)

        cv2.imshow(self.windowName, display)
        key = cv2.waitKey(1) & 0xFF
        self.lastKey = key
        if key in (ord("1"), ord("2"), ord("3")):
            self.zoomIdx = int(chr(key)) - 1
        elif key == ord("q"):
            self.quitRequested = True
        elif key == ord(" "):
            self.paused = not self.paused
        elif key == ord("s"):
            self._saveFrame(frame)
        return key

    def close(self):
        cv2.destroyWindow(self.windowName)


def waitWhilePaused(cam: "Camera"):
    """Blocks at a safe boundary (between trials/patterns, never mid-capture) while
    the attached LiveView is paused, keeping the window responsive (live feed, keys)
    until resumed. Raises KeyboardInterrupt if 'q' was pressed instead."""
    liveView = cam.liveView
    if not liveView:
        return
    if liveView.paused:
        cam.statusText = "PAUSED - press space to resume"
        while liveView.paused and not liveView.quitRequested:
            cam.grabGray()
    if liveView.quitRequested:
        raise KeyboardInterrupt()


def holdLiveView(cam: "Camera", seconds: float):
    """Keeps the camera view window open and live for at least `seconds` after a
    command finishes, instead of it vanishing the instant the last capture completes -
    otherwise there's no time to actually look at what was just measured/calibrated/
    diagnosed. Still pumps real frames through cam.grabGray() (not a frozen still) so
    the window stays responsive rather than looking hung; 'q' closes it immediately
    instead of waiting out the rest of the hold. No-op if there's no attached view."""
    liveView = cam.liveView
    if not liveView:
        return
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline and not liveView.quitRequested:
        remaining = deadline - time.monotonic()
        cam.statusText = f"done - closing in {remaining:.0f}s (press 'q' to close now)"
        try:
            cam.grabGray()
        except OptimizerError:
            break   # camera went away (unplugged/in use) - nothing left to hold open


# ── camera ───────────────────────────────────────────────────────────────────

CAMERA_BACKENDS = {"dshow": cv2.CAP_DSHOW, "msmf": cv2.CAP_MSMF, "any": cv2.CAP_ANY}

# "Manual exposure" is a UVC extension with no standardized value in DirectShow/
# Media Foundation - different drivers expect different magic numbers here, and
# silently keep auto-exposure running (ignoring the call, no error) if the wrong
# one is sent. Tried in order; readback against CAP_PROP_EXPOSURE picks the one
# that actually stuck. This is the #1 cause of "fps stuck low regardless of
# scene brightness" - auto-exposure lengthens integration time in normal indoor
# lighting, and a global-shutter sensor's fps is hard-capped at 1/exposure_time
# once that exceeds the frame period.
AUTO_EXPOSURE_MANUAL_CANDIDATES = (0.25, 1, 0, 3)


class Camera:
    def __init__(self, cfg: dict, liveView: LiveView | None = None):
        backendName = cfg.get("cameraBackend", "dshow")
        backend = CAMERA_BACKENDS.get(backendName, cv2.CAP_DSHOW)
        try:
            self.cap = cv2.VideoCapture(cfg["cameraIndex"], backend)
        except cv2.error as e:
            raise OptimizerError(
                f"OpenCV failed to open camera index {cfg['cameraIndex']}: {e}"
            ) from e
        if not self.cap.isOpened():
            available = probeCameras(backend=backend)
            hint = (f"detected working index(es): {available}" if available
                    else "no camera detected at all - check the USB connection/drivers")
            raise OptimizerError(
                f"camera index {cfg['cameraIndex']} not found or already in use by "
                f"another application ({hint}). Fix cameraIndex in {CONFIG_FILE.name} "
                f"or run 'optimizeGalvo.py wizard'."
            )
        self.cap.set(cv2.CAP_PROP_FRAME_WIDTH, cfg["frameWidth"])
        self.cap.set(cv2.CAP_PROP_FRAME_HEIGHT, cfg["frameHeight"])
        # Pixel format matters as much as resolution for achievable fps - see the
        # cameraFourcc comment in DEFAULT_CONFIG. Requested after resolution (matches
        # what was actually verified working against a real OV9281 in ov9281_probe.py).
        if cfg.get("cameraFourcc"):
            self.cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*cfg["cameraFourcc"]))
        # Not requested by default - the driver picks whatever mode it negotiates
        # for frameWidth/frameHeight, which is very often well below the sensor's
        # rated fps (e.g. OV9281's 120fps is usually only available at lower
        # resolutions/other UVC modes). Set cameraFps in the config to request a
        # specific rate explicitly; whether the driver honors it is hardware-dependent.
        if cfg.get("cameraFps"):
            self.cap.set(cv2.CAP_PROP_FPS, cfg["cameraFps"])

        appliedAutoExpVal = None
        for autoVal in AUTO_EXPOSURE_MANUAL_CANDIDATES:
            self.cap.set(cv2.CAP_PROP_AUTO_EXPOSURE, autoVal)
            self.cap.set(cv2.CAP_PROP_EXPOSURE, cfg["exposure"])
            for _ in range(2):
                self.cap.read()     # let the setting actually take before reading back
            if abs(self.cap.get(cv2.CAP_PROP_EXPOSURE) - cfg["exposure"]) < 1.0:
                appliedAutoExpVal = autoVal
                break
        # cv2's set() return value is a reliable, honest "does this control exist on this
        # camera" signal for plain IAMVideoProcAmp properties (unlike manual-exposure mode
        # above, which needs the readback dance since drivers silently ignore an
        # unrecognized Flags value instead of failing) - confirmed against a real OV9281
        # (VID_1BCF&PID_28C4), where Gain isn't exposed at all: set() returns False and
        # get() stays pinned at -1 no matter what's requested. autotune-camera checks
        # self.gainSupported to drop gain from its search space when it's not real.
        self.gainSupported = bool(self.cap.set(cv2.CAP_PROP_GAIN, cfg["gain"]))

        # ISP image controls pinned for measurement linearity/consistency, not searched -
        # see the DEFAULT_CONFIG comments for why each value was picked for this use case.
        # Not every camera exposes all of these (or any) - unsupported ones just no-op via
        # the same set()-return-value check as gain above.
        otherControls = {
            "brightness":           (cv2.CAP_PROP_BRIGHTNESS, cfg["brightness"]),
            "contrast":             (cv2.CAP_PROP_CONTRAST, cfg["contrast"]),
            "gamma":                (cv2.CAP_PROP_GAMMA, cfg["gamma"]),
            "sharpness":            (cv2.CAP_PROP_SHARPNESS, cfg["sharpness"]),
            "backlightCompensation": (cv2.CAP_PROP_BACKLIGHT, cfg["backlightCompensation"]),
        }
        self.otherControlsSupported = {
            name: bool(self.cap.set(prop, value)) for name, (prop, value) in otherControls.items()
        }

        for _ in range(3):          # flush pipeline after settings change
            self.cap.read()

        self.liveView = liveView
        self.statusText = ""
        self._displayBuffer = collections.deque(maxlen=max(1, cfg.get("displaySmoothFrames", 3)))
        self.lastAccumulated: np.ndarray | None = None
        reportedFps = self.cap.get(cv2.CAP_PROP_FPS)
        readExposure = self.cap.get(cv2.CAP_PROP_EXPOSURE)
        exposureStuck = appliedAutoExpVal is not None
        unsupportedOther = [name for name, ok in self.otherControlsSupported.items() if not ok]
        rawFourcc = int(self.cap.get(cv2.CAP_PROP_FOURCC))
        appliedFourcc = ("".join(chr((rawFourcc >> (8 * i)) & 0xFF) for i in range(4))
                        if rawFourcc > 0 else "?")
        prTable([
            ("resolution",   f"{cfg['frameWidth']}x{cfg['frameHeight']}"),
            ("backend",      backendName),
            ("fourcc",       f"requested {cfg.get('cameraFourcc') or '(driver default)'}, "
                              f"got {appliedFourcc}" + (
                                  "" if not cfg.get("cameraFourcc") or appliedFourcc == cfg["cameraFourcc"]
                                  else " - DID NOT STICK, check cameraFourcc/cameraBackend")),
            ("driver fps",   f"{reportedFps:.1f} (nominal - watch the live overlay for measured fps)"),
            ("exposure req", cfg["exposure"]),
            ("exposure got", f"{readExposure:.2f}"
                              + (f" (manual mode {appliedAutoExpVal})" if exposureStuck else " (auto-exposure)")),
            ("gain",         cfg["gain"] if self.gainSupported else "not supported by this camera - ignored"),
            ("other controls", "all applied" if not unsupportedOther
                              else f"not supported, ignored: {unsupportedOther}"),
        ], headers=("camera", "value"))
        if not exposureStuck:
            prWarn(f"none of the manual-exposure mode values {AUTO_EXPOSURE_MANUAL_CANDIDATES} "
                  f"stuck, so auto-exposure is very likely still active.")
            prTip(f"if the measured fps in the live overlay is low AND changes with scene "
                  f"brightness, this is why - auto-exposure is lengthening integration time "
                  f"in normal lighting, which caps a global-shutter sensor's fps at "
                  f"1/exposure_time. Try 'cameraBackend': 'msmf' in {CONFIG_FILE.name} (or "
                  f"'dshow' if already on msmf), or your camera vendor's own control app if "
                  f"neither UVC backend exposes a working manual-exposure switch.")

    def setExposureGain(self, exposure: int, gain: int):
        """Changes exposure/gain on an already-open camera (unlike __init__, this
        assumes manual-exposure mode already stuck - used by 'autotune-camera' and
        'preview' to try values live without reopening the device)."""
        self.cap.set(cv2.CAP_PROP_EXPOSURE, exposure)
        self.cap.set(cv2.CAP_PROP_GAIN, gain)
        for _ in range(2):      # let the change take effect before it's relied on
            self.grabGray()

    def grabGray(self) -> np.ndarray:
        ok, frame = self.cap.read()
        if not ok:
            raise OptimizerError(
                "camera read failed - it may have been unplugged or is in use by another "
                "application. Reconnect the camera and re-run the command."
            )
        if frame.ndim == 3:
            frame = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        if self.liveView:
            # Display-only smoothing: at high fps/short exposure a single raw frame
            # often only catches part of a fast vector-scanned pattern, which flickers
            # in the preview window even though nothing is wrong with the laser or the
            # actual measurement (grabAccumulated below does its own, separate, full
            # max-accumulation for scoring - this rolling buffer never touches that).
            self._displayBuffer.append(frame)
            display = self._displayBuffer[0]
            for f in list(self._displayBuffer)[1:]:
                display = np.maximum(display, f)
            self.liveView.update(display, self.statusText)
        return frame

    def grabAccumulated(self, nFrames: int) -> np.ndarray:
        """Max-accumulate frames so the full scan path appears even at short exposure."""
        acc = self.grabGray()
        for _ in range(nFrames - 1):
            np.maximum(acc, self.grabGray(), out=acc)
        self.lastAccumulated = acc
        if self.liveView:
            # 's' prefers this full accumulation over whatever partial rolling-buffer
            # frame is on screen at the moment the key is pressed (see LiveView._saveFrame) -
            # otherwise a snapshot mid-measurement only catches a few frames' worth of a
            # fast-scanned pattern, unlike e.g. measure's own saved PNG.
            self.liveView.lastFullFrame = acc
        return acc

    def grabBackground(self) -> np.ndarray:
        """Capture with laser blanked - call while pattern stopped."""
        return self.grabAccumulated(4)

    def grabMedian(self, nFrames: int) -> np.ndarray:
        """Median-stack nFrames. Unlike grabAccumulated()'s max()-projection - built
        for a fast-scanned pattern that only lights each pixel briefly per frame - a
        single STATIC dwell point (calibrate-warp) is lit continuously across every
        frame, so a median rejects transient sensor noise/reflections that max()
        would instead bake in permanently."""
        frames = [self.grabGray() for _ in range(max(1, nFrames))]
        stacked = np.median(np.stack(frames, axis=0), axis=0).astype(frames[0].dtype)
        self.lastAccumulated = stacked
        if self.liveView:
            self.liveView.lastFullFrame = stacked
        return stacked

    def release(self):
        self.cap.release()


# ── homography ───────────────────────────────────────────────────────────────

def detectDots(image: np.ndarray, expected: int, threshold: int) -> np.ndarray:
    _, binary = cv2.threshold(image, threshold, 255, cv2.THRESH_BINARY)
    binary = cv2.dilate(binary, np.ones((5, 5), np.uint8))
    contours, _ = cv2.findContours(binary, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    centers = []
    for c in contours:
        m = cv2.moments(c)
        if m["m00"] > 0:
            centers.append((m["m10"] / m["m00"], m["m01"] / m["m00"], m["m00"]))
    centers.sort(key=lambda t: -t[2])          # largest blobs first
    if len(centers) < expected:
        try:
            RESULTS_DIR.mkdir(exist_ok=True)
            failPath = RESULTS_DIR / "calibrate_failed.png"
            cv2.imwrite(str(failPath), image)
            savedHint = f" Failed capture saved to {failPath.relative_to(Path(__file__).parent)}."
        except OSError:
            savedHint = ""
        raise OptimizerError(
            f"calibration failed: expected {expected} reference dots but found "
            f"{len(centers)} in the camera image.{savedHint} Possible causes: laser not "
            f"armed or E-Stop/scan-fail tripped (run 'optimizeGalvo.py check'), camera "
            f"out of focus or badly framed (run 'optimizeGalvo.py preview' first), "
            f"exposure too low/high, or binaryThreshold in {CONFIG_FILE.name} (currently "
            f"{threshold}) not matching your setup."
        )
    return np.array([(x, y) for x, y, _ in centers[:expected]], dtype=np.float32)


def detectSinglePoint(image: np.ndarray, threshold: int, minAreaPx: float,
                      maxAreaPx: float, minPeakVal: float) -> tuple[float, float] | None:
    """calibrate-warp's per-point detector: one bright dwell dot, subpixel centroid
    via image moments. Returns None (does NOT raise) if no blob is found, or the
    largest blob's area/the frame's peak brightness falls outside the expected
    bounds - callers collect per-index failures and report them together (see
    runCalibrateWarp), rather than aborting on the first bad point or silently
    using a bad measurement."""
    if float(image.max()) < minPeakVal:
        return None
    _, binary = cv2.threshold(image, threshold, 255, cv2.THRESH_BINARY)
    binary = cv2.dilate(binary, np.ones((5, 5), np.uint8))
    contours, _ = cv2.findContours(binary, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    if not contours:
        return None
    best = max(contours, key=cv2.contourArea)
    area = cv2.contourArea(best)
    if area < minAreaPx or area > maxAreaPx:
        return None
    m = cv2.moments(best)
    if m["m00"] <= 0:
        return None
    return (m["m10"] / m["m00"], m["m01"] / m["m00"])


def detectStreakExtent(image: np.ndarray, threshold: int) -> float | None:
    """measure-resonance's per-frequency-step detector: peak-to-peak amplitude of a
    driven sine streak. Returns the largest blob's extent along its OWN long axis
    (cv2.minAreaRect), NOT a centroid - a symmetric back-and-forth streak's
    moments-centroid sits near the geometric middle regardless of amplitude (the
    same read detectSinglePoint() above uses for a STATIC dwell dot is
    amplitude-blind here), so extent is the only readout that actually tracks
    displacement. Returns None (does NOT raise) if no contour is found at all -
    callers additionally floor the returned extent against resonanceMinExtentPx
    (a thin, near-zero-amplitude streak is expected to have SOME contour, not
    zero). See docs/feature-prompts/DECISIONS.md, Prompt 13."""
    _, binary = cv2.threshold(image, threshold, 255, cv2.THRESH_BINARY)
    binary = cv2.dilate(binary, np.ones((3, 3), np.uint8))
    contours, _ = cv2.findContours(binary, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    if not contours:
        return None
    best = max(contours, key=cv2.contourArea)
    if len(best) < 2:
        return None
    (_, _), (w, h), _ = cv2.minAreaRect(best)
    return float(max(w, h))


def orderCorners(points: np.ndarray) -> np.ndarray:
    """Order 4 points: top-left, top-right, bottom-right, bottom-left (image space)."""
    s = points.sum(axis=1)
    d = np.diff(points, axis=1).ravel()
    return np.array([
        points[np.argmin(s)],      # TL
        points[np.argmin(d)],      # TR
        points[np.argmax(s)],      # BR
        points[np.argmax(d)],      # BL
    ], dtype=np.float32)


def runCalibrate(cfg: dict, esp: EspClient, cam: Camera):
    cornerNames = ["TL", "TR", "BR", "BL"]

    esp.stop()
    time.sleep(0.3)
    cam.statusText = "calibrate: background (laser off)"
    background = cam.grabBackground()

    waitWhilePaused(cam)  # safe boundary: laser is off, nothing running yet
    startResp = esp.startPattern("corners4", channel=cfg["camPatternChannel"])
    # corners4's actual DAC-space half-extent shrinks inward whenever the
    # controller's live dac_limit_min/max output-limiting window is tighter
    # than the nominal dacRange (see calib_patterns.cpp's cornersRadius()) -
    # points outside that window get force-blanked, not just dimmed, which is
    # exactly what made 'calibrate' find 0 dots despite the laser being armed.
    # Read back the value the controller ACTUALLY used instead of assuming
    # cfg["dacRange"] stayed in sync with it. Falls back to dacRange against
    # older firmware whose /api/calib-cam/start response has no "corner_r".
    r = startResp.get("corner_r", cfg["dacRange"])
    # DAC convention: x right, y down -> TL=(-r,-r), TR=(r,-r), BR=(r,r), BL=(-r,r)
    dacCorners = np.array([[-r, -r], [r, -r], [r, r], [-r, r]], dtype=np.float32)
    time.sleep(cfg["settleSeconds"])
    cam.statusText = "calibrate: corners4"
    image = cam.grabAccumulated(cfg["accumFrames"])
    esp.stop()

    diff = cv2.subtract(image, background)
    # orderCorners() labels purely by sum/diff of pixel coordinates (see its docstring) -
    # this assumes the 4 dots appear roughly axis-aligned in the photo. A camera viewing
    # the projection at a steep angle/rotation can make it mislabel which dot is which,
    # silently baking a skewed/mirrored homography that corners4 itself can never reveal
    # (any consistent labeling trivially satisfies its own definition) but that shows up
    # as a distorted/flipped-looking trace on star/square/etc., which sample more of the
    # interior. The labeled screenshot below is the way to catch that: the printed/drawn
    # TL/TR/BR/BL must match the dots' real physical layout as you view the wall.
    pixelCorners = orderCorners(detectDots(diff, 4, cfg["binaryThreshold"]))
    pr("detected corner dots (labeled by position in the photo) - these must match "
         "the real physical layout as you view the projection surface:")
    for label, (px, py) in zip(cornerNames, pixelCorners):
        pr(f"  {label}: pixel ({px:.0f}, {py:.0f})")

    try:
        RESULTS_DIR.mkdir(exist_ok=True)
        timestamp = time.strftime('%Y-%m-%d_%H-%M-%S')
        snapPath = RESULTS_DIR / f"calibrate_{timestamp}.png"
        if cv2.imwrite(str(snapPath), image):
            prOk(f"saved calibration snapshot -> {snapPath.relative_to(Path(__file__).parent)}")
        else:
            prWarn(f"cv2.imwrite reported failure for {snapPath.name}")

        labeled = cv2.cvtColor(diff, cv2.COLOR_GRAY2BGR)
        for label, (px, py) in zip(cornerNames, pixelCorners):
            cv2.circle(labeled, (int(px), int(py)), 10, (0, 255, 0), 2)
            cv2.putText(labeled, label, (int(px) + 14, int(py)), cv2.FONT_HERSHEY_SIMPLEX,
                       0.8, (0, 255, 0), 2, cv2.LINE_AA)
        labeledPath = RESULTS_DIR / f"calibrate_{timestamp}_labeled.png"
        if cv2.imwrite(str(labeledPath), labeled):
            prOk(f"saved labeled corners -> {labeledPath.relative_to(Path(__file__).parent)} "
                  f"- verify TL/TR/BR/BL against the dots' real physical layout")
        else:
            prWarn(f"cv2.imwrite reported failure for {labeledPath.name}")
    except (OSError, cv2.error) as e:
        prWarn(f"could not save calibration snapshot: {e}")

    h, _ = cv2.findHomography(pixelCorners, dacCorners)
    if h is None:
        raise OptimizerError(
            "homography computation failed - the 4 detected dots may be degenerate "
            "(collinear or overlapping). Check camera framing/focus with "
            "'optimizeGalvo.py preview' and re-run 'calibrate'."
        )

    try:
        np.savez(HOMOGRAPHY_FILE, homography=h, background=background)
    except OSError as e:
        raise OptimizerError(f"cannot write {HOMOGRAPHY_FILE.name}: {e}") from e
    prOk(f"homography saved -> {HOMOGRAPHY_FILE.name}")
    print(h)
    resetOrientationCache()
    prInfo(f"cleared {ORIENTATION_FILE.name} (if present) - orientation will be "
         f"re-detected fresh for each pattern on next measurement")


def loadHomography() -> tuple[np.ndarray, np.ndarray]:
    if not HOMOGRAPHY_FILE.exists():
        raise OptimizerError(
            f"no {HOMOGRAPHY_FILE.name} found - run 'optimizeGalvo.py calibrate' first."
        )
    try:
        data = np.load(HOMOGRAPHY_FILE)
        return data["homography"], data["background"]
    except Exception as e:
        # numpy raises different types (zipfile.BadZipFile, OSError, KeyError, ValueError...)
        # for a corrupted/incomplete .npz - the remedy is always the same: recalibrate.
        raise OptimizerError(
            f"{HOMOGRAPHY_FILE.name} is corrupted or incomplete ({e}). Delete it and "
            f"re-run 'optimizeGalvo.py calibrate'."
        ) from e


# ── warp-grid calibration (Prompt 7b) ─────────────────────────────────────────
#
# Distinct from calibrate/homography.npz above: that homography maps CAMERA
# PIXELS -> DAC space for SCORING optimizer trials against an ideal shape.
# calibrate-warp instead SOLVES the /api/warp/* grid itself, by commanding
# exact DAC-space points (POST /api/debug/hw, which bypasses pattern
# generation, the warp stage, AND applyCalibration()'s gain/offset/
# outputScale/mirror entirely - see EspClient.debugHw) and observing where
# they physically land on camera, then working out what DAC position WOULD
# need to be commanded - through the REAL pipeline, warp included - to land
# each control point at its intended spot instead.

@dataclass
class CalibTransform:
    """Models pattern_engine.cpp::applyCalibration()'s fixed per-axis affine
    chain (mirror/invert/gain/offset/outputScale) exactly, using live values
    read from GET /api/config. Warp operates on PATTERN-space coordinates
    (BEFORE this chain runs); /api/debug/hw commands raw DAC-space
    coordinates (AFTER it). This class converts between the two spaces, so
    debug/hw can be used to precisely probe/position a point in DAC space
    while still reasoning about the pattern-space coordinates the firmware's
    warp grid actually operates on. Deliberately excludes the dac_limit
    clamp (non-invertible, lossy) - callers must keep commanded points
    within [dac_limit_min, dac_limit_max] themselves."""
    swapXy: bool
    invertX: bool
    invertY: bool
    gainX: float
    gainY: float
    offsetX: float
    offsetY: float
    outputScale: float

    @classmethod
    def fromEspConfig(cls, espCfg: dict) -> "CalibTransform":
        return cls(
            swapXy=bool(espCfg.get("swap_xy")),
            invertX=bool(espCfg.get("invert_x")),
            invertY=bool(espCfg.get("invert_y")),
            gainX=float(espCfg.get("galvo_x_gain", 32767)) or 32767.0,
            gainY=float(espCfg.get("galvo_y_gain", 32767)) or 32767.0,
            offsetX=float(espCfg.get("galvo_x_offset", 0)),
            offsetY=float(espCfg.get("galvo_y_offset", 0)),
            outputScale=float(espCfg.get("output_scale", 1.0)) or 1.0,
        )

    def toDac(self, x: float, y: float) -> tuple[float, float]:
        """Pattern-space -> DAC-space (mirrors applyCalibration() exactly, minus
        the final dac_limit clamp)."""
        if self.swapXy:
            x, y = y, x
        x = -x                              # fixed physical mirror (unconditional)
        if self.invertX:
            x = -x
        if self.invertY:
            y = -y
        x = x * self.gainX / 32767.0
        y = y * self.gainY / 32767.0
        x += self.offsetX
        y += self.offsetY
        x *= self.outputScale
        y *= self.outputScale
        return x, y

    def toPattern(self, dacX: float, dacY: float) -> tuple[float, float]:
        """DAC-space -> pattern-space (exact inverse of toDac())."""
        x = dacX / self.outputScale
        y = dacY / self.outputScale
        x -= self.offsetX
        y -= self.offsetY
        x = x * 32767.0 / self.gainX
        y = y * 32767.0 / self.gainY
        if self.invertX:
            x = -x
        if self.invertY:
            y = -y
        x = -x                              # undo fixed physical mirror
        if self.swapXy:
            x, y = y, x
        return x, y


def identityGridPatternPos(n: int, r: int, c: int, dacRange: float) -> tuple[float, float]:
    """Pattern-space position of identity-grid control point (r,c) for an n x n
    warp grid - matches config.h's WarpConfig::resetIdentity() exactly (its
    normalized [-1..1] formula, scaled here to pattern-space DAC units by
    dacRange)."""
    u = (-1.0 + (2.0 * c) / (n - 1)) if n > 1 else 0.0
    v = (-1.0 + (2.0 * r) / (n - 1)) if n > 1 else 0.0
    return u * dacRange, v * dacRange


def _parseTargetRect(spec: str | None) -> tuple[float, float, float, float] | None:
    """Parses --target-rect's "X0,Y0,X1,Y1" into a tuple, or None if omitted (the
    caller then falls back to the interactive click picker)."""
    if spec is None:
        return None
    parts = [p.strip() for p in spec.split(",")]
    if len(parts) != 4:
        raise OptimizerError(
            f"--target-rect must be X0,Y0,X1,Y1 (4 comma-separated numbers), got: {spec!r}"
        )
    try:
        x0, y0, x1, y1 = (float(p) for p in parts)
    except ValueError as e:
        raise OptimizerError(f"--target-rect values must be numbers: {spec!r} ({e})") from e
    if x1 <= x0 or y1 <= y0:
        raise OptimizerError(
            f"--target-rect: X1 must be > X0 and Y1 must be > Y0, got {spec!r}"
        )
    return (x0, y0, x1, y1)


def _channelToRgb(channel: int) -> tuple[int, int, int]:
    return {0: (255, 255, 255), 1: (255, 0, 0), 2: (0, 255, 0), 3: (0, 0, 255)}.get(
        channel, (0, 0, 255))


def _resolveTargetRect(cam: "Camera",
                       targetRect: tuple[float, float, float, float] | None) -> np.ndarray:
    """Returns the 4 target pixel corners [TL,TR,BR,BL] (image space) the warp grid
    should map its outer edge onto."""
    if targetRect is not None:
        x0, y0, x1, y1 = targetRect
        return np.array([[x0, y0], [x1, y0], [x1, y1], [x0, y1]], dtype=np.float32)
    if not sys.stdin.isatty():
        raise OptimizerError(
            "calibrate-warp needs --target-rect in a non-interactive session "
            "(no terminal to click corners in)"
        )
    return _clickTargetRect(cam)


def _clickTargetRect(cam: "Camera") -> np.ndarray:
    """Interactive fallback for --target-rect: shows one live-ish frame, lets the
    user click the 4 corners of the intended rectangle in any order (ordered
    afterwards via the same orderCorners() runCalibrate uses), 'q'/Esc cancels."""
    frame = cam.grabGray()
    display = cv2.cvtColor(frame, cv2.COLOR_GRAY2BGR)
    clicked: list[tuple[int, int]] = []

    def onMouse(event, x, y, flags, param):
        if event == cv2.EVENT_LBUTTONDOWN and len(clicked) < 4:
            clicked.append((x, y))

    winName = "calibrate-warp: click the 4 target-rectangle corners (any order), q to cancel"
    cv2.namedWindow(winName)
    cv2.setMouseCallback(winName, onMouse)
    try:
        while len(clicked) < 4:
            frameDisp = display.copy()
            for i, (x, y) in enumerate(clicked):
                cv2.circle(frameDisp, (x, y), 8, (0, 255, 0), 2)
                cv2.putText(frameDisp, str(i + 1), (x + 12, y), cv2.FONT_HERSHEY_SIMPLEX,
                           0.7, (0, 255, 0), 2, cv2.LINE_AA)
            cv2.putText(frameDisp, f"clicked {len(clicked)}/4", (10, 30),
                       cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 255, 255), 2, cv2.LINE_AA)
            cv2.imshow(winName, frameDisp)
            key = cv2.waitKey(30) & 0xFF
            if key == ord('q') or key == 27:
                raise OptimizerError("calibrate-warp: target-rectangle selection cancelled")
    finally:
        cv2.destroyWindow(winName)
    return orderCorners(np.array(clicked, dtype=np.float32))


def _commandAndMeasure(esp: EspClient, cam: "Camera", cfg: dict, background: np.ndarray,
                       dacX: float, dacY: float, colorRgb: tuple[int, int, int],
                       label: str) -> tuple[float, float] | None:
    """Commands one raw DAC-space point (POST /api/debug/hw), captures + detects its
    subpixel centroid. Returns None (does not raise) on detection failure."""
    x = int(round(max(-32767.0, min(32767.0, dacX))))
    y = int(round(max(-32767.0, min(32767.0, dacY))))
    r, g, b = colorRgb
    esp.debugHw(x, y, r, g, b)
    time.sleep(cfg["settleSeconds"])
    cam.statusText = f"calibrate-warp: {label}"
    frame = cam.grabMedian(cfg["warpCalibFrames"])
    diff = cv2.subtract(frame, background)
    return detectSinglePoint(diff, cfg["binaryThreshold"], cfg["warpCalibMinBlobAreaPx"],
                             cfg["warpCalibMaxBlobAreaPx"], cfg["warpCalibMinPeakVal"])


def runCalibrateWarp(cfg: dict, esp: EspClient, cam: "Camera", gridSize: int,
                     targetRect: tuple[float, float, float, float] | None,
                     frames: int | None, dryRun: bool):
    if frames is not None:
        cfg = {**cfg, "warpCalibFrames": frames}
    n = gridSize

    espCfg = esp.getConfig()
    transform = CalibTransform.fromEspConfig(espCfg)
    dacRange = float(cfg["dacRange"])
    limLo = int(espCfg.get("dac_limit_min", 0x0666)) - 32768
    limHi = int(espCfg.get("dac_limit_max", 0xF999)) - 32768
    colorRgb = _channelToRgb(cfg["camPatternChannel"])

    esp.stop()          # in case a calib-cam session was left running
    esp.warpTest(False)  # in case a previous calibrate-warp run was interrupted
    esp.warpReset()      # POST /api/warp/reset first - calibration runs on an identity grid

    # From here on, /api/debug/hw is commanded repeatedly - wrap in try/finally so
    # a mid-loop error, a failed-detection abort, or Ctrl+C still blanks the beam
    # instead of leaving it parked lit at the last commanded point.
    try:
        return _runCalibrateWarpBody(cfg, esp, cam, n, targetRect, dryRun, transform,
                                    dacRange, limLo, limHi, colorRgb)
    finally:
        esp.debugHwOff()


def _runCalibrateWarpBody(cfg: dict, esp: EspClient, cam: "Camera", n: int,
                          targetRect: tuple[float, float, float, float] | None,
                          dryRun: bool, transform: CalibTransform, dacRange: float,
                          limLo: int, limHi: int, colorRgb: tuple[int, int, int]):
    targetCorners = _resolveTargetRect(cam, targetRect)
    pr("target rectangle (image pixels), TL/TR/BR/BL:")
    for label, (px, py) in zip(("TL", "TR", "BR", "BL"), targetCorners):
        pr(f"  {label}: ({px:.0f}, {py:.0f})")

    def targetPixelFor(r: int, c: int) -> np.ndarray:
        # Outer edge lands on targetCorners; interior control points are meant to
        # sit on an evenly-spaced grid INSIDE it - bilinear across the 4 corners.
        #
        # Row convention MUST match identityGridPatternPos()'s: r=0 -> v=-1, i.e.
        # pattern-space BOTTOM (config.h's WarpConfig::resetIdentity() / firmware's
        # warpGrid.cpp sampleGrid(), both v=-1 at r=0). targetCorners are in CAMERA
        # PIXEL space, where row 0 (small y) is the image TOP - so r=0 must resolve
        # to the BOTTOM target corners (BL/BR), not TOP. Getting this backwards (as
        # a prior version did) commands each identity point at its true DAC
        # position but solves it against the vertically-opposite pixel target,
        # baking a full row-inversion into the fitted grid - invisible on
        # point-symmetric calibration shapes (circle/square/star) but a hard
        # top/bottom flip on anything asymmetric (text, triangles).
        u = c / (n - 1) if n > 1 else 0.5
        v = r / (n - 1) if n > 1 else 0.5
        top = targetCorners[0] * (1 - u) + targetCorners[1] * u
        bot = targetCorners[3] * (1 - u) + targetCorners[2] * u
        return bot * (1 - v) + top * v

    ids = [(r, c) for r in range(n) for c in range(n)]
    pr(f"projecting {n * n} identity-grid control point(s) as single dwell dots ...")
    background = cam.grabMedian(4)

    measured: dict[tuple[int, int], tuple[float, float]] = {}
    commandedDac: dict[tuple[int, int], tuple[float, float]] = {}
    failed: list[str] = []
    for idx, (r, c) in enumerate(ids):
        patX, patY = identityGridPatternPos(n, r, c, dacRange)
        dacX, dacY = transform.toDac(patX, patY)
        if not (limLo <= dacX <= limHi and limLo <= dacY <= limHi):
            failed.append(
                f"({r},{c}): identity position clips dac_limit_min/max "
                f"(DAC {dacX:.0f},{dacY:.0f} outside [{limLo},{limHi}]) - lower "
                f"dacRange in {CONFIG_FILE.name} or widen the DAC scan limit in "
                f"the Calibration tab"
            )
            continue
        p = _commandAndMeasure(esp, cam, cfg, background, dacX, dacY, colorRgb,
                              f"point {idx + 1}/{n * n} ({r},{c})")
        if p is None:
            failed.append(f"({r},{c}): no dot detected at commanded DAC ({dacX:.0f},{dacY:.0f})")
            continue
        measured[(r, c)] = p
        commandedDac[(r, c)] = (dacX, dacY)
    esp.debugHwOff()

    if failed:
        raise OptimizerError(
            f"calibrate-warp: {len(failed)} of {n * n} control point(s) failed "
            "detection:\n  " + "\n  ".join(failed) +
            f"\nCheck the laser is armed, camera focus/exposure ('optimizeGalvo.py "
            f"preview'), and warpCalibMinBlobAreaPx/warpCalibMaxBlobAreaPx/"
            f"warpCalibMinPeakVal/binaryThreshold in {CONFIG_FILE.name}."
        )

    beforeErrs = [float(np.hypot(*(np.array(measured[k]) - targetPixelFor(*k)))) for k in ids]
    prTable([("mean", f"{np.mean(beforeErrs):.1f} px"), ("max", f"{np.max(beforeErrs):.1f} px")],
           headers=("before-correction residual", ""))

    # ── Solve ────────────────────────────────────────────────────────────────
    # H maps DAC-space -> measured pixel-space (fit from the 4 corners' known
    # commanded DAC position and their measured pixel landing spot). Its inverse
    # therefore maps a DESIRED pixel position to the DAC position that produces
    # it - evaluating Hinv at the target pixel corners gives the corrected DAC
    # coordinates directly. (Do NOT fit pixel->pixel here - that just gives back
    # the original measured pixels, not a DAC position, since the target and
    # measured corners share the same correspondence order by construction.)
    cornerIds = [(0, 0), (0, n - 1), (n - 1, n - 1), (n - 1, 0)]  # TL,TR,BR,BL
    dacCornerPts = np.array([commandedDac[k] for k in cornerIds], dtype=np.float32)
    measuredCornerPts = np.array([measured[k] for k in cornerIds], dtype=np.float32)
    H, _ = cv2.findHomography(dacCornerPts, measuredCornerPts)
    if H is None:
        raise OptimizerError(
            "calibrate-warp: homography solve failed - the 4 corner measurements "
            "may be degenerate (collinear/overlapping). Check camera framing "
            "('optimizeGalvo.py preview') and re-run."
        )
    Hinv = np.linalg.inv(H)

    def pixelToDacViaH(px: float, py: float) -> tuple[float, float]:
        v = Hinv @ np.array([px, py, 1.0])
        return float(v[0] / v[2]), float(v[1] / v[2])

    solvedDac: dict[tuple[int, int], tuple[float, float]] = {}
    if n == 2:
        # Exactly 4 correspondences determine the homography - no further
        # measurement needed, just evaluate it at the 4 target corners.
        for k in ids:
            solvedDac[k] = pixelToDacViaH(*targetPixelFor(*k))
    else:
        tolerancePx = cfg["warpCalibToleranceCameraPx"]
        for (r, c) in ids:
            target = targetPixelFor(r, c)
            targetDac = pixelToDacViaH(*target)   # homography-seeded initial guess
            guess = targetDac
            for roundIdx in range(3):
                gx = max(limLo, min(limHi, guess[0]))
                gy = max(limLo, min(limHi, guess[1]))
                p = _commandAndMeasure(esp, cam, cfg, background, gx, gy, colorRgb,
                                      f"refine ({r},{c}) round {roundIdx + 1}/3")
                if p is None:
                    break   # keep the last guess rather than failing the whole run
                errPx = float(np.hypot(p[0] - target[0], p[1] - target[1]))
                guess = (gx, gy)
                if errPx <= tolerancePx:
                    break
                # Feedback correction: pixelToDacViaH is a FIXED local linear model
                # (the corner homography) of the real, non-projective (piecewise-
                # bilinear-ish) transfer function. Its own self-consistency error at
                # the currently commanded point - targetDac - pixelToDacViaH(measured)
                # - is the model's estimate of how far off THIS guess is in DAC
                # space, added back onto the guess (proportional/Newton-style
                # feedback control, not a full per-point Jacobian).
                modelDacAtMeasured = pixelToDacViaH(*p)
                guess = (gx + (targetDac[0] - modelDacAtMeasured[0]),
                        gy + (targetDac[1] - modelDacAtMeasured[1]))
            solvedDac[(r, c)] = guess
        esp.debugHwOff()

    # ── Verify (after) ──────────────────────────────────────────────────────
    afterErrs = []
    for k in ids:
        gx, gy = solvedDac[k]
        gx = max(limLo, min(limHi, gx))
        gy = max(limLo, min(limHi, gy))
        p = _commandAndMeasure(esp, cam, cfg, background, gx, gy, colorRgb,
                              f"verify ({k[0]},{k[1]})")
        if p is not None:
            target = targetPixelFor(*k)
            afterErrs.append(float(np.hypot(p[0] - target[0], p[1] - target[1])))
    esp.debugHwOff()
    if afterErrs:
        prTable([("mean", f"{np.mean(afterErrs):.1f} px"), ("max", f"{np.max(afterErrs):.1f} px")],
               headers=("after-correction residual", ""))
    else:
        prWarn("after-correction verification capture failed for every point - "
              "residual not re-measured (grid was still solved)")

    # ── Emit normalized grid (firmware warpPoints format) ───────────────────
    points = [[[0.0, 0.0] for _ in range(n)] for _ in range(n)]
    clipped = []
    for (r, c), (dx, dy) in solvedDac.items():
        px, py = transform.toPattern(dx, dy)
        u, v = px / dacRange, py / dacRange
        if abs(u) > 1.5 or abs(v) > 1.5:
            clipped.append(f"({r},{c})")
        u = max(-1.5, min(1.5, u))
        v = max(-1.5, min(1.5, v))
        points[r][c] = [round(u, 5), round(v, 5)]
    if clipped:
        prWarn(f"control point(s) {', '.join(clipped)} needed a correction beyond the "
              f"+-1.5 normalized range and were clamped - the required correction may "
              f"be too large for this dacRange/projector geometry")

    result = {"gridSize": n, "points": points, "enabled": True}
    print(json.dumps(result, indent=2))

    if dryRun:
        prInfo("dry-run: nothing was POSTed to the ESP32")
        return

    esp.warpSet(n, points, enabled=True)
    prOk("warp grid applied via /api/warp/set")


# ── DAC-range clip detection + auto-tune (Prompt 10) ──────────────────────────
#
# Distinct from Prompt 9a's firmware-side dacClipX/Y counters (measureOnce() above):
# those flag DAC-CODE clipping against dac_limit_min/max, the fixed OPA-safety clamp.
# This is a CAMERA-side read - it looks at where the projected 'square' calib-cam
# pattern's bounding box sits relative to the CAPTURED FRAME's own border, to catch
# the image running off the visible/projectable area (or under-filling it), which is
# a framing/calibration problem, not a DAC-safety one. Auto-tunes galvo_x/y_gain and
# galvo_x/y_offset - the same live-settable fields (POST /api/calib-live) 7b/9b/the
# WebUI's Calibration card already expose - rather than dac_limit_min/max itself,
# since that clamp is the hardware OPA-clipping safety margin (docs/HARDWARE.md),
# not a free per-axis framing knob. Like calibrate-warp/measure-resonance, needs no
# prior homography.npz - it only ever reasons in camera-pixel space.

# 'square' (calib_patterns.cpp's cam_square()) is a fixed, static, closed 4-vertex
# outline at pattern-space +-CAM_H with sharp 90deg corners and single-channel
# max-brightness color (camColorOut, same 0/255-only convention as every other
# calib-cam pattern) - the thin rectangle-outline test pattern this feature needs.
# Its corner-dwell points are the only place ringing/overshoot can show up; detecting
# via the bounding rect's SIDES (never its corners, see detectClipping below) keeps
# that isolated and out of the clip/underscan read.
CALIB_CAM_SQUARE_HALF = 15000.0   # mirrors calib_patterns.cpp's CAM_H

DAC_RANGE_TUNE_CLIP_MARGIN_FRAC = 0.03     # bbox within this fraction of the frame
                                            # border on a side -> that side is CLIPped
DAC_RANGE_TUNE_DEADBAND_FRAC = 0.025       # convergence guard: freeze a side once its
                                            # margin sits within [CLIP_MARGIN_FRAC,
                                            # CLIP_MARGIN_FRAC + this] - i.e. it just
                                            # cleared the clip threshold with a small,
                                            # safe margin (the goal: hug the frame border
                                            # as closely as safely possible). Without
                                            # this a side could shrink past the clip
                                            # threshold and expand back past it forever.
DAC_RANGE_TUNE_UNDERSCAN_FRAC = (DAC_RANGE_TUNE_CLIP_MARGIN_FRAC
                                 + DAC_RANGE_TUNE_DEADBAND_FRAC)  # bbox clears this much
                                            # margin on BOTH sides of an axis ->
                                            # UNDERSCANning. Deliberately pinned to sit
                                            # right at the deadband's outer edge, not a
                                            # separately-tuned wider value - a gap between
                                            # "frozen" and "underscan" would be a dead
                                            # zone where a side is neither clipped,
                                            # underscanning, nor within the freeze
                                            # deadband, so autoTuneDacRange never touches
                                            # it and the loop can never converge on it.
DAC_RANGE_TUNE_MIN_SPAN_UNITS = 4000.0     # never let a shrink collapse an axis' span
                                            # below this


@dataclass
class AxisClipStatus:
    """Per-axis clip/underscan read for one 'tune-dac-range' capture. low/high refer
    to the bounding box's near-zero-side / far-side edge (left/top vs. right/bottom in
    frame pixels) - kept separate rather than one combined per-axis verdict because a
    physically off-center projection can clip on only one side while the other
    underscans."""
    lowClipped: bool
    highClipped: bool
    underscan: bool
    lowMarginFrac: float
    highMarginFrac: float

    @property
    def label(self) -> str:
        if self.lowClipped and self.highClipped:
            return "CLIP_BOTH"
        if self.lowClipped:
            return "CLIP_LOW"
        if self.highClipped:
            return "CLIP_HIGH"
        if self.underscan:
            return "UNDERSCAN"
        return "OK"


def detectClipping(frame: np.ndarray, threshold: int) -> dict[str, AxisClipStatus]:
    """Bounding-box-only clip/underscan detector for a projected calib-cam test
    pattern capture (already background-subtracted). Deliberately does NOT do corner/
    edge shape detection - thresholds frame to a binary mask, then cv2.boundingRect(
    mask) alone gives the four extremes the beam actually reached. Reading the
    resulting box's SIDES (never its corners) keeps corner-dwell ringing/overshoot -
    which stays localized to the shape's actual corners - from ever contaminating the
    edge-clip read. Returns {'x': AxisClipStatus, 'y': AxisClipStatus}."""
    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY) if frame.ndim == 3 else frame
    _, mask = cv2.threshold(gray, threshold, 255, cv2.THRESH_BINARY)
    if not np.any(mask):
        raise OptimizerError(
            "detectClipping: nothing above threshold in the capture - check the laser "
            "is armed and 'square' is actually projecting (see 'optimizeGalvo.py "
            "preview' to verify focus/exposure)."
        )
    x, y, w, h = cv2.boundingRect(mask)
    frameH, frameW = mask.shape[:2]

    def axisStatus(lo: float, hi: float, span: float) -> AxisClipStatus:
        lowFrac = lo / span
        highFrac = (span - hi) / span
        lowClipped = lowFrac <= DAC_RANGE_TUNE_CLIP_MARGIN_FRAC
        highClipped = highFrac <= DAC_RANGE_TUNE_CLIP_MARGIN_FRAC
        underscan = (not lowClipped and not highClipped
                    and lowFrac >= DAC_RANGE_TUNE_UNDERSCAN_FRAC
                    and highFrac >= DAC_RANGE_TUNE_UNDERSCAN_FRAC)
        return AxisClipStatus(lowClipped, highClipped, underscan, lowFrac, highFrac)

    return {
        "x": axisStatus(x, x + w, frameW),
        "y": axisStatus(y, y + h, frameH),
    }


def autoTuneDacRange(currentRange: tuple[float, float, float, float],
                     clipStatus: dict[str, AxisClipStatus],
                     step: float = 600.0) -> tuple[float, float, float, float]:
    """One bounded adjustment step over (xMin, xMax, yMin, yMax) DAC-code-space
    bounds: shrinks the side(s) that clipped (moving that bound inward, towards
    center), expands both sides of an axis that's underscanning, and leaves a side
    untouched otherwise - including when the caller has zeroed a frozen side's flags
    (see runTuneDacRange's convergence guard). Clamps to the DAC8562's raw int16
    range and enforces a minimum span so opposite-side clipping can never collapse an
    axis to zero width."""
    xMin, xMax, yMin, yMax = currentRange
    xs, ys = clipStatus["x"], clipStatus["y"]

    if xs.lowClipped:
        xMin += step
    elif xs.underscan:
        xMin -= step
    if xs.highClipped:
        xMax -= step
    elif xs.underscan:
        xMax += step

    if ys.lowClipped:
        yMin += step
    elif ys.underscan:
        yMin -= step
    if ys.highClipped:
        yMax -= step
    elif ys.underscan:
        yMax += step

    def clampSpan(lo: float, hi: float) -> tuple[float, float]:
        lo = max(-32768.0, min(32767.0, lo))
        hi = max(-32768.0, min(32767.0, hi))
        if hi - lo < DAC_RANGE_TUNE_MIN_SPAN_UNITS:
            mid = (lo + hi) / 2.0
            lo = mid - DAC_RANGE_TUNE_MIN_SPAN_UNITS / 2.0
            hi = mid + DAC_RANGE_TUNE_MIN_SPAN_UNITS / 2.0
        return lo, hi

    xMin, xMax = clampSpan(xMin, xMax)
    yMin, yMax = clampSpan(yMin, yMax)
    return xMin, xMax, yMin, yMax


def _inDeadband(marginFrac: float) -> bool:
    return (DAC_RANGE_TUNE_CLIP_MARGIN_FRAC <= marginFrac
           <= DAC_RANGE_TUNE_CLIP_MARGIN_FRAC + DAC_RANGE_TUNE_DEADBAND_FRAC)


def _squarePatternExtremes(transform: CalibTransform, half: float) -> tuple[float, float, float, float]:
    """Returns (pXlo, pXhi, pYlo, pYhi): the 'square' pattern's two corner positions
    per axis, AFTER mirror/invert/swap but BEFORE gain/offset/outputScale - evaluated
    through a neutral transform (gain=32767, offset=0, outputScale=1, same swap/
    invert bits as the real one) since those three steps of CalibTransform.toDac()
    reduce to the identity at those neutral values. applyCalibration()'s gain/offset/
    outputScale is then a pure per-axis affine step on top of these two fixed points -
    see _solveGainOffset()."""
    neutral = CalibTransform(swapXy=transform.swapXy, invertX=transform.invertX,
                             invertY=transform.invertY, gainX=32767.0, gainY=32767.0,
                             offsetX=0.0, offsetY=0.0, outputScale=1.0)
    ax, ay = neutral.toDac(-half, -half)
    bx, by = neutral.toDac(half, half)
    xLo, xHi = sorted((ax, bx))
    yLo, yHi = sorted((ay, by))
    return xLo, xHi, yLo, yHi


def _solveGainOffset(pLo: float, pHi: float, dacLo: float, dacHi: float,
                     outputScale: float) -> tuple[float, float]:
    """Exact inverse of applyCalibration()'s per-axis gain/offset/outputScale chain:
    solves the (gain, offset) pair that makes pattern-space corner positions pLo/pHi
    (already mirror/inverted - see _squarePatternExtremes()) land exactly on DAC
    positions dacLo/dacHi, holding outputScale fixed (it's a single global knob shared
    by both axes - 9b's own separate pre-clamp scale - not part of this per-axis
    solve). Same "exact mechanical inverse, not an empirical fit" approach as
    calibrate-warp's CalibTransform (Prompt 7b)."""
    qLo, qHi = dacLo / outputScale, dacHi / outputScale
    gain = (qHi - qLo) * 32767.0 / (pHi - pLo)
    offset = qLo - pLo * gain / 32767.0
    return gain, offset


def runTuneDacRange(cfg: dict, esp: EspClient, cam: "Camera", maxIterations: int, dryRun: bool):
    espCfg = esp.getConfig()
    baseline = CalibTransform.fromEspConfig(espCfg)
    limLo = int(espCfg.get("dac_limit_min", 0x0666)) - 32768
    limHi = int(espCfg.get("dac_limit_max", 0xF999)) - 32768

    def restoreBaseline():
        esp.setCalibLive(galvo_x_gain=int(round(baseline.gainX)),
                        galvo_y_gain=int(round(baseline.gainY)),
                        galvo_x_offset=int(round(baseline.offsetX)),
                        galvo_y_offset=int(round(baseline.offsetY)))

    pXlo, pXhi, pYlo, pYhi = _squarePatternExtremes(baseline, CALIB_CAM_SQUARE_HALF)
    dacXlo, dacYlo = baseline.toDac(-CALIB_CAM_SQUARE_HALF, -CALIB_CAM_SQUARE_HALF)
    dacXhi, dacYhi = baseline.toDac(CALIB_CAM_SQUARE_HALF, CALIB_CAM_SQUARE_HALF)
    currentRange = (min(dacXlo, dacXhi), max(dacXlo, dacXhi),
                    min(dacYlo, dacYhi), max(dacYlo, dacYhi))
    pr(f"starting DAC range (from live gain/offset): "
      f"X [{currentRange[0]:.0f}, {currentRange[1]:.0f}]  "
      f"Y [{currentRange[2]:.0f}, {currentRange[3]:.0f}]")

    esp.stop()
    time.sleep(0.3)
    cam.statusText = "tune-dac-range: background (laser off)"
    background = cam.grabBackground()

    frozen = {"xLow": False, "xHigh": False, "yLow": False, "yHigh": False}
    converged = False
    lastGainOffset = (baseline.gainX, baseline.gainY, baseline.offsetX, baseline.offsetY)
    try:
        for i in range(maxIterations):
            gainX, offsetX = _solveGainOffset(pXlo, pXhi, currentRange[0], currentRange[1],
                                              baseline.outputScale)
            gainY, offsetY = _solveGainOffset(pYlo, pYhi, currentRange[2], currentRange[3],
                                              baseline.outputScale)
            gainX = max(100.0, min(32767.0, gainX))
            gainY = max(100.0, min(32767.0, gainY))
            offsetX = max(-32768.0, min(32767.0, offsetX))
            offsetY = max(-32768.0, min(32767.0, offsetY))
            lastGainOffset = (gainX, gainY, offsetX, offsetY)
            esp.setCalibLive(galvo_x_gain=int(round(gainX)), galvo_y_gain=int(round(gainY)),
                             galvo_x_offset=int(round(offsetX)), galvo_y_offset=int(round(offsetY)))

            waitWhilePaused(cam)  # safe boundary: laser is off (esp.stop() below already ran once)
            esp.startPattern("square", channel=cfg["camPatternChannel"])
            time.sleep(cfg["patternSwitchSettleSeconds"])
            time.sleep(cfg["settleSeconds"])
            cam.statusText = f"tune-dac-range: iteration {i + 1}/{maxIterations}"
            capture = cam.grabAccumulated(cfg["accumFrames"])
            esp.stop()
            time.sleep(cfg["patternSwitchSettleSeconds"])

            diff = cv2.subtract(capture, background)
            status = detectClipping(diff, cfg["binaryThreshold"])
            xs, ys = status["x"], status["y"]

            pr(f"iter {i + 1}/{maxIterations}: X={xs.label} (margins {xs.lowMarginFrac:.1%}/"
              f"{xs.highMarginFrac:.1%})  Y={ys.label} (margins {ys.lowMarginFrac:.1%}/"
              f"{ys.highMarginFrac:.1%})  range X[{currentRange[0]:.0f},{currentRange[1]:.0f}] "
              f"Y[{currentRange[2]:.0f},{currentRange[3]:.0f}]")

            if not frozen["xLow"] and _inDeadband(xs.lowMarginFrac):
                frozen["xLow"] = True
            if not frozen["xHigh"] and _inDeadband(xs.highMarginFrac):
                frozen["xHigh"] = True
            if not frozen["yLow"] and _inDeadband(ys.lowMarginFrac):
                frozen["yLow"] = True
            if not frozen["yHigh"] and _inDeadband(ys.highMarginFrac):
                frozen["yHigh"] = True

            if all(frozen.values()):
                converged = True
                prOk(f"converged after {i + 1} iteration(s) - all four edges within the "
                    f"target margin")
                break

            effStatus = {
                "x": AxisClipStatus(xs.lowClipped and not frozen["xLow"],
                                    xs.highClipped and not frozen["xHigh"],
                                    xs.underscan and not (frozen["xLow"] or frozen["xHigh"]),
                                    xs.lowMarginFrac, xs.highMarginFrac),
                "y": AxisClipStatus(ys.lowClipped and not frozen["yLow"],
                                    ys.highClipped and not frozen["yHigh"],
                                    ys.underscan and not (frozen["yLow"] or frozen["yHigh"]),
                                    ys.lowMarginFrac, ys.highMarginFrac),
            }
            currentRange = autoTuneDacRange(currentRange, effStatus,
                                            step=cfg["dacRangeTuneStepUnits"])
            # dac_limit_min/max is the tighter, real hardware-safety clamp (OPA
            # clipping margin, docs/HARDWARE.md) - autoTuneDacRange only enforces the
            # raw int16 DAC range, so re-clamp against it here before the next iteration.
            currentRange = (max(limLo, currentRange[0]), min(limHi, currentRange[1]),
                            max(limLo, currentRange[2]), min(limHi, currentRange[3]))
        else:
            stillMoving = [k for k, v in frozen.items() if not v]
            prWarn(f"reached max-iterations ({maxIterations}) without full convergence - "
                  f"{stillMoving} still adjusting")
    finally:
        esp.stop()

    gainX, gainY, offsetX, offsetY = lastGainOffset
    prTable([
        ("galvo_x_gain", f"{baseline.gainX:.0f} -> {gainX:.0f}"),
        ("galvo_y_gain", f"{baseline.gainY:.0f} -> {gainY:.0f}"),
        ("galvo_x_offset", f"{baseline.offsetX:.0f} -> {offsetX:.0f}"),
        ("galvo_y_offset", f"{baseline.offsetY:.0f} -> {offsetY:.0f}"),
    ], headers=("field", "value"))

    if dryRun:
        prInfo("dry-run: leaving the original calibration live, nothing saved")
        restoreBaseline()
        return

    if not converged and not askYesNo(
            "did not fully converge - keep the last tuned values live anyway?", default=False):
        restoreBaseline()
        prInfo("reverted to the original calibration")
        return

    if askYesNo("save the tuned gain/offset to NVS (POST /api/calib-save)?", default=True):
        esp.calibSave()
        prOk("saved via /api/calib-save")
    else:
        prInfo("left live (not persisted) - values revert on the next ESP32 reboot")


# ── resonance measurement (Prompt 13) ─────────────────────────────────────────

def _resonanceSafeAmpDac(espCfg: dict, ampFraction: float) -> int:
    """Largest symmetric peak amplitude (DAC units) that keeps both +amp and -amp
    inside [dac_limit_min, dac_limit_max], scaled down by ampFraction. Firmware
    re-clamps this anyway (see EspClient.resonanceTest) - this is the "pick a
    conservative test amplitude" half, not the safety net itself."""
    limMin = int(espCfg.get("dac_limit_min", 0x0666))
    limMax = int(espCfg.get("dac_limit_max", 0xF999))
    safeMax = min(0x8000 - limMin, limMax - 0x8000)
    return max(1, int(round(safeMax * ampFraction)))


def _measureOneFrequency(cfg: dict, esp: EspClient, cam: "Camera", background: np.ndarray,
                         axis: int, freqHz: float, ampDac: int,
                         colorRgb: tuple[int, int, int]) -> float | None:
    """Commands one frequency, waits out the transient, captures enough frames to
    span resonanceMinCycles periods, and returns the streak extent (px) or None if
    nothing measurable was detected. Frame count is scaled up at low frequencies -
    the default accumFrames window (tuned for fast-scanned calib patterns) can be
    shorter than one period down at the sweep's low end."""
    r, g, b = colorRgb
    result = esp.resonanceTest(axis, freqHz, ampDac, r, g, b)
    if result.get("clamped"):
        prWarn(f"  {freqHz:.1f}Hz: amp {ampDac} clamped to {result.get('amp')} by "
              f"dac_limit_min/max")
    time.sleep(cfg["resonanceSettleSeconds"])

    fps = cfg.get("cameraFps") or 60.0
    periodS = 1.0 / freqHz
    framesForCycles = int(-(-(cfg["resonanceMinCycles"] * periodS * fps) // 1))  # ceil
    nFrames = max(cfg["accumFrames"], framesForCycles, 3)

    cam.statusText = f"measure-resonance: {freqHz:.1f}Hz axis={'Y' if axis else 'X'}"
    frame = cam.grabAccumulated(nFrames)
    diff = cv2.subtract(frame, background)
    extent = detectStreakExtent(diff, cfg["binaryThreshold"])
    if extent is None or extent < cfg["resonanceMinExtentPx"]:
        return None
    return extent


def _sweepFrequencies(cfg: dict, esp: EspClient, cam: "Camera", background: np.ndarray,
                      axis: int, ampDac: int, colorRgb: tuple[int, int, int],
                      freqs: list[float], label: str) -> list[float | None]:
    amps: list[float | None] = []
    for i, f in enumerate(freqs):
        pr(f"  [{label} {i + 1}/{len(freqs)}] {f:7.1f} Hz ...", end=" ")
        a = _measureOneFrequency(cfg, esp, cam, background, axis, f, ampDac, colorRgb)
        amps.append(a)
        pr(f"{a:.1f}px" if a is not None else "no response")
        waitWhilePaused(cam)
    return amps


def _bodePlotPng(freqs: list[float], amps: list[float | None], fRes: float | None,
                 f3dbLo: float | None, f3dbHi: float | None, axisLabel: str) -> np.ndarray:
    """Renders a log-frequency Bode-magnitude plot with plain cv2 drawing (no
    matplotlib dependency - consistent with the rest of this script's
    annotateCanvas()/annotateLiveGeometry() style camera-canvas rendering)."""
    W, H = 900, 500
    margin = {"l": 70, "r": 20, "t": 40, "b": 60}
    canvas = np.full((H, W, 3), 255, dtype=np.uint8)
    validPairs = [(f, a) for f, a in zip(freqs, amps) if a is not None]
    if not validPairs:
        cv2.putText(canvas, "no response detected at any swept frequency", (30, H // 2),
                   cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 0, 200), 2, cv2.LINE_AA)
        return canvas

    fMin, fMax = min(freqs), max(freqs)
    aMax = max(a for _, a in validPairs)
    plotW, plotH = W - margin["l"] - margin["r"], H - margin["t"] - margin["b"]

    def xOf(f: float) -> int:
        u = (np.log10(f) - np.log10(fMin)) / (np.log10(fMax) - np.log10(fMin) + 1e-9)
        return margin["l"] + int(u * plotW)

    def yOf(a: float) -> int:
        v = a / (aMax + 1e-9)
        return margin["t"] + plotH - int(v * plotH)

    cv2.rectangle(canvas, (margin["l"], margin["t"]), (W - margin["r"], H - margin["b"]),
                 (200, 200, 200), 1)
    for f in (fMin, fMax) if fMin == fMax else np.geomspace(fMin, fMax, 6):
        x = xOf(f)
        cv2.line(canvas, (x, margin["t"]), (x, H - margin["b"]), (235, 235, 235), 1)
        cv2.putText(canvas, f"{f:.0f}", (x - 15, H - margin["b"] + 18),
                   cv2.FONT_HERSHEY_SIMPLEX, 0.42, (60, 60, 60), 1, cv2.LINE_AA)

    pts = [(xOf(f), yOf(a)) for f, a in validPairs]
    for p0, p1 in zip(pts, pts[1:]):
        cv2.line(canvas, p0, p1, (200, 120, 0), 2, cv2.LINE_AA)
    for p in pts:
        cv2.circle(canvas, p, 3, (200, 120, 0), -1, cv2.LINE_AA)

    if fRes is not None:
        x = xOf(fRes)
        cv2.line(canvas, (x, margin["t"]), (x, H - margin["b"]), (0, 0, 220), 1, cv2.LINE_AA)
        cv2.putText(canvas, f"fRes={fRes:.1f}Hz", (x + 6, margin["t"] + 16),
                   cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 0, 220), 1, cv2.LINE_AA)
    if f3dbLo is not None and f3dbHi is not None:
        yThresh = yOf(aMax / np.sqrt(2))
        cv2.line(canvas, (margin["l"], yThresh), (W - margin["r"], yThresh),
                (0, 160, 0), 1, cv2.LINE_AA)
        cv2.putText(canvas, "-3dB", (margin["l"] + 4, yThresh - 4),
                   cv2.FONT_HERSHEY_SIMPLEX, 0.42, (0, 160, 0), 1, cv2.LINE_AA)

    cv2.putText(canvas, f"Galvo resonance sweep - axis {axisLabel}", (margin["l"], 24),
               cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 0, 0), 1, cv2.LINE_AA)
    cv2.putText(canvas, "frequency (Hz, log scale)", (W // 2 - 90, H - 12),
               cv2.FONT_HERSHEY_SIMPLEX, 0.45, (60, 60, 60), 1, cv2.LINE_AA)
    return canvas


def _halfPowerBandwidth(freqs: list[float], amps: list[float | None],
                        peakIdx: int) -> tuple[float | None, float | None]:
    """Linear-interpolated -3dB (1/sqrt(2) of peak) crossing frequencies on either
    side of the peak. None on a side that never crosses (e.g. the sweep range
    didn't extend far enough) - callers must handle a partial/missing bandwidth,
    not assume both sides are always found."""
    peakAmp = amps[peakIdx]
    if peakAmp is None:
        return None, None
    target = peakAmp / np.sqrt(2)

    def interp(iLo: int, iHi: int) -> float | None:
        aLo, aHi = amps[iLo], amps[iHi]
        if aLo is None or aHi is None:
            return None
        if (aLo - target) * (aHi - target) > 0:
            return None   # no crossing between these two samples
        t = (target - aLo) / (aHi - aLo) if aHi != aLo else 0.0
        # log-interpolate frequency (sweep is log-spaced)
        return float(np.exp(np.log(freqs[iLo]) + t * (np.log(freqs[iHi]) - np.log(freqs[iLo]))))

    fLo = None
    for i in range(peakIdx, 0, -1):
        fLo = interp(i - 1, i)
        if fLo is not None:
            break
    fHi = None
    for i in range(peakIdx, len(freqs) - 1):
        fHi = interp(i, i + 1)
        if fHi is not None:
            break
    return fLo, fHi


def runMeasureResonance(cfg: dict, esp: EspClient, cam: "Camera", axisName: str | None):
    """Prompt 13: sweeps one galvo axis 50-2000Hz (default, see resonanceMin/
    MaxFreqHz), reads the driven streak's spatial extent (NOT centroid - see
    detectStreakExtent()) as an amplitude proxy, and extracts fRes/Q from the
    resulting Bode-magnitude curve. Two-pass: a coarse log-spaced sweep across
    the full range locates an approximate peak, then a fine linear-spaced pass
    around it gets an accurate -3dB bandwidth. See docs/feature-prompts/
    DECISIONS.md, Prompt 13 for the full design rationale/known limitations."""
    # fw_version lives in /api/status (getStatus()), NOT /api/config - buildConfigJson()
    # doesn't publish it at all, unlike buildStateJson()/api/status's own sprintf (see
    # runCheckConnection() for the same read pattern).
    status = esp.getStatus()
    fwVersion = parseFwVersion(status.get("fw_version", ""))
    if fwVersion is not None and fwVersion < MIN_FW_VERSION_RESONANCE:
        minStr = ".".join(map(str, MIN_FW_VERSION_RESONANCE))
        raise OptimizerError(
            f"connected firmware is older than v{minStr} - /api/debug/resonance "
            f"doesn't exist yet on this controller (Prompt 13). Update firmware first."
        )
    espCfg = esp.getConfig()

    axis = 1 if (axisName or cfg["resonanceAxis"]).lower() == "y" else 0
    axisLabel = "Y" if axis else "X"
    ampDac = _resonanceSafeAmpDac(espCfg, cfg["resonanceAmpFraction"])
    colorRgb = _channelToRgb(cfg["resonanceChannel"])

    esp.stop()             # in case a calib-cam session was left running
    esp.resonanceOff()     # in case a previous run was interrupted mid-sweep
    try:
        RESULTS_DIR.mkdir(exist_ok=True)
    except OSError as e:
        raise OptimizerError(f"cannot create {RESULTS_DIR.name}/: {e}") from e

    pr(f"resonance sweep: axis={axisLabel} amp={ampDac} DAC units "
      f"({cfg['resonanceAmpFraction'] * 100:.0f}% of safe range)")
    background = cam.grabBackground()

    try:
        coarseFreqs = list(np.geomspace(cfg["resonanceMinFreqHz"], cfg["resonanceMaxFreqHz"],
                                        cfg["resonanceCoarsePoints"]))
        pr(f"coarse pass: {len(coarseFreqs)} points, "
          f"{cfg['resonanceMinFreqHz']:.0f}-{cfg['resonanceMaxFreqHz']:.0f}Hz (log-spaced)")
        coarseAmps = _sweepFrequencies(cfg, esp, cam, background, axis, ampDac, colorRgb,
                                       coarseFreqs, "coarse")

        validCoarse = [(f, a) for f, a in zip(coarseFreqs, coarseAmps) if a is not None]
        if not validCoarse:
            raise OptimizerError(
                "no response detected at any coarse-sweep frequency - check the laser is "
                "visible to the camera (aim/focus), raise resonanceAmpFraction, or widen "
                "resonanceMinFreqHz/resonanceMaxFreqHz in the config"
            )
        coarsePeakFreq = max(validCoarse, key=lambda p: p[1])[0]
        pr(f"coarse peak: ~{coarsePeakFreq:.1f}Hz")

        # Fine pass: linear-spaced around the coarse peak, for an accurate -3dB
        # bandwidth. If the crossing doesn't land on both sides (peak too sharp/
        # narrow for the span, or noisy near the edges), widen the span and
        # re-sweep instead of giving up on Q/ring_damping_ratio outright - bounded
        # by resonanceFineSpanRetries so a genuinely flat/noisy curve still stops.
        span = cfg["resonanceFineSpanFraction"]
        maxRetries = cfg["resonanceFineSpanRetries"]
        attempt = 0
        while True:
            fineLo = max(cfg["resonanceMinFreqHz"], coarsePeakFreq * (1 - span))
            fineHi = min(cfg["resonanceMaxFreqHz"], coarsePeakFreq * (1 + span))
            pr(f"fine pass (attempt {attempt + 1}/{maxRetries + 1}): "
              f"{cfg['resonanceFinePoints']} points, {fineLo:.1f}-{fineHi:.1f}Hz "
              f"(linear-spaced, +-{span * 100:.0f}%)")
            fineFreqs = list(np.linspace(fineLo, fineHi, cfg["resonanceFinePoints"]))
            fineAmps = _sweepFrequencies(cfg, esp, cam, background, axis, ampDac, colorRgb,
                                         fineFreqs, "fine")

            allFreqs = coarseFreqs + fineFreqs
            allAmps = coarseAmps + fineAmps
            order = sorted(range(len(allFreqs)), key=lambda i: allFreqs[i])
            freqs = [allFreqs[i] for i in order]
            amps = [allAmps[i] for i in order]

            validAll = [(i, a) for i, a in enumerate(amps) if a is not None]
            if not validAll:
                raise OptimizerError("no response detected in either sweep pass")
            peakIdx = max(validAll, key=lambda p: p[1])[0]
            fPeak = freqs[peakIdx]
            peakAmp = amps[peakIdx]

            f3dbLo, f3dbHi = _halfPowerBandwidth(freqs, amps, peakIdx)
            if f3dbLo is not None and f3dbHi is not None:
                break
            spanExhausted = fineLo <= cfg["resonanceMinFreqHz"] and fineHi >= cfg["resonanceMaxFreqHz"]
            if attempt >= maxRetries or spanExhausted:
                break
            attempt += 1
            span = min(span * 2.0, 1.0) if span < 1.0 else span * 2.0
            prWarn(f"-3dB crossing not resolved on both sides - retrying with a wider "
                  f"fine-pass span (+-{span * 100:.0f}%)")

        result: dict = {"fPeakHz": fPeak, "peakAmplitudePx": peakAmp, "axis": axisLabel}
        if f3dbLo is not None and f3dbHi is not None:
            bandwidth = f3dbHi - f3dbLo
            q = fPeak / bandwidth if bandwidth > 0 else None
            if q is not None and q > 0:
                zeta = 1.0 / (2.0 * q)
                # f_peak = fn*sqrt(1-2*zeta^2) for a driven 2nd-order system - correct
                # back to the UNDAMPED natural frequency the firmware's ZV shaper
                # actually wants (ring_freq_hz = wn/2pi, point_optimizer.cpp
                # computeZvShaper()), not the driven-response peak itself. Small at
                # light damping (~2% at zeta=0.15) but not zero.
                underRoot = 1.0 - 2.0 * zeta * zeta
                fn = fPeak / np.sqrt(underRoot) if underRoot > 0 else fPeak
                result.update({"f3dbLoHz": f3dbLo, "f3dbHiHz": f3dbHi, "bandwidthHz": bandwidth,
                              "Q": q, "ringDampingRatio": zeta, "ringFreqHz": float(fn)})
        else:
            prWarn(f"could not resolve a full -3dB bandwidth on both sides of the peak after "
                  f"{attempt + 1} fine-pass attempt(s) (final span +-{span * 100:.0f}%) - "
                  f"Q/ring_damping_ratio unavailable this run. Raise resonanceFineSpanRetries "
                  f"or check the coarse-sweep plot for a ragged/noisy curve near the peak.")

        ts = time.strftime("%Y%m%d_%H%M%S")
        csvPath = RESULTS_DIR / f"resonance_{axisLabel}_{ts}.csv"
        with open(csvPath, "w", newline="") as fh:
            w = csv.writer(fh)
            w.writerow(["freq_hz", "amplitude_px"])
            for f, a in zip(freqs, amps):
                w.writerow([f, "" if a is None else a])
        pngPath = RESULTS_DIR / f"resonance_{axisLabel}_{ts}.png"
        plot = _bodePlotPng(freqs, amps, fPeak, f3dbLo, f3dbHi, axisLabel)
        cv2.imwrite(str(pngPath), plot)
        if cam.liveView:
            cam.liveView.update(cv2.cvtColor(plot, cv2.COLOR_BGR2GRAY), "resonance sweep result")

        pr()
        prOk(f"fRes (driven peak) = {fPeak:.1f}Hz, amplitude = {peakAmp:.1f}px")
        if "Q" in result:
            prOk(f"Q = {result['Q']:.2f}  ->  ring_damping_ratio = {result['ringDampingRatio']:.3f}"
                f"  ->  ring_freq_hz (corrected) = {result['ringFreqHz']:.1f}Hz")
            prTip(f"apply via: POST /api/optimizer-live "
                 f"{{ring_freq_hz: {result['ringFreqHz']:.1f}, "
                 f"ring_damping_ratio: {result['ringDampingRatio']:.3f}, "
                 f"ringing_comp_enabled: true, profile: <N>}}, then /api/optimizer-save "
                 f"to persist - NOT auto-applied by this command.")
        pr(f"saved: {csvPath.name}, {pngPath.name}")
        return result
    finally:
        esp.resonanceOff()


# ── ideal pattern geometry (must match firmware calib patterns, DAC coords) ──

def idealPolylines(pattern: str, r: int) -> tuple[list[np.ndarray], list[np.ndarray]]:
    """Returns (litPolylines, blankGapSegments). Coordinates in DAC space."""
    h = r // 2
    if pattern == "square":
        lit = [np.array([[-h, -h], [h, -h], [h, h], [-h, h], [-h, -h]], float)]
        return lit, []
    if pattern == "star":
        angles = np.arange(5) * (4 * np.pi / 5) - np.pi / 2
        pts = np.stack([h * np.cos(angles), h * np.sin(angles)], axis=1)
        return [np.vstack([pts, pts[:1]])], []
    if pattern == "segments":
        lit, gaps = [], []
        xs = np.linspace(-h, h, 4)
        for i, x in enumerate(xs):
            lit.append(np.array([[x, -h], [x, h]], float))
            if i < 3:
                gaps.append(np.array([[x, h], [xs[i + 1], -h]], float))
        return lit, gaps
    if pattern == "circle":
        t = np.linspace(0, 2 * np.pi, 128)
        return [np.stack([h * np.cos(t), h * np.sin(t)], axis=1)], []
    if pattern == "spiral":
        t = np.linspace(0, 6 * np.pi, 512)
        rad = np.linspace(h * 0.15, h, 512)
        return [np.stack([rad * np.cos(t), rad * np.sin(t)], axis=1)], []
    if pattern == "wireframe":
        return _wireframeIdeal(h)
    if pattern == "text":
        return _textIdeal(h)
    if pattern == "particles":
        return _particlesIdeal(h)
    raise ValueError(f"unknown pattern {pattern}")


# ── mirrors of calib_patterns.cpp's second camera block (21-23) ──────────────
#
# These three exist because the firmware geometry is no longer a one-liner:
# a projected cube, a stroke-font string and a scattered dot set each need
# real (small) code to reproduce. Every constant below is copied from
# calib_patterns.cpp - if that file changes, these must change with it.

# calib_patterns.cpp cam_wireframe(): first two rows of a yaw-35/pitch-25
# rotation matrix, as literals. Deliberately not recomputed from np.sin/np.cos
# here - the firmware cannot use trig and stay bit-comparable, so the literals
# ARE the contract.
_WF_PX_X, _WF_PX_Z = 0.81915, 0.57358
_WF_PY_X, _WF_PY_Y, _WF_PY_Z = 0.24238, 0.90631, 0.34614
_WF_CHAINS = ((1, 0, 2, 6), (2, 3, 1, 5), (0, 4, 5, 7), (4, 6, 7, 3))


def _wireframeIdeal(h: int) -> tuple[list[np.ndarray], list[np.ndarray]]:
    s = h * 0.63                     # CAM_WF_HALF, relative to CAM_H == h
    verts = []
    for i in range(8):
        x = s if (i & 1) else -s     # bit0 = x, bit1 = y, bit2 = z
        y = s if (i & 2) else -s
        z = s if (i & 4) else -s
        verts.append((_WF_PX_X * x + _WF_PX_Z * z,
                      _WF_PY_X * x + _WF_PY_Y * y - _WF_PY_Z * z))
    lit = [np.array([verts[v] for v in chain], float) for chain in _WF_CHAINS]
    # No blank-gap geometry on purpose: gapMask/blankLeakage reads BACKWARDS on
    # the defect it exists to catch (see Metrics.blankCorridorLeakage), so the
    # chain-to-chain leakage on this pattern is scored by the corridor metric,
    # which needs no explicit gap geometry.
    return lit, []


# calib_patterns.cpp cam_text(): CAM_TEXT_STRING / CAM_TEXT_SCALE, plus the
# glyphs that string actually uses, transcribed from text_renderer.cpp's FONT_*
# tables (font space: x in [-5,5], y in [-7,7], +y = up, which is also DAC +y
# = up - glyphOutlinePaths() applies no flip). Only the glyphs in
# CAM_TEXT_STRING are mirrored, on purpose: a full font copy here would be a
# second source of truth nobody would keep in sync.
_TEXT_STRING = "GAL"
_TEXT_SCALE = 950.0
_TEXT_GLYPHS = {
    # char: (advance, [subpath as [(x, y), ...], ...])
    "G": (10, [[(4, 5), (2, 7), (-2, 7), (-4, 5), (-4, -5),
                (-2, -7), (2, -7), (4, -5), (4, 0), (1, 0)]]),
    "A": (10, [[(-4, -7), (0, 7), (4, -7)], [(-3, -2), (3, -2)]]),
    "L": (9,  [[(-4, 7), (-4, -7)], [(-4, -7), (4, -7)]]),
}


def _textIdeal(h: int) -> tuple[list[np.ndarray], list[np.ndarray]]:
    del h                            # fixed DAC-space scale, not h-relative
    scale = _TEXT_SCALE
    total = sum(_TEXT_GLYPHS[c][0] for c in _TEXT_STRING)
    cx = -total * scale / 2.0        # glyphOutlinePaths(): string centered on 0
    lit: list[np.ndarray] = []
    for c in _TEXT_STRING:
        advance, subpaths = _TEXT_GLYPHS[c]
        gx = cx + advance * 0.5 * scale          # glyph centered in its cell
        for sub in subpaths:
            lit.append(np.array([(gx + sx * scale, sy * scale) for sx, sy in sub],
                                float))
        cx += advance * scale
    return lit, []                   # see _wireframeIdeal() on why no gaps


# calib_patterns.cpp cam_particles(): 4x3 grid, visit order chosen to grade the
# jump distances short/medium/long. Order matters here only for the blank-gap
# geometry - the blob metrics themselves are order-independent.
_PARTICLE_ORDER = ((0, 0), (1, 0), (1, 1), (0, 1), (0, 2), (2, 0),
                   (3, 2), (2, 1), (3, 0), (1, 2), (3, 1), (2, 2))


def particleDots(h: int) -> np.ndarray:
    """The 12 ideal dot centres, DAC space, in firmware visit order."""
    colStep = h * (2.0 / 3.0)
    return np.array([(-h + col * colStep, -h + row * h)
                     for col, row in _PARTICLE_ORDER], float)


def _particlesIdeal(h: int) -> tuple[list[np.ndarray], list[np.ndarray]]:
    dots = particleDots(h)
    # A dot is a degenerate 2-point polyline; rasterizePolylines draws it as a
    # single blob at IDEAL_RASTER_THICKNESS["particles"] px across.
    lit = [np.array([p, p], float) for p in dots]
    return lit, []                   # see _wireframeIdeal() on why no gaps


# Ideal-geometry line thickness in DAC-canvas pixels, per pattern. Everything not
# listed stays at 1 px, so every pre-existing pattern's idealMask/distToIdeal is
# byte-identical to before this table existed. 'particles' is the exception it was
# added for: its ideal geometry is 12 ZERO-LENGTH polylines, which at 1 px rasterise
# to 12 single pixels - below computeMetrics' "> 10 lit pixels on the ideal path"
# bar, so every particles measurement would be flagged invalid on principle before
# any beam behaviour was even considered. 5 px is about this rig's own imaged beam
# width (beamWidthUnits ~300 units ~4 px), i.e. the mask covers the dot the camera
# actually sees rather than an idealised mathematical point.
IDEAL_RASTER_THICKNESS = {"particles": 5}


def idealThickness(pattern: str) -> int:
    return IDEAL_RASTER_THICKNESS.get(pattern, 1)


def rasterizePolylines(polylines: list[np.ndarray], canvasSize: int, r: int,
                       thickness: int = 1) -> np.ndarray:
    scale = canvasSize / (2 * r)
    canvas = np.zeros((canvasSize, canvasSize), np.uint8)
    for line in polylines:
        pts = ((line + r) * scale).astype(np.int32)
        cv2.polylines(canvas, [pts], False, 255, thickness)
    return canvas


# ── orientation auto-correction ──────────────────────────────────────────────
#
# A firmware coordinate-convention bug (axis swap/negation) is always one of the
# 8 signed-permutation transforms below - never an arbitrary angle - so that's the
# whole, correctly-scoped search space; brute-forcing continuous rotation would
# both be slower and model the wrong kind of bug. This does NOT fix any such bug
# in firmware - it only re-orients idealPolylines()'s reference so this tool's
# geometry metrics (scaleError/offset) aren't corrupted by it while it's outstanding.
# See ORIENTATION_FILE's own warning for what this can and can't tell you.
D4_TRANSFORMS: dict[str, tuple[int, int, int, int]] = {
    # (a, b, c, d): (x, y) -> (a*x + b*y, c*x + d*y)
    "identity":       (1, 0, 0, 1),
    "rot90":          (0, -1, 1, 0),
    "rot180":         (-1, 0, 0, -1),
    "rot270":         (0, 1, -1, 0),
    "mirror_x":       (-1, 0, 0, 1),
    "mirror_y":       (1, 0, 0, -1),
    "transpose":      (0, 1, 1, 0),
    "anti_transpose": (0, -1, -1, 0),
}


def _applyD4(points: np.ndarray, name: str) -> np.ndarray:
    a, b, c, d = D4_TRANSFORMS[name]
    x, y = points[:, 0], points[:, 1]
    return np.stack([a * x + b * y, c * x + d * y], axis=1)


_orientationCache: dict[str, dict] = {}
_orientationCacheLoaded = False


def _loadOrientationCache() -> dict[str, dict]:
    global _orientationCacheLoaded
    if not _orientationCacheLoaded:
        if ORIENTATION_FILE.exists():
            try:
                _orientationCache.update(json.loads(ORIENTATION_FILE.read_text()))
            except (json.JSONDecodeError, OSError) as e:
                prWarn(f"could not read {ORIENTATION_FILE.name} ({e}) - "
                      f"re-detecting orientation for every pattern")
        _orientationCacheLoaded = True
    return _orientationCache


def _saveOrientationCache():
    try:
        ORIENTATION_FILE.write_text(json.dumps(_orientationCache, indent=2))
    except OSError as e:
        prWarn(f"could not save {ORIENTATION_FILE.name}: {e}")


def resetOrientationCache():
    """Called by 'calibrate' - a fresh homography is a natural point to also
    re-verify the orientation assumption rather than trust a possibly-stale one."""
    global _orientationCacheLoaded
    _orientationCache.clear()
    _orientationCacheLoaded = True
    if ORIENTATION_FILE.exists():
        try:
            ORIENTATION_FILE.unlink()
        except OSError as e:
            prWarn(f"could not remove stale {ORIENTATION_FILE.name}: {e}")


def detectOrientation(pattern: str, r: int, trace: np.ndarray) -> str:
    """Picks whichever of the 8 D4_TRANSFORMS makes idealPolylines(pattern) best match
    an already-captured trace, caches the result (in-memory + ORIENTATION_FILE) so it's
    detected once and reused, and prints a loud, un-missable warning the first time a
    non-identity transform is chosen - this masks a real firmware orientation quirk from
    THIS tool's metrics, it does not fix it, and a genuinely mis-projected shape could
    still look 'OK' here. If this pattern shares its point-generation path with a real
    preset (e.g. asymmetric content like text), that preset could still be rotated/
    mirrored live - this only corrects what gets measured, never what gets projected."""
    cache = _loadOrientationCache()
    if pattern in cache:
        return cache[pattern]["name"]

    tracePixels = trace > 0
    if not tracePixels.any():
        return "identity"  # nothing to compare against - don't guess, caller's own
                            # "nothing visible" worst-case handling still applies

    scores = {}
    for name in D4_TRANSFORMS:
        _, _, idealMask, distToIdeal, *_ = _idealGeometryFor(pattern, r, name)
        scores[name] = float(np.mean(distToIdeal[tracePixels]))
    best = min(scores, key=scores.get)

    cache[pattern] = {"name": best, "score": scores[best],
                      "identityScore": scores["identity"],
                      "detected": time.strftime("%Y-%m-%d_%H-%M-%S")}
    _saveOrientationCache()
    if best != "identity":
        pr(f"\n{'!' * 70}")
        prWarn(f"ORIENTATION MISMATCH on pattern '{pattern}': idealPolylines() only fits "
             f"the measured trace after applying '{best}' (fit {scores[best]:.1f} DAC-unit "
             f"avg deviation vs. {scores['identity']:.1f} unrotated).")
        prWarn(f"This means the firmware's actual output for '{pattern}' is rotated/mirrored "
             f"relative to this tool's reference - a real coordinate-convention issue, not "
             f"a camera/calibration problem. From here on, THIS TOOL compensates for it when "
             f"scoring '{pattern}' (saved to {ORIENTATION_FILE.name}) so geometry metrics "
             f"stay meaningful - but that only fixes the MEASUREMENT. If this pattern's "
             f"point-generation path is shared by a real preset, that preset could still be "
             f"projected rotated/mirrored live; this does not check or fix that.")
        pr(f"{'!' * 70}\n")
    return best


# ── metrics ──────────────────────────────────────────────────────────────────

CANVAS = 800   # DAC space rasterized to CANVAS x CANVAS px


@dataclass
class Metrics:
    pathDeviationRms: float
    blankLeakage: float
    cornerHotspot: float
    brightnessNonUniformity: float
    # ── validity ─────────────────────────────────────────────────────────────
    # A measurement that saw (almost) nothing still produces perfectly ordinary-looking
    # floats for every field above - computeMetrics' "nothing visible" branches used to
    # hand back pathDeviationRms=2*dacRange, brightnessNonUniformity=1.0 and
    # offsetX/YUnits=dacRange as if they were readings. An Optuna objective happily
    # optimises against those. `valid` makes that impossible: an invalid Metrics carries
    # cost=NaN, callers must refuse to score it, and `invalidReasons` says which
    # criterion failed so the operator knows where to look (threshold/exposure/focus).
    valid: bool = True
    invalidReasons: list[str] = field(default_factory=list)
    # Supporting evidence for `valid`, and useful on their own:
    traceLitPx: int = 0          # lit pixels on the whole 800x800 DAC canvas
    offPathLitPx: int = 0        # of those, how many are further than offPathGuardPx
                                 # from any ideal segment (stray light / noise)
    pathCoveragePct: float = 0.0  # % of the rasterised ideal path with a lit pixel
                                 # within pathCoverageRadiusPx - the "is the camera
                                 # actually seeing the shape" signal
    beamWidthUnits: float = 0.0  # 2 x median distance of lit pixels from the ideal
                                 # path, DAC units. This is the rig's own measurement
                                 # floor for pathDeviationRms - see diagnoseThresholds'
                                 # pathDeviationBeamWidthFactor.
    # Mean brightness of every region that SHOULD be dark (inside the ideal shape's
    # bounding box, further than offPathGuardPx from any ideal segment) - not just the
    # ideal blank-jump corridor blankLeakage samples. blankLeakage is anti-correlated
    # with the defect it exists to catch: a beam that streaks anywhere OTHER than along
    # the ideal jump path reads LOWER there, not higher. Measured on this rig, cam_
    # segments at blank_samples=1 (visibly streaking) scored blankLeakage 0.95 vs 2.95
    # for the clean blank_samples=40 case - exactly backwards.
    blankCorridorLeakage: float = 0.0
    blankCorridorMaxVal: float = 0.0   # brightest single pixel in that corridor
    blankCorridorLitPx: int = 0        # corridor pixels above binaryThreshold
    # Fraction of traced pixels at raw sensor saturation (dacImage >= 250) - a global-
    # shutter CMOS sensor bleeds charge into neighboring pixels once a spot is bright
    # enough (blooming), which can inflate pathDeviationRms (halo spreads past the
    # ideal path) and cornerHotspot (ROI reads "hot" from clipping, not real dwell
    # brightness) with a camera artifact rather than an actual scan/dwell problem.
    # Flagged here (and painted magenta in annotateCanvas) instead of silently baked
    # into those metrics, since there's no reliable way to subtract the halo back out.
    saturationFrac: float = 0.0
    # ── isolated-dot metrics (pattern 'particles' only; 0 / blobExpected 0 elsewhere) ──
    # The Particles profile's failure mode is not path deviation - there is no path -
    # it is a blank window too short for the jump distance, which leaves the beam still
    # in flight when the laser re-arms and smears an isolated dot into a comet. That is
    # exactly the v6.65.1 Starfield regression, and none of the metrics above sees it:
    # the streak lands ON the way to a real dot, so pathDeviationRms barely moves and
    # path coverage stays high. These three do see it.
    blobExpected: int = 0            # ideal dot count (0 = not a dot pattern -> ignore below)
    blobCount: int = 0               # detected blobs above minBlobAreaPx
    blobCountError: int = 0          # abs(blobCount - blobExpected): merged or split dots
    blobElongation: float = 1.0      # mean major/minor axis ratio over detected blobs;
                                     # 1.0 = perfectly round, higher = smeared
    blobElongationMax: float = 1.0   # worst single blob - a short blank window streaks
                                     # only the LONG jumps, so the mean understates it
    blobCentroidErrorUnits: float = 0.0  # mean DAC-unit distance from each ideal dot to
                                     # the nearest detected blob centroid
    # Overall shape size/position vs. ideal (DAC-space bounding box, 1st-99th percentile
    # to shrug off stray noise pixels) - NOT part of `cost` on purpose: these reflect
    # galvo gain/offset calibration, which no scan/dwell parameter can fix, so letting
    # Optuna chase them would just waste trials. Used by 'diagnose' to tell a geometry
    # problem apart from an optimizer-settings one.
    scaleErrorXPct: float = 0.0
    scaleErrorYPct: float = 0.0
    offsetXUnits: float = 0.0
    offsetYUnits: float = 0.0
    cost: float = field(default=0.0)


def warpToDacCanvas(image: np.ndarray, homography: np.ndarray, r: int) -> np.ndarray:
    scale = CANVAS / (2 * r)
    toCanvas = np.array([[scale, 0, r * scale],
                         [0, scale, r * scale],
                         [0, 0, 1]])
    return cv2.warpPerspective(image, toCanvas @ homography, (CANVAS, CANVAS))


@functools.lru_cache(maxsize=None)
def _idealGeometryFor(pattern: str, r: int, orientation: str = "identity"):
    """idealPolylines/rasterizePolylines/distanceTransform depend only on (pattern, r,
    orientation) - all constant for an entire optimize/measure/diagnose run - so cache
    them instead of recomputing on every trial/pattern measurement. `orientation` is one
    of D4_TRANSFORMS, applied to the raw ideal geometry before rasterizing - see
    detectOrientation() for why/when it's non-identity."""
    lit, gaps = idealPolylines(pattern, r)
    if orientation != "identity":
        lit = [_applyD4(line, orientation) for line in lit]
        gaps = [_applyD4(line, orientation) for line in gaps]
    idealMask = rasterizePolylines(lit, CANVAS, r, idealThickness(pattern))
    distToIdeal = cv2.distanceTransform(cv2.bitwise_not(idealMask), cv2.DIST_L2, 5)
    gapMask = rasterizePolylines(gaps, CANVAS, r, thickness=9) if gaps else None
    idealPts = np.vstack(lit)
    idealExtentX = float(idealPts[:, 0].max() - idealPts[:, 0].min())
    idealExtentY = float(idealPts[:, 1].max() - idealPts[:, 1].min())
    idealCenterX = float((idealPts[:, 0].max() + idealPts[:, 0].min()) / 2.0)
    idealCenterY = float((idealPts[:, 1].max() + idealPts[:, 1].min()) / 2.0)
    return (lit, gaps, idealMask, distToIdeal, gapMask,
            idealExtentX, idealExtentY, idealCenterX, idealCenterY)


@functools.lru_cache(maxsize=None)
def _darkCorridorMaskFor(pattern: str, r: int, orientation: str, guardPx: int) -> np.ndarray:
    """The "should be dark" mask: everything inside the ideal shape's own bounding box
    (inset by guardPx so the mask never hugs the shape's outer edge, where warp residual
    and beam width legitimately spill) that is further than guardPx from ANY ideal
    segment.

    Derived automatically from the pattern's own ideal polylines, so it generalises past
    the one pattern that has explicit blank-jump geometry:
      segments -> the three lanes between the four vertical lines (this reproduces the
                  hand-measured corridors the streak investigation used)
      square/star/circle -> the shape's interior
      spiral   -> the gaps between successive turns
    A pattern whose ideal path fills its own bounding box would produce an empty mask;
    callers treat an empty mask as "no corridor to measure" (leakage 0), not as clean.
    """
    lit, _ = idealPolylines(pattern, r)
    if orientation != "identity":
        lit = [_applyD4(line, orientation) for line in lit]
    idealMask = rasterizePolylines(lit, CANVAS, r, idealThickness(pattern))
    distToIdeal = cv2.distanceTransform(cv2.bitwise_not(idealMask), cv2.DIST_L2, 5)
    scale = CANVAS / (2 * r)
    px = (np.vstack(lit) + r) * scale
    x0, y0 = np.floor(px.min(axis=0)).astype(int)
    x1, y1 = np.ceil(px.max(axis=0)).astype(int)
    box = np.zeros(distToIdeal.shape, bool)
    ya, yb = max(int(y0) + guardPx, 0), min(int(y1) - guardPx, CANVAS)
    xa, xb = max(int(x0) + guardPx, 0), min(int(x1) - guardPx, CANVAS)
    if yb > ya and xb > xa:
        box[ya:yb, xa:xb] = True
    return box & (distToIdeal > guardPx)


# Patterns whose ideal geometry has real corners with a real dwell, i.e. where the
# corner/edge brightness ratio means something. Deliberately NOT circle/spiral
# (no corners at all) or segments/particles (no shared vertices to dwell at), which
# keep cornerHotspot at 0.0 exactly as before.
CORNER_HOTSPOT_PATTERNS = ("square", "star", "wireframe", "text")

# Patterns scored as isolated dots rather than as a path.
BLOB_PATTERNS = ("particles",)

def cornerAndEdgeRois(lit: list[np.ndarray]) -> tuple[np.ndarray, np.ndarray]:
    """Corner ROI centres (vertices) and edge ROI centres (edge midpoints) for an
    arbitrary set of ideal polylines.

    Reproduces the original square/star behaviour exactly: both are a single CLOSED
    polyline whose last point repeats the first, so the duplicate is dropped and the
    rolled midpoints pick up the closing edge - identical arrays to the previous
    inline `lit[0][:-1]` / `np.roll` code. Open polylines (wireframe chains, text
    strokes) contribute every vertex including their two endpoints: on an open path
    those endpoints are exactly where the optimizer emits a full stop-dwell
    (cornerSeverity() returns 1.0 there, see point_optimizer.cpp), so they are corner
    ROIs in the sense this metric means, not exceptions to it."""
    corners, mids = [], []
    for line in lit:
        if len(line) < 2:
            continue                       # degenerate (particles) - no corner to score
        closed = bool(np.allclose(line[0], line[-1]))
        verts = line[:-1] if closed else line
        if len(verts) < 2:
            continue
        corners.append(np.asarray(verts, float))
        if closed:
            mids.append((verts + np.roll(verts, -1, axis=0)) / 2.0)
        else:
            mids.append((verts[:-1] + verts[1:]) / 2.0)
    if not corners:
        return np.empty((0, 2)), np.empty((0, 2))
    return np.vstack(corners), np.vstack(mids)


def computeMetrics(capture: np.ndarray, background: np.ndarray, homography: np.ndarray,
                   pattern: str, cfg: dict) -> tuple[Metrics, dict]:
    r = cfg["dacRange"]
    diff = cv2.subtract(capture, background)
    dacImage = warpToDacCanvas(diff, homography, r)
    _, trace = cv2.threshold(dacImage, cfg["binaryThreshold"], 255, cv2.THRESH_BINARY)

    orientation = detectOrientation(pattern, r, trace)
    (lit, gaps, idealMask, distToIdeal, gapMask,
     idealExtentX, idealExtentY, idealCenterX, idealCenterY) = _idealGeometryFor(pattern, r, orientation)

    # 0. validity: is this a measurement at all, or a blind capture? Every "nothing
    # visible" branch below records a reason instead of quietly returning a worst-case
    # constant that reads like a real number downstream.
    invalidReasons: list[str] = []
    guardPx = int(cfg["offPathGuardPx"])
    coverR = int(cfg["pathCoverageRadiusPx"])

    # 1. path deviation: distance of every lit pixel from ideal path (DAC units RMS)
    tracePixels = trace > 0
    dacPerPixel = (2 * r) / CANVAS
    traceLitPx = int(tracePixels.sum())
    if traceLitPx:
        distLit = distToIdeal[tracePixels]
        pathDeviationRms = float(np.sqrt(np.mean(distLit ** 2)) * dacPerPixel)
        saturationFrac = float(np.mean(dacImage[tracePixels] >= 250))
        offPathLitPx = int((distLit > guardPx).sum())
        beamWidthUnits = float(2.0 * np.median(distLit) * dacPerPixel)
    else:
        pathDeviationRms = float(2 * r)    # nothing visible -> worst case
        saturationFrac = 0.0
        offPathLitPx = 0
        beamWidthUnits = 0.0
        invalidReasons.append("no lit pixels above binaryThreshold")

    # 1b. path coverage: how much of the ideal path was actually seen. This is the
    # metric that catches a blind capture the other ones can't - a too-high threshold
    # leaves only the brightest core pixels, which makes pathDeviationRms and
    # brightnessNonUniformity look BETTER while the shape has effectively vanished.
    coverKernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * coverR + 1,) * 2)
    grownTrace = cv2.dilate(trace, coverKernel)
    idealPathPx = idealMask > 0
    pathCoveragePct = (float(np.mean(grownTrace[idealPathPx] > 0)) * 100.0
                       if idealPathPx.any() else 0.0)
    if pathCoveragePct < cfg["minPathCoveragePct"]:
        invalidReasons.append(
            f"only {pathCoveragePct:.1f}% of the ideal path is visible "
            f"(need >= {cfg['minPathCoveragePct']:.0f}%)")

    # 2. blank leakage: mean brightness inside gap corridors (should be dark)
    if gapMask is not None:
        blankLeakage = float(np.mean(dacImage[gapMask > 0]))
    else:
        blankLeakage = 0.0

    # 2b. blank CORRIDOR leakage: same idea, but over every region that should be dark
    # rather than only the ideal jump path. See Metrics.blankCorridorLeakage for why the
    # ideal-path-only version reads backwards on the exact failure it exists to catch.
    darkMask = _darkCorridorMaskFor(pattern, r, orientation, guardPx)
    if darkMask.any():
        darkVals = dacImage[darkMask]
        blankCorridorLeakage = float(darkVals.mean())
        blankCorridorMaxVal = float(darkVals.max())
        blankCorridorLitPx = int((darkVals > cfg["binaryThreshold"]).sum())
    else:
        blankCorridorLeakage = blankCorridorMaxVal = 0.0
        blankCorridorLitPx = 0

    # 3. corner hotspot: corner ROI brightness / edge ROI brightness (dwell tuning)
    cornerHotspot = 0.0
    if pattern in CORNER_HOTSPOT_PATTERNS:
        vertices, edgeMids = cornerAndEdgeRois(lit)
        if len(vertices) and len(edgeMids):
            scale = CANVAS / (2 * r)
            roiHalf = 12
            cornerVals, edgeVals = [], []
            for group, sink in ((vertices, cornerVals), (edgeMids, edgeVals)):
                for vx, vy in group:
                    cx, cy = int((vx + r) * scale), int((vy + r) * scale)
                    roi = dacImage[max(cy - roiHalf, 0):cy + roiHalf, max(cx - roiHalf, 0):cx + roiHalf]
                    sink.append(float(roi.mean()))
            edgeMean = max(np.mean(edgeVals), 1.0)
            cornerHotspot = float(abs(np.mean(cornerVals) / edgeMean - 1.0))

    # 3b. isolated-dot blobs (particles only) - see Metrics.blobExpected.
    blobExpected = blobCount = blobCountError = 0
    blobElongation = blobElongationMax = 1.0
    blobCentroidErrorUnits = 0.0
    if pattern in BLOB_PATTERNS:
        idealDots = particleDots(r // 2)
        if orientation != "identity":
            idealDots = _applyD4(idealDots, orientation)
        blobExpected = len(idealDots)
        minArea = int(cfg.get("minBlobAreaPx", DEFAULT_MIN_BLOB_AREA_PX))
        nLabels, labels, stats, centroids = cv2.connectedComponentsWithStats(trace, 8)
        keep = [i for i in range(1, nLabels)
                if stats[i, cv2.CC_STAT_AREA] >= minArea]
        blobCount = len(keep)
        blobCountError = abs(blobCount - blobExpected)
        if keep:
            elongations = []
            for i in keep:
                ys, xs = np.nonzero(labels == i)
                if len(xs) < 3:
                    elongations.append(1.0)
                    continue
                cov = np.cov(np.stack([xs.astype(float), ys.astype(float)]))
                ev = np.linalg.eigvalsh(cov)          # ascending
                major = float(np.sqrt(max(ev[1], 0.0)))
                # 0.5 px floor: a genuinely 1-px-wide blob has ~0 minor variance and
                # would otherwise divide by zero and report an infinite ratio.
                minor = max(float(np.sqrt(max(ev[0], 0.0))), 0.5)
                elongations.append(major / minor)
            blobElongation = float(np.mean(elongations))
            blobElongationMax = float(np.max(elongations))
            found = np.array([centroids[i] for i in keep], float)
            foundDac = found / (CANVAS / (2 * r)) - r
            d = np.linalg.norm(idealDots[:, None, :] - foundDac[None, :, :], axis=2)
            blobCentroidErrorUnits = float(np.mean(d.min(axis=1)))
        else:
            invalidReasons.append(
                f"no dot blobs of at least {minArea}px detected (expected {blobExpected})")

    # 4. brightness uniformity along ideal path (adaptive density check)
    pathVals = dacImage[idealMask > 0].astype(float)
    litVals = pathVals[pathVals > cfg["binaryThreshold"]]
    if len(litVals) > 10:
        brightnessNonUniformity = float(np.std(litVals) / max(np.mean(litVals), 1.0))
    else:
        brightnessNonUniformity = 1.0
        invalidReasons.append(
            f"only {len(litVals)} lit pixel(s) on the ideal path - brightness "
            f"uniformity is a fallback, not a reading")

    # 5. overall geometry: lit shape's size/position vs. ideal, in DAC units. Compared
    # against the ideal's own polyline coordinates (not idealMask) for exact values,
    # unaffected by rasterization. 1st/99th percentile instead of true min/max so a
    # handful of stray noise pixels surviving the threshold can't blow up the extent.
    pxScale = CANVAS / (2 * r)
    traceRows, traceCols = np.nonzero(trace)
    if len(traceCols) > 10:
        traceDacX = traceCols / pxScale - r
        traceDacY = traceRows / pxScale - r
        xLo, xHi = np.percentile(traceDacX, [1, 99])
        yLo, yHi = np.percentile(traceDacY, [1, 99])
        scaleErrorXPct = ((xHi - xLo) / idealExtentX - 1.0) * 100.0 if idealExtentX > 0 else 0.0
        scaleErrorYPct = ((yHi - yLo) / idealExtentY - 1.0) * 100.0 if idealExtentY > 0 else 0.0
        offsetXUnits = float((xHi + xLo) / 2.0 - idealCenterX)
        offsetYUnits = float((yHi + yLo) / 2.0 - idealCenterY)
    else:
        scaleErrorXPct = scaleErrorYPct = 0.0
        offsetXUnits = offsetYUnits = float(r)     # nothing visible -> worst case, flag it
        invalidReasons.append(
            f"only {len(traceCols)} lit pixel(s) on the canvas - scale/offset are "
            f"fallbacks, not readings")

    valid = not invalidReasons
    w = cfg["costWeights"]
    if valid:
        cost = (w["pathDeviationRms"] * pathDeviationRms / 100.0
                + w["blankLeakage"] * blankLeakage / 10.0
                + w.get("blankCorridorLeakage", 0.0) * blankCorridorLeakage
                + w["cornerHotspot"] * cornerHotspot
                + w["brightnessNonUniformity"] * brightnessNonUniformity
                + w.get("saturationFrac", 0.0) * saturationFrac
                # Dot terms: all three are identically zero for every non-dot pattern
                # (blobExpected == 0 there), so no existing pattern's cost changes.
                # Elongation enters as (ratio - 1) so a perfectly round dot contributes
                # nothing rather than a constant offset.
                + w.get("blobElongation", 0.0) * max(0.0, blobElongation - 1.0)
                + w.get("blobCountError", 0.0) * blobCountError
                + w.get("blobCentroidErrorUnits", 0.0) * blobCentroidErrorUnits / 100.0)
    else:
        # NOT a large number: a large number is still a score, and a search would happily
        # rank two blind trials against each other. NaN forces every consumer to deal with
        # it explicitly (see scoreOrRaise / runStudyForProfile's objective).
        cost = float("nan")
    metrics = Metrics(pathDeviationRms=pathDeviationRms, blankLeakage=blankLeakage,
                      cornerHotspot=cornerHotspot, brightnessNonUniformity=brightnessNonUniformity,
                      valid=valid, invalidReasons=invalidReasons, traceLitPx=traceLitPx,
                      offPathLitPx=offPathLitPx, pathCoveragePct=pathCoveragePct,
                      beamWidthUnits=beamWidthUnits,
                      blankCorridorLeakage=blankCorridorLeakage,
                      blankCorridorMaxVal=blankCorridorMaxVal,
                      blankCorridorLitPx=blankCorridorLitPx,
                      saturationFrac=saturationFrac,
                      blobExpected=blobExpected, blobCount=blobCount,
                      blobCountError=blobCountError, blobElongation=blobElongation,
                      blobElongationMax=blobElongationMax,
                      blobCentroidErrorUnits=blobCentroidErrorUnits,
                      scaleErrorXPct=scaleErrorXPct,
                      scaleErrorYPct=scaleErrorYPct, offsetXUnits=offsetXUnits,
                      offsetYUnits=offsetYUnits, cost=cost)
    debug = {"dacImage": dacImage, "trace": trace, "idealMask": idealMask, "gapMask": gapMask,
             "darkMask": darkMask, "orientation": orientation}
    return metrics, debug


def metricsToDict(m: Metrics) -> dict:
    """vars(Metrics) straight into json.dumps blows up on cost=NaN (json emits a bare
    `NaN`, which is not valid JSON and breaks any strict reader of the .jsonl trial log).
    Also puts `valid`/`invalidReasons` first so an invalid record is obvious at a glance."""
    d = vars(m).copy()
    ordered = {"valid": d.pop("valid"), "invalidReasons": list(d.pop("invalidReasons"))}
    for k, v in d.items():
        ordered[k] = None if isinstance(v, float) and math.isnan(v) else v
    return ordered


def formatInvalid(m: Metrics, pattern: str = "") -> str:
    """One-line, un-missable rendering of an invalid measurement, shared by measure/
    diagnose/optimize so the operator always gets the same wording and the same
    where-to-look hint."""
    what = f" on '{pattern}'" if pattern else ""
    return (f"INVALID MEASUREMENT{what}: " + "; ".join(m.invalidReasons) +
            f"  [lit {m.traceLitPx}px, path coverage {m.pathCoveragePct:.1f}%]")


INVALID_MEASUREMENT_HINT = (
    "the camera is not seeing the beam properly. In likelihood order: binaryThreshold "
    "too high (run 'autotune-camera', or lower it by hand and re-run 'measure'), "
    "exposure/gain too low, beam out of focus or partly outside the calibrated area, "
    "or a stale homography (re-run 'calibrate')."
)


def annotateCanvas(debug: dict, m: Metrics, pattern: str, label: str = "") -> np.ndarray:
    """Visual readout of one measurement, on the same DAC-canvas computeMetrics scores
    against (not the raw camera frame), so what's drawn lines up exactly with what was
    measured: dim green = ideal path, cyan = beam detected on that path (good), red =
    beam detected off the ideal path (the pathDeviationRms/scaleError/offset problem),
    amber = light leaking into a blank corridor (blankLeakage), magenta = raw sensor
    saturation (dacImage >= 250) within the traced beam - painted last so it overrides
    cyan/red/amber there, since a bloomed/clipped pixel can otherwise look exactly like
    a genuine off-path detection (see Metrics.saturationFrac's docstring). The faint
    gray background elsewhere is real camera signal that never crossed binaryThreshold -
    it's not scored at all, on-path or not, and is drawn at HALF brightness so it can
    never be mistaken for detected beam. An invalid measurement gets a red border and a
    reason list rather than looking like any other frame. Used for both saved screenshots
    and the live view so 'what was analyzed' is never just numbers."""
    dacImage, trace, idealMask, gapMask = (debug["dacImage"], debug["trace"],
                                           debug["idealMask"], debug["gapMask"])
    orientation = debug.get("orientation", "identity")
    # Halved, so the un-thresholded camera signal reads as clearly dimmer than anything
    # that actually crossed binaryThreshold and got scored. Before this, a capture where
    # the threshold was so high that only a handful of pixels were scored still LOOKED
    # like a perfectly traced shape here, because the drawing started from the raw
    # pre-threshold image - the exact reason the blind-measurement failure went unnoticed.
    canvas = cv2.cvtColor((dacImage // 2).astype(np.uint8), cv2.COLOR_GRAY2BGR)
    idealDilated = cv2.dilate(idealMask, np.ones((5, 5), np.uint8))
    tracePixels = trace > 0
    matched = tracePixels & (idealDilated > 0)
    deviated = tracePixels & (idealDilated == 0)
    saturated = tracePixels & (dacImage >= 250)
    canvas[idealMask > 0] = (0, 140, 0)
    canvas[matched] = (255, 255, 0)
    canvas[deviated] = (0, 0, 255)
    if gapMask is not None:
        leaking = (gapMask > 0) & (dacImage > 40)
        canvas[leaking] = (0, 200, 255)
    canvas[saturated] = (255, 0, 255)

    title = f"{label + ': ' if label else ''}{pattern}"
    costText = "n/a (invalid)" if math.isnan(m.cost) else f"{m.cost:.3f}"
    lines = [
        title,
        f"path dev {m.pathDeviationRms:.1f}  blank leak {m.blankLeakage:.1f}  "
        f"corridor leak {m.blankCorridorLeakage:.2f}  "
        f"corner hot {m.cornerHotspot:.2f}  uniformity {m.brightnessNonUniformity:.2f}  "
        f"sat {m.saturationFrac * 100:.0f}%",
        f"scale X{m.scaleErrorXPct:+.1f}% Y{m.scaleErrorYPct:+.1f}%  "
        f"offset X{m.offsetXUnits:+.0f} Y{m.offsetYUnits:+.0f}  cost {costText}",
        # The blind-capture readout: everything above can look plausible with almost
        # nothing actually detected, so the counts that decide validity are on screen.
        f"lit {m.traceLitPx}px (off-path {m.offPathLitPx})  "
        f"path coverage {m.pathCoveragePct:.1f}%  beam width {m.beamWidthUnits:.0f}u",
    ]
    for i, line in enumerate(lines):
        cv2.putText(canvas, line, (8, 22 + i * 20), cv2.FONT_HERSHEY_SIMPLEX, 0.5,
                   (255, 255, 255), 1, cv2.LINE_AA)
    warnY = 22 + len(lines) * 20 + 6
    if not m.valid:
        cv2.rectangle(canvas, (2, 2), (canvas.shape[1] - 3, canvas.shape[0] - 3),
                     (0, 0, 255), 4)
        cv2.putText(canvas, "!! INVALID MEASUREMENT - NOT SCORED !!", (8, warnY),
                   cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 0, 255), 2, cv2.LINE_AA)
        warnY += 20
        for reason in m.invalidReasons[:3]:
            cv2.putText(canvas, f"  {reason}", (8, warnY), cv2.FONT_HERSHEY_SIMPLEX,
                       0.45, (0, 0, 255), 1, cv2.LINE_AA)
            warnY += 17
        warnY += 4
    if np.any(saturated):
        cv2.putText(canvas, "magenta = sensor-saturated (likely blooming, not a real "
                   "position/dwell error)", (8, warnY), cv2.FONT_HERSHEY_SIMPLEX, 0.5,
                   (255, 0, 255), 1, cv2.LINE_AA)
        warnY += 20
    if orientation != "identity":
        # Always shown, not just on first detection - the whole point is that this
        # compensation must never be silently invisible in a saved/live image (see
        # detectOrientation()'s warning for what it does and doesn't mean).
        cv2.putText(canvas, f"!! ideal reference auto-oriented: {orientation} !!",
                   (8, warnY), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (0, 0, 255), 2, cv2.LINE_AA)
    return canvas


def measureOnce(esp: EspClient, cam: Camera, cfg: dict, homography: np.ndarray,
                background: np.ndarray, pattern: str, saveTo: Path | None = None,
                statusPrefix: str = "measure",
                params: dict | None = None) -> tuple[Metrics, dict | None]:
    """params, if given, is applied via /api/calib-cam/params right after starting
    the pattern - the ESP32 only accepts /params while a calib-cam session is
    active, and it resets any previous overrides on every /start, so overrides
    must be (re-)applied per pattern rather than once per optimize trial."""
    waitWhilePaused(cam)  # safe boundary: no pattern is running yet
    esp.startPattern(pattern, channel=cfg["camPatternChannel"])
    # Grace period for the actual pattern SWITCH (stop streaming whatever shape was
    # live before, start streaming this one) - separate from settleSeconds below,
    # which is about letting a /params override take effect. Too short here and the
    # tail of the previous pattern (or the very start of this one, before the galvo
    # has caught up) ends up baked into the same max-accumulated capture as an extra,
    # unrelated shape - see patternSwitchSettleSeconds in DEFAULT_CONFIG.
    time.sleep(cfg["patternSwitchSettleSeconds"])
    effective = esp.setParams(params) if params else None
    if effective and effective.get("ignored"):
        raise OptimizerError(
            f"ESP32 ignored param(s) {effective['ignored']} for pattern '{pattern}' - "
            f"these names don't match any field the firmware accepts (see "
            f"OptimizerLiveConfig in GalvOS's include/config.h). Fix the key names in "
            f"{SEARCH_SPACE_FILE.name}; continuing would silently tune nothing."
        )
    time.sleep(cfg["settleSeconds"])
    cam.statusText = f"{statusPrefix}: {pattern}"
    capture = cam.grabAccumulated(cfg["accumFrames"])
    # P9a clip diagnostic: DAC output-limiting clamps corners AFTER the
    # optimizer/pattern layer has already run, so this tool's score is blind
    # to it - poll while the pattern is still live so the numbers reflect
    # what the camera just captured, and warn (never auto-correct: folding
    # this into the objective would let Optuna "fix" saturation by shrinking
    # geometry instead of the operator widening dac_limit_min/max or lowering
    # outputScale).
    try:
        clipState = esp.getState()
        dacClipPct = clipState.get("dacClipPct", 0.0)
        if dacClipPct > 0:
            prWarn(f"{pattern}: DAC clip {dacClipPct:.1f}% "
                   f"(X={clipState.get('dacClipX', 0)} Y={clipState.get('dacClipY', 0)} pts) - "
                   f"this trial's score is unreliable, corners are being clamped, not scaled")
    except OptimizerError as e:
        prWarn(f"{pattern}: could not poll DAC clip status: {e}")
    esp.stop()
    # Same grace period again, symmetrically, before whatever comes next (the next
    # pattern in this same trial, or the next trial's first pattern) is allowed to
    # start - the capture above is already safely in hand, so this delay can never
    # taint ITS own measurement, only protect the NEXT one's.
    time.sleep(cfg["patternSwitchSettleSeconds"])
    if saveTo:
        try:
            cv2.imwrite(str(saveTo), capture)
        except cv2.error as e:
            prWarn(f"could not save capture to {saveTo}: {e}")
    metrics, debug = computeMetrics(capture, background, homography, pattern, cfg)
    if not metrics.valid:
        prWarn(formatInvalid(metrics, pattern))
    annotated = annotateCanvas(debug, metrics, pattern, label=statusPrefix)
    if cam.liveView:
        # Flashes the analyzed result (ideal vs. traced vs. deviation) into the same
        # window right after each measurement - the next capture's raw feed will
        # overwrite it, but it's what makes "what was just analyzed" visible live
        # instead of only ever showing up in a saved file.
        cam.liveView.update(annotated, f"{statusPrefix}: {pattern} (analyzed)")
    if saveTo:
        annotatedPath = saveTo.with_name(f"{saveTo.stem}_annotated{saveTo.suffix}")
        try:
            cv2.imwrite(str(annotatedPath), annotated)
        except cv2.error as e:
            prWarn(f"could not save annotated capture to {annotatedPath}: {e}")
    return metrics, effective


# ── live (no-reference) geometry analysis ────────────────────────────────────
#
# measure/diagnose/optimize all score against a known ideal (idealPolylines()), so
# they only work on the 6 calib patterns. analyze-live has no ideal to compare
# against - it runs on whatever real preset is actually live - so instead it asks
# purely structural questions of the traced beam: does it form one continuous
# piece, and does it enclose an area (closed loop)? That's enough to catch "there's
# a gap" / "this doesn't close" without needing to know what the shape should be.

# Just above sensor/compression noise after background subtraction - low enough to
# catch the dim connecting arcs between bright corner-dwell points (a real, fully-
# connected stroke reads as ONE piece here even when it fragments into many at a
# higher threshold - see analyzeLiveGeometry's docstring), high enough to not pick
# up per-pixel dark-frame noise as spurious extra pieces.
LIVE_ANALYSIS_FLOOR_THRESHOLD = 12


def _filteredComponents(mask: np.ndarray, minAreaPx: float) -> tuple[int, np.ndarray, np.ndarray | None]:
    """mask: uint8, 0 or 255. Dilates by 1px first so a trace broken only by sub-
    pixel antialiasing/dropout isn't miscounted as multiple pieces, then drops any
    component smaller than minAreaPx (liveAnalysisMinComponentPx) as sensor noise/
    dust rather than a real piece of the beam trace - without this, a handful of
    stray noise specks (observed 9-57px on this rig) can inflate a genuinely
    continuous, single-gap trace's piece count into the double digits. Returns
    (piece count, filtered mask of ALL kept pieces, mask of the single LARGEST kept
    piece alone or None if nothing survived the filter). The "all pieces" mask feeds
    the saved screenshot and bbox/saturation stats (everything real that's visible);
    the "largest piece alone" mask feeds the closure test, deliberately excluding
    any other real-but-unrelated piece (a second object, a stray reflection just
    above the noise floor) that would otherwise corrupt what "does THIS shape
    enclose an area" is asking about."""
    dilated = cv2.dilate(mask, np.ones((3, 3), np.uint8))
    n, labels, stats, _ = cv2.connectedComponentsWithStats(dilated)
    filtered = np.zeros_like(dilated)
    kept = [i for i in range(1, n) if stats[i, cv2.CC_STAT_AREA] >= minAreaPx]
    for i in kept:
        filtered[labels == i] = 255
    largest = None
    if kept:
        biggest = max(kept, key=lambda i: stats[i, cv2.CC_STAT_AREA])
        largest = np.where(labels == biggest, np.uint8(255), np.uint8(0))
    return len(kept), filtered, largest


def analyzeLiveGeometry(dacImage: np.ndarray, cfg: dict) -> dict:
    """No-reference structural read of an already background-subtracted, DAC-canvas-
    warped beam trace (see warpToDacCanvas) - works on any preset, not just the 6
    calib patterns computeMetrics knows the ideal shape of. Returns a dict:

      litPixelCount        - RAW trace pixel count at the permissive floor threshold,
                              before noise filtering. <30 means "nothing meaningful
                              visible" to the caller; this stays unfiltered so a
                              legitimately sparse preset (e.g. a few Particles dots)
                              isn't misreported as "nothing visible".
      componentsAtFloor     - piece count at LIVE_ANALYSIS_FLOOR_THRESHOLD, after
                              dropping sub-liveAnalysisMinComponentPx noise blobs
                              (see _filteredComponents). The key signal: a genuinely
                              continuous stroke reads as 1 here even if corner-dwell
                              brightness makes it look beaded at a higher threshold.
                              Stays >1 only for an actual break in the beam path, or
                              a preset that's legitimately multi-piece by design
                              (particles, multi-object scenes).
      componentsAtConfigured - piece count at cfg['binaryThreshold'] (what the rest
                              of this tool treats as "on"), same noise filtering.
                              Usually >= componentsAtFloor for corner-dwell-heavy
                              patterns; the gap between the two is brightness/dwell
                              unevenness, not a defect.
      closedLoop            - True/False/None. Flood-fills from the canvas border
                              around the SINGLE LARGEST noise-filtered floor-
                              threshold piece (not the union of all pieces - a
                              second, unrelated real piece elsewhere in frame
                              shouldn't corrupt whether the main shape closes) and
                              checks whether that piece's own centroid is sealed off
                              (encloses an area) or reachable (open path). None if
                              too little is visible, or the centroid itself sits on
                              the piece (can't judge).
      bboxDacUnits           - (width, height) of the floor-threshold trace in DAC
                              units (1st-99th percentile, like computeMetrics' scale
                              check), or None if nothing visible.
      saturationFrac         - fraction of the floor-threshold trace at raw sensor
                              saturation (dacImage >= 250) - see Metrics.
                              saturationFrac's blooming caveat. NOTE: computed over
                              the floor trace here, not the configured-threshold one
                              Metrics uses, since analyze-live's whole point is to
                              look at the full trace including its dim segments.
    """
    h, w = dacImage.shape
    _, maskFloorRaw = cv2.threshold(dacImage, LIVE_ANALYSIS_FLOOR_THRESHOLD, 255, cv2.THRESH_BINARY)

    result = {
        "litPixelCount": int((maskFloorRaw > 0).sum()),
        "componentsAtFloor": 0,
        "componentsAtConfigured": 0,
        "closedLoop": None,
        "bboxDacUnits": None,
        "saturationFrac": 0.0,
    }
    if result["litPixelCount"] < 30:
        return result   # nothing meaningful visible - caller reports INDETERMINATE

    minAreaPx = cfg.get("liveAnalysisMinComponentPx", 60)
    result["componentsAtFloor"], maskFloor, largestFloor = _filteredComponents(maskFloorRaw, minAreaPx)
    _, maskConfiguredRaw = cv2.threshold(dacImage, cfg["binaryThreshold"], 255, cv2.THRESH_BINARY)
    result["componentsAtConfigured"], _, _ = _filteredComponents(maskConfiguredRaw, minAreaPx)
    if not np.any(maskFloor):
        return result   # everything below minAreaPx was noise - too faint to say more

    result["saturationFrac"] = float(np.mean(dacImage[maskFloor > 0] >= 250))

    r = cfg["dacRange"]
    dacPerPixel = (2 * r) / CANVAS
    ys, xs = np.nonzero(maskFloor)
    xLo, xHi = np.percentile(xs, [1, 99])
    yLo, yHi = np.percentile(ys, [1, 99])
    result["bboxDacUnits"] = ((xHi - xLo) * dacPerPixel, (yHi - yLo) * dacPerPixel)

    # Closed-loop test: flood-fill from a canvas corner (off-trace - the homography
    # maps the usable DAC range well inside the CANVAS x CANVAS square) around the
    # largest single piece only, and check whether that piece's own centroid is
    # reachable from outside it.
    lys, lxs = np.nonzero(largestFloor)
    cx, cy = int(lxs.mean()), int(lys.mean())
    if largestFloor[cy, cx] == 0:   # centroid must be off-trace for this to mean anything
        free = 255 - largestFloor
        ffMask = np.zeros((h + 2, w + 2), np.uint8)
        cv2.floodFill(free, ffMask, (0, 0), 128)
        result["closedLoop"] = bool(free[cy, cx] != 128)
    return result


def annotateLiveGeometry(dacImage: np.ndarray, geometry: dict, cfg: dict, label: str = "") -> np.ndarray:
    """Visual readout for analyze-live's saved/live-view screenshot: cyan = beam
    trace at the permissive, noise-filtered floor threshold (what componentsAtFloor/
    closedLoop are computed from - recomputed here rather than threaded through
    `geometry` so that dict stays plain printable/JSON-able data), magenta = raw
    sensor saturation within it (see Metrics.saturationFrac's blooming caveat -
    painted last so a bloomed dwell point isn't mistaken for extra disconnected
    pieces). Unlike annotateCanvas, there's no ideal-path overlay - there is no
    reference geometry to compare against here."""
    _, maskFloorRaw = cv2.threshold(dacImage, LIVE_ANALYSIS_FLOOR_THRESHOLD, 255, cv2.THRESH_BINARY)
    _, maskFloor, _ = _filteredComponents(maskFloorRaw, cfg.get("liveAnalysisMinComponentPx", 60))
    canvas = cv2.cvtColor(dacImage, cv2.COLOR_GRAY2BGR)
    tracePixels = maskFloor > 0
    saturated = tracePixels & (dacImage >= 250)
    canvas[tracePixels] = (255, 255, 0)
    canvas[saturated] = (255, 0, 255)

    lines = [label] if label else []
    lines.append(
        f"pieces: floor={geometry['componentsAtFloor']} configured={geometry['componentsAtConfigured']}  "
        f"closed={geometry['closedLoop']}  sat={geometry['saturationFrac'] * 100:.0f}%")
    if geometry["bboxDacUnits"]:
        bx, by = geometry["bboxDacUnits"]
        lines.append(f"bbox {bx:.0f} x {by:.0f} DAC units  lit px {geometry['litPixelCount']}")
    for i, line in enumerate(lines):
        cv2.putText(canvas, line, (8, 22 + i * 20), cv2.FONT_HERSHEY_SIMPLEX, 0.5,
                   (255, 255, 255), 1, cv2.LINE_AA)
    if np.any(saturated):
        cv2.putText(canvas, "magenta = sensor-saturated (likely blooming at a dwell "
                   "point, not necessarily a real defect)", (8, 22 + len(lines) * 20),
                   cv2.FONT_HERSHEY_SIMPLEX, 0.5, (255, 0, 255), 1, cv2.LINE_AA)
    return canvas


def describeLiveSource(esp: EspClient, presetsCache: list[dict] | None) -> dict:
    """Figures out what's actually driving the current output, so analyze-live's
    report is labeled correctly instead of assuming a preset is active. presetsCache:
    an already-fetched GET /api/presets list (or None if that call failed) - analyze-
    live only ever needs one, so it's fetched once by the caller."""
    state = esp.getState()
    if state.get("calib_active"):
        return {"kind": "calib", "category": None,
                "label": "calibration/test pattern (calib-cam tool) - not a real "
                "preset, the analysis below describes that test pattern instead"}
    if state.get("ilda_active"):
        return {"kind": "ilda", "category": None, "label": "ILDA file playback"}
    if state.get("playlist_active"):
        return {"kind": "playlist", "category": None, "label": "playlist"}
    presetIdx = state.get("preset_idx")
    if isinstance(presetIdx, int) and presetIdx >= 0 and presetsCache:
        match = next((p for p in presetsCache if p.get("idx") == presetIdx), None)
        if match:
            return {"kind": "preset", "category": match.get("cat"),
                    "presetName": match.get("name"),
                    "label": f"preset '{match.get('name')}' ({match.get('cat')})"}
    return {"kind": "custom", "category": None,
            "label": "custom output (text/paint/curves mode, or an unrecognized "
            "preset index) - no preset name available"}


def runAnalyzeLive(cfg: dict, esp: EspClient, cam: Camera):
    """Analyzes whatever is currently projecting on the ESP32. Unlike measure/
    diagnose/optimize, this NEVER calls startPattern()/stop() - it captures exactly
    one frame of the live output and leaves the controller alone throughout (see
    dispatch(), which special-cases this command to skip its usual esp.stop() on
    exit too). Trades the reference-based scoring those commands do (limited to the
    6 calib patterns, since that's all that has a known ideal shape) for a generic,
    no-reference structural read that works on any preset: whether the beam trace
    is one continuous piece and whether it encloses an area (closed loop), plus a
    saturation check. This catches "doesn't close" / "has a gap" - it can NOT tell
    you a shape is the WRONG shape (e.g. an octagon silently missing one edge's
    interior points, with the rest still tracing a closed loop, reads as fine here);
    for that, look at the saved screenshot. Requires an existing homography.npz -
    run 'calibrate' first."""
    homography, background = loadHomography()

    status = esp.getStatus()
    if not status.get("laser_armed"):
        prWarn("laser_armed is false on the controller - nothing will be "
             "visible until it's armed. Capturing anyway.")
    if not status.get("estop_ok") or not status.get("scanfail_ok"):
        prWarn("E-Stop or scan-fail interlock is tripped - nothing will be "
             "visible. Capturing anyway.")

    try:
        presetsCache = esp.getPresets()
    except OptimizerError as e:
        prWarn(f"could not fetch /api/presets ({e}) - preset name lookup "
             f"will be skipped, the report will describe the raw preset index only")
        presetsCache = None
    source = describeLiveSource(esp, presetsCache)
    pr(f"analyzing live output: {bold(source['label'])}")

    cam.statusText = "analyze-live: capturing current output (not touching the pattern)"
    capture = cam.grabAccumulated(cfg["accumFrames"])
    diff = cv2.subtract(capture, background)
    dacImage = warpToDacCanvas(diff, homography, cfg["dacRange"])
    geometry = analyzeLiveGeometry(dacImage, cfg)

    try:
        RESULTS_DIR.mkdir(exist_ok=True)
    except OSError as e:
        prWarn(f"could not create {RESULTS_DIR.name}/ for screenshots: {e}")

    timestamp = time.strftime("%Y-%m-%d_%H-%M-%S")
    rawPath = RESULTS_DIR / f"analyze_live_{timestamp}.png"
    try:
        cv2.imwrite(str(rawPath), capture)
    except cv2.error as e:
        prWarn(f"could not save {rawPath}: {e}")

    pr()
    pr(bold("=== analyze-live ==="))
    if geometry["litPixelCount"] < 30:
        prWarn("INDETERMINATE - essentially nothing detected in the frame.")
        prTip("Check: laser armed / E-Stop / scan-fail (see warnings above), camera "
             "exposure/threshold/framing, or that something is actually projecting.")
    else:
        bx, by = geometry["bboxDacUnits"]
        closed = geometry["closedLoop"]
        prTable([
            ("lit trace",       f"{geometry['litPixelCount']} px"),
            ("bounding box",    f"{bx:.0f} x {by:.0f} DAC units"),
            ("sensor saturated", f"{geometry['saturationFrac'] * 100:.0f}%"),
            ("pieces (floor)",  geometry["componentsAtFloor"]),
            ("pieces (configured)", f"{geometry['componentsAtConfigured']} "
                                     f"@ threshold {cfg['binaryThreshold']}"),
            ("closed loop",     closed if closed is not None else "indeterminate"),
        ], headers=("metric", "value"))
        if geometry["componentsAtConfigured"] > geometry["componentsAtFloor"]:
            prInfo("that gap is brightness/dwell unevenness - dimmer connecting "
                 "arcs between brighter dwell points - not a broken path; see the "
                 "annotated screenshot")

        expectClosed = CATEGORY_EXPECTS_CLOSED.get(source.get("category"))
        flag = None
        if geometry["componentsAtFloor"] > 1 and closed is True:
            # The largest single piece already forms a fully sealed loop on its
            # own (closedLoop is tested against that piece alone, see
            # analyzeLiveGeometry) - the extra piece(s) are very likely unrelated
            # clutter (a stray blob just above the noise floor, a second object)
            # rather than a break in the main shape, so this is downgraded from a
            # gap warning to an FYI.
            flag = (f"NOTE - trace fragments into {geometry['componentsAtFloor']} "
                    f"piece(s) at the floor threshold, but the largest one already "
                    f"forms a fully closed loop by itself - likely unrelated "
                    f"clutter, not a break in the main shape. Check the annotated "
                    f"screenshot to confirm.")
        elif geometry["componentsAtFloor"] > 1 and source.get("presetName") in KNOWN_MULTI_PIECE_PRESETS:
            flag = (f"NOTE - '{source.get('presetName')}' fragments into "
                    f"{geometry['componentsAtFloor']} piece(s) at the floor threshold, "
                    f"but that's expected - it's a known multi-object/multi-segment "
                    f"preset by design (see KNOWN_MULTI_PIECE_PRESETS), not a beam-path "
                    f"gap.")
        elif geometry["componentsAtFloor"] > 1:
            flag = ("POSSIBLE GAP / DISCONNECTED SEGMENT - the trace does not form a "
                    "single continuous piece even at the most permissive threshold. "
                    "This is expected by design for multi-object/particle presets "
                    "(Starfield, Confetti Burst, Grid 3x3, Three Circles, Multi Wave, "
                    "Radial Waves, Wave Field, ...) - only worth investigating if this "
                    "preset is meant to be one continuous stroke.")
        elif expectClosed is True and closed is False:
            flag = (f"NOTE - '{source.get('presetName')}' is category "
                    f"'{source['category']}' (normally a closed shape) but the "
                    f"traced loop reads as open. Could be a real gap too small to "
                    f"break connectivity at this threshold, or just this heuristic "
                    f"being wrong for this preset - check the annotated screenshot.")
        pr()
        if flag:
            prWarn(flag)
        else:
            prOk("no structural issue detected (this is an informational read, "
                 "not a full optimizer diagnostic - see 'diagnose' for that, on the "
                 "6 calib patterns).")

    annotated = annotateLiveGeometry(dacImage, geometry, cfg, label=source["label"])
    if cam.liveView:
        cam.liveView.update(annotated, "analyze-live (analyzed)")
    annotatedPath = rawPath.with_name(f"{rawPath.stem}_annotated{rawPath.suffix}")
    try:
        cv2.imwrite(str(annotatedPath), annotated)
        pr()
        prOk(f"screenshots saved -> {rawPath.relative_to(Path(__file__).parent)}, "
             f"{annotatedPath.name}")
    except cv2.error as e:
        prWarn(f"could not save {annotatedPath}: {e}")


# ── optimization ─────────────────────────────────────────────────────────────

def defaultSearchSpace() -> dict:
    """Default searchSpace.json content - written to disk by loadSearchSpaceFile()
    the first time the file is missing, the same way 'wizard' regenerates a missing
    camConfig.json. There is no separate shipped example file to fall out of sync
    with (the whole directory is gitignored, see .gitignore), so this IS the
    source of truth for what a fresh checkout gets. Ranges are centered on each
    profile's firmware default (OPT_PROFILE_DEFAULTS in include/config.h),
    narrower than the WebUI sliders' full legal range so a first 'optimize' run
    searches something useful out of the box instead of the entire range."""
    return {
        "_comment": "Auto-generated by loadSearchSpaceFile() because this file was "
                    "missing - see that function's docstring. Ranges are a tuning "
                    "starting point, not a firmware limit; edit freely. 'patterns' "
                    "per profile is optional (defaults to the profile's own camera "
                    "pattern(s), FW_PROFILE_PATTERNS in this script, if omitted).",
        "Vector": {
            "patterns": ["square", "star"],
            "params": {
                "corner_angle_deg": {"type": "float", "min": 10.0, "max": 60.0},
                "min_corner_pts": {"type": "int", "min": 1, "max": 4},
                "max_corner_pts": {"type": "int", "min": 4, "max": 14},
                "pts_per_1000_units": {"type": "float", "min": 3.0, "max": 20.0},
                "blank_samples": {"type": "int", "min": 8, "max": 30},
                "min_blank_samples": {"type": "int", "min": 2, "max": 10},
                "blank_pts_per_1000_units": {"type": "float", "min": 2.0, "max": 16.0},
                "stage1_blank_target": {"type": "int", "min": 6, "max": 20},
                "min_interior_pts_per_segment": {"type": "int", "min": 2, "max": 14},
            },
        },
        "Smooth": {
            "patterns": ["circle"],
            "params": {
                "max_corner_pts": {"type": "int", "min": 2, "max": 5},
                "pts_per_1000_units": {"type": "float", "min": 5.0, "max": 25.0},
                "blank_samples": {"type": "int", "min": 8, "max": 30},
                "min_blank_samples": {"type": "int", "min": 2, "max": 10},
                "min_interior_pts_per_segment": {"type": "int", "min": 4, "max": 16},
            },
        },
        "Waves": {
            "patterns": ["spiral"],
            "params": {
                "corner_angle_deg": {"type": "float", "min": 15.0, "max": 60.0},
                "max_corner_pts": {"type": "int", "min": 3, "max": 10},
                "pts_per_1000_units": {"type": "float", "min": 3.0, "max": 18.0},
                "blank_samples": {"type": "int", "min": 8, "max": 30},
                "min_blank_samples": {"type": "int", "min": 2, "max": 10},
                "min_interior_pts_per_segment": {"type": "int", "min": 2, "max": 14},
            },
        },
        "MultiObject": {
            "patterns": ["segments"],
            "params": {
                "blank_samples": {"type": "int", "min": 10, "max": 40},
                "min_blank_samples": {"type": "int", "min": 2, "max": 12},
                "blank_pts_per_1000_units": {"type": "float", "min": 0.5, "max": 4.0},
                "stage1_blank_target": {"type": "int", "min": 6, "max": 20},
                "corner_angle_deg": {"type": "float", "min": 10.0, "max": 45.0},
                "max_corner_pts": {"type": "int", "min": 3, "max": 10},
            },
        },
        # Wireframe/Text/Particles got their camera patterns in fw v6.75.0
        # (calib_patterns.cpp idx 21/22/23). Ranges centered on
        # OPT_PROFILE_DEFAULTS[3]/[7]/[5] respectively.
        "Wireframe": {
            "patterns": ["wireframe"],
            "params": {
                "corner_angle_deg": {"type": "float", "min": 10.0, "max": 45.0},
                "min_corner_pts": {"type": "int", "min": 1, "max": 4},
                "max_corner_pts": {"type": "int", "min": 4, "max": 14},
                "pts_per_1000_units": {"type": "float", "min": 3.0, "max": 14.0},
                "blank_samples": {"type": "int", "min": 10, "max": 44},
                "min_blank_samples": {"type": "int", "min": 2, "max": 12},
                "blank_pts_per_1000_units": {"type": "float", "min": 0.3, "max": 3.0},
                "stage1_blank_target": {"type": "int", "min": 6, "max": 20},
                "min_interior_pts_per_segment": {"type": "int", "min": 2, "max": 12},
            },
        },
        "Text": {
            "patterns": ["text"],
            "params": {
                "corner_angle_deg": {"type": "float", "min": 10.0, "max": 50.0},
                "min_corner_pts": {"type": "int", "min": 1, "max": 4},
                "max_corner_pts": {"type": "int", "min": 3, "max": 12},
                "pts_per_1000_units": {"type": "float", "min": 3.0, "max": 14.0},
                "blank_samples": {"type": "int", "min": 8, "max": 36},
                "min_blank_samples": {"type": "int", "min": 2, "max": 12},
                "blank_pts_per_1000_units": {"type": "float", "min": 0.4, "max": 3.0},
                "stage1_blank_target": {"type": "int", "min": 4, "max": 16},
                "min_interior_pts_per_segment": {"type": "int", "min": 1, "max": 8},
            },
        },
        # Particles is the blanked-jump profile: nearly all of its cost is the
        # jump window, so the dwell params get a narrow range and the blanking
        # ones a wide one. min_blank_samples' range deliberately reaches up to
        # 40 - the firmware default is 32 after the v6.65.2 settle-window fix
        # (config.h), and anything much below ~16 reintroduces the streaks.
        "Particles": {
            "patterns": ["particles"],
            "params": {
                "max_corner_pts": {"type": "int", "min": 2, "max": 10},
                "blank_samples": {"type": "int", "min": 16, "max": 64},
                "min_blank_samples": {"type": "int", "min": 8, "max": 40},
                "blank_pts_per_1000_units": {"type": "float", "min": 0.3, "max": 3.0},
                "stage1_blank_target": {"type": "int", "min": 4, "max": 16},
            },
        },
    }


def loadSearchSpaceFile() -> dict:
    if not SEARCH_SPACE_FILE.exists():
        spaces = defaultSearchSpace()
        try:
            SEARCH_SPACE_FILE.write_text(json.dumps(spaces, indent=2))
        except OSError as e:
            raise OptimizerError(f"cannot create {SEARCH_SPACE_FILE}: {e}") from e
        prWarn(f"{SEARCH_SPACE_FILE.name} was missing - regenerated with default "
              f"ranges for {', '.join(profileNames(spaces))} (centered on each "
              f"profile's firmware default). Review before relying on it for real "
              f"tuning.")
        return spaces
    try:
        spaces = json.loads(SEARCH_SPACE_FILE.read_text())
    except json.JSONDecodeError as e:
        raise OptimizerError(f"{SEARCH_SPACE_FILE.name} is not valid JSON: {e}") from e
    except OSError as e:
        raise OptimizerError(f"cannot read {SEARCH_SPACE_FILE}: {e}") from e

    if not isinstance(spaces, dict):
        raise OptimizerError(f"{SEARCH_SPACE_FILE.name} must contain a JSON object")

    # An existing file is never rewritten (it is the user's own tuning ranges),
    # so profiles that gained a camera pattern after it was created would just
    # silently not show up as tunable. Say so instead, with the exact keys to
    # copy from defaultSearchSpace().
    stale = [n for n in defaultSearchSpace()
             if not n.startswith("_") and n not in spaces]
    if stale:
        prWarn(f"{SEARCH_SPACE_FILE.name} has no entry for {', '.join(stale)} - "
               f"those profiles are camera-tunable but will be skipped by "
               f"'optimize'. Delete the file to regenerate it with defaults, or "
               f"copy the missing block(s) from defaultSearchSpace() in this "
               f"script.")
    return spaces


def profileNames(spaces: dict) -> list[str]:
    """Profile keys, excluding documentation entries like '_comment'."""
    return [k for k in spaces if not k.startswith("_")]


def fetchFirmwareProfiles(esp: EspClient, config: dict | None = None) -> list[dict]:
    """Always queries the live ESP32 (GET /api/config) for which optimizer profiles
    it actually has - rather than assuming a fixed local list is still accurate.
    Pass an already-fetched config dict to avoid a second round-trip."""
    if config is None:
        config = esp.getConfig()
    members = config.get("opt_profile_members")
    if not isinstance(members, list):
        raise OptimizerError(
            "ESP32 /api/config response has no 'opt_profile_members' - firmware may "
            "be too old or the calib-cam optimizer API has changed. Run "
            "'optimizeGalvo.py check' to see the fw_version."
        )
    activeIdx = config.get("opt_active_profile")
    profiles = []
    for i, memberList in enumerate(members):
        name = FIRMWARE_PROFILE_NAMES[i] if i < len(FIRMWARE_PROFILE_NAMES) else f"profile{i}"
        profiles.append({
            "index": i,
            "name": name,
            "members": memberList if isinstance(memberList, list) else [],
            "active": i == activeIdx,
        })
    return profiles


def resolveProfileForPreset(fwProfiles: list[dict], presetName: str) -> str:
    """Maps a real preset name (e.g. 'Milky Way') to the firmware optimizer profile
    that governs it, via each profile's live 'members' list (GET /api/config). Lets
    --preset stand in for --profile when you know which preset looks wrong but not
    which optimizer profile drives it - selectFirmwareProfiles still does the actual
    camera-tunability check on the name this resolves to."""
    matches = [p for p in fwProfiles
              if any(m.lower() == presetName.lower() for m in p["members"])]
    if not matches:
        allMembers = sorted({m for p in fwProfiles for m in p["members"]})
        raise OptimizerError(
            f"no firmware profile reports a preset named '{presetName}' - known preset(s): "
            f"{allMembers or '(none reported)'}. Check spelling/case (run "
            f"'optimizeGalvo.py optimize' without --preset to pick a profile directly instead)."
        )
    return matches[0]["name"]


def selectFirmwareProfiles(spaces: dict, requested: str | None,
                           fwProfiles: list[dict]) -> list[dict]:
    """Returns the firmware profiles to tune (list of fetchFirmwareProfiles entries).
    Tunable = has camera pattern(s) AND a search space in searchSpace.json. With
    `requested` (comma-separated names or 'all') validates that; otherwise offers an
    interactive multi-select menu showing each profile's member presets."""
    tunable = [p for p in fwProfiles
               if p["name"] in FW_PROFILE_PATTERNS and p["name"] in spaces]
    if not tunable:
        raise OptimizerError(
            f"no tunable profiles: {SEARCH_SPACE_FILE.name} must define at least one of "
            f"{sorted(FW_PROFILE_PATTERNS)} (the camera-tunable firmware profiles)"
        )
    byName = {p["name"]: p for p in tunable}

    if requested is not None:
        if requested.strip().lower() == "all":
            return tunable
        names = [n.strip() for n in requested.split(",") if n.strip()]
        missing = [n for n in names if n not in byName]
        if missing:
            raise OptimizerError(
                f"profile(s) {missing} not tunable - available: {sorted(byName)} "
                f"(must have camera patterns and an entry in {SEARCH_SPACE_FILE.name})"
            )
        return [byName[n] for n in names]

    if len(tunable) == 1:
        pr(f"only one tunable profile - using '{tunable[0]['name']}'")
        return tunable

    if not sys.stdin.isatty():
        raise OptimizerError(
            f"multiple tunable profiles available ({sorted(byName)}) - pass --profile "
            f"with comma-separated names or 'all' in a non-interactive session"
        )

    pr("\ntunable firmware profiles:")
    for i, p in enumerate(tunable, 1):
        patterns = spaces[p["name"]].get("patterns", FW_PROFILE_PATTERNS[p["name"]])
        nParams = len(spaces[p["name"]].get("params", {}))
        members = ", ".join(p["members"]) if p["members"] else "(no presets)"
        active = "  [active]" if p["active"] else ""
        pr(f"  {i}. {p['name']}{active}  -  cam pattern(s): {', '.join(patterns)}, "
              f"{nParams} param(s)")
        pr(f"       presets: {members}")
    while True:
        raw = input(f"select profile(s) to tune [numbers/names, comma-separated, or "
                    f"'all']: ").strip().lower()
        if raw == "all":
            return tunable
        picks, bad = [], []
        for token in (t.strip() for t in raw.split(",") if t.strip()):
            if token.isdigit() and 1 <= int(token) <= len(tunable):
                picks.append(tunable[int(token) - 1])
            else:
                match = next((p for p in tunable if p["name"].lower() == token), None)
                picks.append(match) if match else bad.append(token)
        if picks and not bad:
            seen = set()
            return [p for p in picks if not (p["name"] in seen or seen.add(p["name"]))]
        pr(f"  invalid choice(s) {bad or [raw]} - enter numbers 1-{len(tunable)}, "
              f"profile names, or 'all'")


def baselineParamsFor(config: dict, profileIndex: int) -> dict:
    """Current parameter values of one firmware profile, from GET /api/config's
    opt_profiles array - keys stripped of their 'opt_' prefix so they line up with
    the field names /api/calib-cam/params and /api/optimizer-live accept."""
    profiles = config.get("opt_profiles")
    if not isinstance(profiles, list) or profileIndex >= len(profiles):
        raise OptimizerError(
            "ESP32 /api/config has no usable 'opt_profiles' array - firmware too old "
            "for the before/after report. Run 'optimizeGalvo.py check'."
        )
    return {k[4:]: v for k, v in profiles[profileIndex].items()
            if k.startswith("opt_") and not k.startswith("opt_eff_")}


def valuesEqual(a, b) -> bool:
    if isinstance(a, bool) or isinstance(b, bool):
        return bool(a) == bool(b)
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return abs(a - b) <= 1e-6 * max(abs(a), abs(b), 1.0)
    return a == b


def buildChangeReport(baseline: dict, bestParams: dict) -> tuple[list, list]:
    """Compares a profile's pre-tuning baseline against the best found params.
    Returns (changed, unchanged): changed = (field, before, after[, note]),
    unchanged = (field, value, reason)."""
    changed, unchanged = [], []
    for field in sorted(baseline):
        before = baseline[field]
        gate = GATED_PARAMS.get(field)
        if field in bestParams:
            after = bestParams[field]
            gateOff = gate is not None and not bestParams.get(gate, baseline.get(gate, False))
            if valuesEqual(before, after):
                unchanged.append((field, before,
                                  "searched - best trial kept the baseline value"))
            elif gateOff:
                unchanged.append((field, before,
                                  f"searched (best trial: {after}) but has no effect "
                                  f"while {gate}=false - value not applied"))
            else:
                changed.append((field, before, after))
        else:
            gateValue = bestParams.get(gate, baseline.get(gate, False)) if gate else None
            if gate and not gateValue:
                unchanged.append((field, before,
                                  f"not searched - inactive anyway ({gate}=false)"))
            else:
                unchanged.append((field, before,
                                  f"not searched (not in {SEARCH_SPACE_FILE.name} "
                                  f"for this profile)"))
    return changed, unchanged


def formatValue(v) -> str:
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, float):
        return f"{v:.4g}"
    return str(v)


def printChangeReport(profileName: str, changed: list, unchanged: list):
    pr()
    pr(bold(f"=== {profileName}: parameter changes (before -> after) ==="))
    if changed:
        pr("changed:")
        for field, before, after in changed:
            pr(f"  {field:<30} {formatValue(before):>10} -> {formatValue(after)}")
    else:
        pr("changed: (none)")
    pr("unchanged:")
    for field, value, reason in unchanged:
        pr(f"  {field:<30} {formatValue(value):>10}   {reason}")


def validateProfileSpace(profile: str, space: dict) -> dict:
    if not isinstance(space, dict):
        raise OptimizerError(f"profile '{profile}' in {SEARCH_SPACE_FILE.name} must be a "
                             f"JSON object")
    allowedPatterns = FW_PROFILE_PATTERNS.get(profile)
    if allowedPatterns is None:
        raise OptimizerError(
            f"profile '{profile}' in {SEARCH_SPACE_FILE.name} is not a camera-tunable "
            f"firmware profile (known: {sorted(FW_PROFILE_PATTERNS)})"
        )
    patterns = space.get("patterns", allowedPatterns)
    badPatterns = [p for p in patterns if p not in allowedPatterns]
    if badPatterns:
        raise OptimizerError(
            f"profile '{profile}' lists pattern(s) {badPatterns} that don't tune it - "
            f"its own camera pattern(s): {allowedPatterns}"
        )
    space = {**space, "patterns": patterns}
    if not space.get("params"):
        raise OptimizerError(f"profile '{profile}' in {SEARCH_SPACE_FILE.name} has no "
                             f"non-empty 'params' defined")
    for name, spec in space["params"].items():
        if not isinstance(spec, dict) or spec.get("type") not in ("int", "float", "categorical"):
            raise OptimizerError(
                f"param '{name}' in profile '{profile}' has invalid/missing 'type' "
                f"(must be int/float/categorical)"
            )
        if spec["type"] in ("int", "float"):
            if "min" not in spec or "max" not in spec:
                raise OptimizerError(f"param '{name}' in profile '{profile}' needs "
                                     f"'min' and 'max'")
            if spec["min"] >= spec["max"]:
                raise OptimizerError(f"param '{name}' in profile '{profile}': "
                                     f"min ({spec['min']}) must be < max ({spec['max']})")
        elif spec["type"] == "categorical" and not spec.get("choices"):
            raise OptimizerError(f"param '{name}' in profile '{profile}' needs a "
                                 f"non-empty 'choices' list")
    return space


def suggestParams(trial, space: dict) -> dict:
    params = {}
    for name, spec in space["params"].items():
        if spec["type"] == "int":
            params[name] = trial.suggest_int(name, spec["min"], spec["max"], step=spec.get("step", 1))
        elif spec["type"] == "float":
            params[name] = trial.suggest_float(name, spec["min"], spec["max"])
        elif spec["type"] == "categorical":
            params[name] = trial.suggest_categorical(name, spec["choices"])
    return params


def runStudyForProfile(optuna, cfg: dict, esp: EspClient, cam: Camera,
                       profileName: str, space: dict, trials: int,
                       studyName: str, storageUrl: str,
                       homography: np.ndarray, background: np.ndarray):
    """Runs one Optuna study tuning a single firmware profile via its own camera
    pattern(s). Returns (study, stoppedEarly) - stoppedEarly is None on a full run,
    otherwise a human-readable reason; a quit/interrupt also aborts remaining
    profiles in the caller's loop."""
    patterns = space["patterns"]
    logFile = RESULTS_DIR / f"optimize_{profileName}_{time.strftime('%Y-%m-%d_%H-%M-%S')}.jsonl"
    trialDurations: list[float] = []
    runIndex = 0
    invalidTrials = 0
    invalidAbort: str | None = None
    # Below this many attempts the invalid FRACTION is too noisy to act on (1 bad trial
    # out of 2 is 50%, but says nothing) - the guard needs a minimum sample first.
    minTrialsForInvalidAbort = 4

    def measureValid(pattern, params, label):
        """One measurement, retried once on an invalid result before giving up. A single
        invalid reading can be a transient (a pattern switch not fully settled, someone
        walking through the beam path); a repeat is the rig, not the moment."""
        m, effective = measureOnce(
            esp, cam, cfg, homography, background, pattern,
            statusPrefix=label, params=params)
        if not m.valid:
            prWarn(f"retrying '{pattern}' once after an invalid measurement ...")
            m, effective = measureOnce(
                esp, cam, cfg, homography, background, pattern,
                statusPrefix=f"{label} (retry)", params=params)
        return m, effective

    def objective(trial):
        nonlocal runIndex, invalidTrials, invalidAbort
        trialStart = time.monotonic()
        if cam.liveView:
            cam.liveView.setProgress(runIndex + 1, trials)
        params = suggestParams(trial, space)
        totalCost = 0.0
        perPattern = {}
        effectivePerPattern = {}
        invalidPatterns = []
        for pattern in patterns:
            # params are re-applied for every pattern, not once per trial: the ESP32
            # only accepts /params while a calib-cam session is active, and each
            # /start resets any previous overrides back to the profile baseline.
            m, effective = measureValid(
                pattern, params,
                f"optimize {profileName} trial {runIndex + 1}/{trials}")
            if m.valid:
                totalCost += m.cost
            else:
                invalidPatterns.append(pattern)
            trial.set_user_attr(f"metrics_{pattern}", metricsToDict(m))
            perPattern[pattern] = metricsToDict(m)
            effectivePerPattern[pattern] = effective
        trial.set_user_attr("effectiveParams", effectivePerPattern)

        duration = time.monotonic() - trialStart
        trialDurations.append(duration)
        runIndex += 1
        avgDuration = sum(trialDurations) / len(trialDurations)
        eta = (trials - runIndex) * avgDuration
        costText = "INVALID" if invalidPatterns else f"{totalCost:.4f}"
        pr(f"[{profileName} trial {runIndex}/{trials}] (#{trial.number}) "
              f"cost={costText}  {duration:.1f}s, avg {avgDuration:.1f}s/trial, "
              f"ETA {eta / 60:.1f} min")

        try:
            with logFile.open("a") as f:
                f.write(json.dumps({
                    "trial": trial.number,
                    "cost": None if invalidPatterns else totalCost,
                    "valid": not invalidPatterns,
                    "invalidPatterns": invalidPatterns,
                    "params": params,
                    "effectiveParams": effectivePerPattern, "metrics": perPattern,
                    "durationSeconds": round(duration, 2)
                }) + "\n")
        except OSError as e:
            prWarn(f"could not append to per-trial log {logFile.name}: {e}")

        if invalidPatterns:
            # Dropped, not scored. Assigning any number here - however large - would let
            # the sampler rank blind trials against each other and "learn" that whatever
            # made the camera blind is a good region of the search space.
            invalidTrials += 1
            prWarn(f"trial #{trial.number} dropped: no valid measurement for "
                   f"{invalidPatterns} even after a retry "
                   f"({invalidTrials}/{runIndex} trial(s) invalid so far)")
            if (runIndex >= minTrialsForInvalidAbort
                    and invalidTrials / runIndex > cfg["maxInvalidTrialFraction"]):
                invalidAbort = (
                    f"{invalidTrials} of {runIndex} trial(s) on profile '{profileName}' "
                    f"produced no valid measurement (limit: "
                    f"{cfg['maxInvalidTrialFraction'] * 100:.0f}%). This study is "
                    f"measuring the camera, not the galvo - " + INVALID_MEASUREMENT_HINT)
                raise OptimizerError(invalidAbort)
            raise optuna.TrialPruned()
        return totalCost

    try:
        study = optuna.create_study(
            study_name=studyName, storage=storageUrl, load_if_exists=True,
            direction="minimize", sampler=optuna.samplers.TPESampler(seed=42))
    except Exception as e:
        raise OptimizerError(f"failed to open Optuna study storage '{storageUrl}': {e}") from e

    priorTrials = len(study.trials)
    if priorTrials:
        pr(f"resuming study '{studyName}' ({storageUrl}) - "
              f"{priorTrials} trial(s) already recorded, adding {trials} more")
        prWarn("costs recorded before v2.19.0 are NOT comparable with new ones: the cost "
               "function gained a blankCorridorLeakage term, and trials whose capture saw "
               "nothing used to be scored as if they were real readings instead of being "
               "dropped. If this study predates v2.19.0, start a fresh one (--fresh) "
               "rather than mixing the two.")
    else:
        pr(f"new study '{studyName}' -> {storageUrl}")
    pr(f"per-trial log -> {logFile.name}")

    stoppedEarly = None
    try:
        study.optimize(objective, n_trials=trials, show_progress_bar=True)
    except KeyboardInterrupt:
        if cam.liveView and cam.liveView.quitRequested:
            stoppedEarly = "'q' pressed in the camera view window"
        else:
            stoppedEarly = "interrupted by user (Ctrl+C)"
    except OptimizerError as e:
        stoppedEarly = str(e)
    finally:
        if cam.liveView:
            cam.liveView.clearProgress()
        try:
            esp.stop()
        except OptimizerError:
            pass    # best-effort cleanup - the original error/interrupt is what matters

    if invalidAbort:
        # Hard stop for the whole command, not a per-profile "stopped early": every
        # remaining profile would be measured through the same blind camera. Trials
        # recorded so far are still safely in the study storage.
        raise OptimizerError(invalidAbort)
    if invalidTrials:
        prWarn(f"{invalidTrials} of {runIndex} trial(s) on '{profileName}' were dropped "
               f"as invalid measurements - the result below rests on the "
               f"{runIndex - invalidTrials} readable one(s). If that count looks low, "
               + INVALID_MEASUREMENT_HINT)

    if stoppedEarly:
        pr(f"\noptimize stopped early: {stoppedEarly}")
        pr(f"re-run the same command (same --study-name/--storage) to continue - "
              f"{len(study.trials)} trial(s) are safely recorded so far.")
    return study, stoppedEarly


def runOptimize(cfg: dict, esp: EspClient, cam: Camera, profile: str | None, trials: int,
                studyName: str | None = None, storageUrl: str | None = None,
                fresh: bool = False, autoApply: bool = False, presetName: str | None = None):
    try:
        import optuna
    except ImportError as e:
        raise OptimizerError(
            "optuna is not installed - run: pip install -r requirements.txt"
        ) from e

    spaces = loadSearchSpaceFile()

    # One config fetch drives everything: the tunable-profile menu, each profile's
    # member presets, and the before/after baselines.
    pr("querying ESP32 for available optimizer profiles ...")
    config = esp.getConfig()
    fwProfiles = fetchFirmwareProfiles(esp, config)
    activeFw = next((p for p in fwProfiles if p["active"]), None)
    pr(f"ESP32 reports {len(fwProfiles)} optimizer profile(s)"
          + (f", currently active: {activeFw['name']}" if activeFw else "") + ".")

    if presetName:
        profile = resolveProfileForPreset(fwProfiles, presetName)
        pr(f"preset '{presetName}' -> firmware profile '{profile}'")

    selected = selectFirmwareProfiles(spaces, profile, fwProfiles)

    pr(f"\ntuning {len(selected)} profile(s), {trials} trial(s) each - "
          f"presets that will be affected:")
    for p in selected:
        members = ", ".join(p["members"]) if p["members"] else "(no presets)"
        pr(f"  {p['name']}: {members}")

    homography, background = loadHomography()
    try:
        RESULTS_DIR.mkdir(exist_ok=True)
    except OSError as e:
        raise OptimizerError(f"cannot create {RESULTS_DIR.name}/: {e}") from e
    storageUrl = storageUrl or f"sqlite:///{(RESULTS_DIR / 'optuna_study.db').as_posix()}"

    interactive = sys.stdin.isatty()
    appliedAny = False
    summary = []

    for p in selected:
        name, idx = p["name"], p["index"]
        space = validateProfileSpace(name, spaces[name])
        baseline = baselineParamsFor(config, idx)

        thisStudyName = studyName if studyName and len(selected) == 1 \
            else (f"{studyName}_{name}" if studyName else name)
        if fresh:
            thisStudyName = f"{thisStudyName}_{time.strftime('%Y%m%d_%H%M%S')}"

        pr()
        pr(bold(f"=== tuning profile '{name}' via pattern(s) {space['patterns']} ==="))
        study, stoppedEarly = runStudyForProfile(
            optuna, cfg, esp, cam, name, space, trials,
            thisStudyName, storageUrl, homography, background)

        completed = [t for t in study.trials if t.state.name == "COMPLETE"]
        if completed:
            bestParams = study.best_params
            changed, unchanged = buildChangeReport(baseline, bestParams)
            printChangeReport(name, changed, unchanged)

            result = {
                "profile": name,
                "profileIndex": idx,
                "studyName": thisStudyName,
                "storage": storageUrl,
                "bestCost": study.best_value,
                "bestParams": bestParams,
                "baseline": baseline,
                "changed": [{"param": f, "before": b, "after": a} for f, b, a in changed],
                "unchanged": [{"param": f, "value": v, "reason": r} for f, v, r in unchanged],
                "trials": trials,
                "totalTrialsInStudy": len(study.trials),
                "stoppedEarly": stoppedEarly,
                "timestamp": time.strftime("%Y-%m-%d_%H-%M-%S")
            }
            outFile = RESULTS_DIR / f"best_{name}_{result['timestamp']}.json"
            try:
                outFile.write_text(json.dumps(result, indent=2))
            except OSError as e:
                prWarn(f"could not save best-params file: {e}")
            pr()
            prOk(f"best cost {study.best_value:.4f} (over {len(completed)} completed "
                  f"trial(s)) - saved -> {outFile.name}")
            summary.append((name, study.best_value, len(changed)))

            # Visual before/after: baseline (no override, same state 'diagnose' would
            # see) vs. the trial-found best params, on the same camera pattern(s) just
            # tuned. Only worth the extra camera time when something actually changed -
            # an unchanged profile has nothing to compare.
            if changed:
                pr(f"\ncapturing before/after comparison for '{name}' ...")
                for pattern in space["patterns"]:
                    beforeM, _ = measureOnce(
                        esp, cam, cfg, homography, background, pattern,
                        statusPrefix=f"{name} BEFORE",
                        saveTo=RESULTS_DIR / f"{name}_{pattern}_before_{result['timestamp']}.png")
                    afterM, _ = measureOnce(
                        esp, cam, cfg, homography, background, pattern,
                        statusPrefix=f"{name} AFTER", params=bestParams,
                        saveTo=RESULTS_DIR / f"{name}_{pattern}_after_{result['timestamp']}.png")
                    fmt = (lambda c: "INVALID" if math.isnan(c) else f"{c:.4f}")
                    pr(f"  {pattern}: cost {fmt(beforeM.cost)} -> {fmt(afterM.cost)}  -> "
                          f"{RESULTS_DIR.name}/{name}_{pattern}_{{before,after}}_"
                          f"{result['timestamp']}[_annotated].png")
                    for side, mm in (("BEFORE", beforeM), ("AFTER", afterM)):
                        if not mm.valid:
                            prWarn(f"  {side} {formatInvalid(mm, pattern)} - this "
                                   f"comparison is not meaningful")

            # The calib-cam session restored the pre-tuning snapshot on /stop, so
            # tuned values are NOT live on the controller yet - apply explicitly.
            doApply = autoApply
            if not doApply and interactive and changed:
                doApply = askYesNo(f"\napply these {len(changed)} changed value(s) to the "
                                   f"'{name}' profile on the ESP32 now? [y/N]: ", default=False)
            if doApply and changed:
                esp.applyOptimizerLive(idx, bestParams)
                appliedAny = True
                prOk(f"applied to '{name}' (live until reboot - persisting to NVS is "
                      f"offered at the end)")
            elif changed and not interactive:
                prInfo("not applied - non-interactive session without --apply; values "
                      "are only in the results JSON")
        else:
            prWarn(f"no completed trials for '{name}' - nothing to report")

        if stoppedEarly and ("interrupted" in stoppedEarly or "'q' pressed" in stoppedEarly):
            remaining = [q["name"] for q in selected[selected.index(p) + 1:]]
            if remaining:
                prWarn(f"skipping remaining profile(s) {remaining} after interrupt")
            break

    if summary:
        pr()
        pr(bold("=== summary ==="))
        for name, cost, nChanged in summary:
            pr(f"  {name:<12} best cost {cost:.4f}, {nChanged} param(s) changed")
        orientedPatterns = {p: v["name"] for p, v in _loadOrientationCache().items()
                           if v["name"] != "identity"}
        if orientedPatterns:
            prInfo(f"orientation-compensated pattern(s): {orientedPatterns} - scan/dwell "
                 f"parameters are unaffected by this (rotation-invariant), but this profile's "
                 f"geometry checks (diagnose) are scored against a rotated/mirrored reference. "
                 f"See the warning printed when first detected.")

    if appliedAny:
        doSave = autoApply
        if not doSave and interactive:
            doSave = askYesNo("\npersist the applied values to NVS so they survive a "
                              "reboot? [y/N]: ", default=False)
        if doSave:
            esp.saveOptimizer()
            prOk("saved to NVS")
        else:
            prInfo("not persisted - applied values are live until the ESP32 reboots")


# ── diagnose ─────────────────────────────────────────────────────────────────

def classifyProfile(name: str, patterns: list[str], allMetrics: dict[str, Metrics],
                    thresholds: dict) -> tuple[str, list[str], list[str]]:
    """Checks one profile's measured Metrics (one per camera pattern) against
    diagnoseThresholds. Returns (verdict, geometryIssues, settingsIssues).
    Geometry issues take priority: retuning scan/dwell parameters cannot fix a
    shape that's the wrong size or in the wrong place - that's galvo gain/offset
    calibration (or a moved camera/surface), so autotune is only offered when
    geometry is clean but the settings-related metrics are still out of tolerance."""
    geometryIssues, settingsIssues = [], []
    invalidIssues: list[str] = []
    for pattern in patterns:
        m = allMetrics[pattern]
        if not m.valid:
            # Not classified at all: every number below would be a fallback constant,
            # and reporting "path deviation 60000 DAC units" as a settings issue would
            # send the operator tuning the galvo over a camera problem.
            invalidIssues.append(formatInvalid(m, pattern))
            continue
        if abs(m.scaleErrorXPct) > thresholds["geometryScalePct"]:
            geometryIssues.append(f"{pattern}: X size off by {m.scaleErrorXPct:+.1f}% vs. ideal")
        if abs(m.scaleErrorYPct) > thresholds["geometryScalePct"]:
            geometryIssues.append(f"{pattern}: Y size off by {m.scaleErrorYPct:+.1f}% vs. ideal")
        if abs(m.offsetXUnits) > thresholds["geometryOffsetUnits"]:
            geometryIssues.append(f"{pattern}: X position off by {m.offsetXUnits:+.0f} DAC units")
        if abs(m.offsetYUnits) > thresholds["geometryOffsetUnits"]:
            geometryIssues.append(f"{pattern}: Y position off by {m.offsetYUnits:+.0f} DAC units")
        # The path-deviation bar is derived from THIS measurement's own beam width, not
        # a flat number: on any real rig the RMS distance from the ideal path is
        # dominated by how wide the beam images, which is a property of the optics and
        # the camera, not of any scan parameter. See diagnoseThresholds' comment.
        pathBar = max(thresholds.get("pathDeviationRmsMinUnits", 0.0),
                      thresholds.get("pathDeviationBeamWidthFactor", 0.0) * m.beamWidthUnits)
        if m.pathDeviationRms > pathBar:
            settingsIssues.append(f"{pattern}: path deviation {m.pathDeviationRms:.1f} DAC units "
                                  f"(threshold {pathBar:.0f} = "
                                  f"{thresholds.get('pathDeviationBeamWidthFactor', 0.0):.1f} x "
                                  f"measured beam width {m.beamWidthUnits:.0f}u, floor "
                                  f"{thresholds.get('pathDeviationRmsMinUnits', 0.0):.0f})")
        if m.blankLeakage > thresholds["blankLeakage"]:
            settingsIssues.append(f"{pattern}: blank leakage {m.blankLeakage:.1f} "
                                  f"(threshold {thresholds['blankLeakage']})")
        if m.blankCorridorLeakage > thresholds.get("blankCorridorLeakage", float("inf")):
            settingsIssues.append(
                f"{pattern}: blank corridor leakage {m.blankCorridorLeakage:.2f} "
                f"(threshold {thresholds['blankCorridorLeakage']}), "
                f"{m.blankCorridorLitPx} lit px in regions that should be dark, "
                f"brightest {m.blankCorridorMaxVal:.0f}")
        if m.cornerHotspot > thresholds["cornerHotspot"]:
            settingsIssues.append(f"{pattern}: corner hotspot ratio {m.cornerHotspot:.2f} "
                                  f"(threshold {thresholds['cornerHotspot']})")
        if m.brightnessNonUniformity > thresholds["brightnessNonUniformity"]:
            settingsIssues.append(f"{pattern}: brightness non-uniformity "
                                  f"{m.brightnessNonUniformity:.2f} "
                                  f"(threshold {thresholds['brightnessNonUniformity']})")
        # Isolated-dot checks: only meaningful where there ARE ideal dots.
        if m.blobExpected > 0:
            if m.blobElongation > thresholds.get("blobElongation", float("inf")):
                settingsIssues.append(
                    f"{pattern}: dots are smeared - mean elongation {m.blobElongation:.2f} "
                    f"(threshold {thresholds['blobElongation']}), worst "
                    f"{m.blobElongationMax:.2f}; the blank window is too short for the "
                    f"jump distance")
            if m.blobCountError > thresholds.get("blobCountError", float("inf")):
                settingsIssues.append(
                    f"{pattern}: saw {m.blobCount} dots, expected {m.blobExpected} - "
                    f"dots merging into streaks or dropping out entirely")
            if m.blobCentroidErrorUnits > thresholds.get("blobCentroidErrorUnits", float("inf")):
                settingsIssues.append(
                    f"{pattern}: dots land {m.blobCentroidErrorUnits:.0f} DAC units from "
                    f"their target on average (threshold "
                    f"{thresholds['blobCentroidErrorUnits']:.0f}) - the beam has not "
                    f"arrived when the laser re-arms")
        if m.saturationFrac > thresholds["saturationFrac"]:
            settingsIssues.append(
                f"{pattern}: {m.saturationFrac * 100:.0f}% of the traced beam is raw-sensor-"
                f"saturated (threshold {thresholds['saturationFrac'] * 100:.0f}%) - likely "
                f"camera blooming inflating path deviation/corner hotspot, not a real scan/"
                f"dwell problem; try 'autotune-camera' before 'optimize' for this one")
    if invalidIssues:
        # Outranks everything: with a blind capture there is no verdict to give, and
        # saying "GEOMETRY ISSUE" or "OK" here would both be inventions.
        verdict = "INVALID MEASUREMENT"
    elif geometryIssues:
        verdict = "GEOMETRY ISSUE"
    elif settingsIssues:
        verdict = "OPTIMIZER SETTINGS ISSUE"
    else:
        verdict = "OK"
    return verdict, geometryIssues, settingsIssues + invalidIssues


def printDiagnosis(name: str, verdict: str, geometryIssues: list[str], settingsIssues: list[str],
                   calib: dict | None = None):
    pr()
    pr(f"{bold(name)}: {bold(verdict)}")
    for g in geometryIssues:
        pr(f"  [geometry] {g}")
    for s in settingsIssues:
        # Invalid-measurement entries are carried in the same list (see classifyProfile's
        # return) but are not settings problems - they're already self-labelling.
        pr(f"  {s}" if s.startswith("INVALID MEASUREMENT") else f"  [settings] {s}")
    if verdict == "INVALID MEASUREMENT":
        prWarn(f"'{name}' was NOT diagnosed - " + INVALID_MEASUREMENT_HINT)
    elif verdict == "GEOMETRY ISSUE":
        prTip("not fixable by autotune. Re-run 'calibrate' (camera/projection surface "
              "may have moved); if it persists, suspect galvo gain/offset/DAC-range "
              "calibration drift - current live values, for reference:")
        if calib:
            prTable([
                ("gain X / Y",   f"{calib['gain_x']} / {calib['gain_y']}"),
                ("offset X / Y", f"{calib['offset_x']} / {calib['offset_y']}"),
                ("dac_limit",    f"[{calib['dac_limit_min']}, {calib['dac_limit_max']}]"),
            ], headers=("galvo calib", "value"))
        else:
            prInfo("could not read - GET /api/config missing expected fields")
    elif verdict == "OK":
        pr("  -> within tolerance, no action needed.")


def runDiagnose(cfg: dict, esp: EspClient, cam: Camera, profile: str | None,
                autotune: bool, trials: int, studyName: str | None,
                storageUrl: str | None, autoApply: bool):
    """Measures each selected profile's currently-live output (no parameter override -
    whatever is actually configured right now) and classifies it as OK, a geometry
    problem, or an optimizer-settings problem (see classifyProfile). Settings problems
    can be handed straight off to the existing 'optimize' flow as an autotune step;
    geometry problems can't, so those are only ever reported, never auto-"fixed"."""
    spaces = loadSearchSpaceFile()
    homography, background = loadHomography()

    pr("querying ESP32 for available optimizer profiles ...")
    config = esp.getConfig()
    fwProfiles = fetchFirmwareProfiles(esp, config)
    selected = selectFirmwareProfiles(spaces, profile, fwProfiles)
    thresholds = cfg["diagnoseThresholds"]

    # A "GEOMETRY ISSUE" verdict is only ever a guess at the cause (gain/offset/
    # DAC-range drift vs. a moved camera/surface vs. a stale calibrate) - this tool
    # has no way to tell those apart from the photo alone. Surfacing the actual
    # live NVS values lets you at least sanity-check the guess against reality
    # instead of taking the message on faith.
    calibKeys = ("galvo_x_gain", "galvo_y_gain", "galvo_x_offset", "galvo_y_offset",
                "dac_limit_min", "dac_limit_max")
    calib = ({"gain_x": config["galvo_x_gain"], "gain_y": config["galvo_y_gain"],
             "offset_x": config["galvo_x_offset"], "offset_y": config["galvo_y_offset"],
             "dac_limit_min": config["dac_limit_min"], "dac_limit_max": config["dac_limit_max"]}
            if all(k in config for k in calibKeys) else None)

    pr(f"\ndiagnosing {len(selected)} profile(s) against currently-live parameters "
         f"(no overrides applied) ...")

    timestamp = time.strftime("%Y-%m-%d_%H-%M-%S")
    try:
        RESULTS_DIR.mkdir(exist_ok=True)
    except OSError as e:
        prWarn(f"could not create {RESULTS_DIR.name}/ for diagnosis screenshots: {e}")

    results = []    # (name, verdict, geometryIssues, settingsIssues)
    flagged = []    # firmware-profile dicts flagged OPTIMIZER SETTINGS ISSUE
    try:
        for p in selected:
            name = p["name"]
            space = validateProfileSpace(name, spaces[name])
            allMetrics = {}
            for pattern in space["patterns"]:
                m, _ = measureOnce(esp, cam, cfg, homography, background, pattern,
                                   statusPrefix=f"diagnose {name}",
                                   saveTo=RESULTS_DIR / f"diagnose_{name}_{pattern}_{timestamp}.png")
                allMetrics[pattern] = m
            verdict, geometryIssues, settingsIssues = classifyProfile(
                name, space["patterns"], allMetrics, thresholds)
            results.append((name, verdict, geometryIssues, settingsIssues))
            if verdict == "OPTIMIZER SETTINGS ISSUE":
                flagged.append(p)
    finally:
        try:
            esp.stop()
        except OptimizerError:
            pass    # best-effort - a genuine failure here would already have raised above

    pr()
    pr(bold("=== diagnosis ==="))
    pr(f"(measured against the fixed camera-calibration geometry, DAC range "
         f"+-{cfg['dacRange']:.0f} - a profile's tuning is only verified at that size, "
         f"not necessarily at whatever 'Size' a live preset actually runs at)")
    prOk(f"annotated screenshots (ideal path vs. traced beam vs. deviation) saved -> "
         f"{RESULTS_DIR.name}/diagnose_<profile>_<pattern>_{timestamp}_annotated.png")
    orientedPatterns = {p: v["name"] for p, v in _loadOrientationCache().items()
                        if v["name"] != "identity"}
    if orientedPatterns:
        prInfo(f"orientation-compensated pattern(s) this run: {orientedPatterns} - "
             f"geometry metrics for these are scored against a rotated/mirrored reference "
             f"(see the warning printed when each was first detected, and the annotated "
             f"screenshots); this does not confirm the firmware output itself is correctly "
             f"oriented, only that this tool's measurement accounts for the mismatch.")
    for name, verdict, geometryIssues, settingsIssues in results:
        printDiagnosis(name, verdict, geometryIssues, settingsIssues, calib)

    invalidNames = [n for n, v, _, _ in results if v == "INVALID MEASUREMENT"]
    if invalidNames:
        pr()
        prWarn(f"{len(invalidNames)} of {len(results)} profile(s) could not be diagnosed "
               f"at all: {invalidNames}. Fix the capture before trusting ANY number from "
               f"this run - " + INVALID_MEASUREMENT_HINT)

    if not flagged:
        pr()
        if invalidNames:
            prWarn("no profile flagged for autotune - but see the invalid measurement(s) "
                   "above; this is not a clean bill of health.")
        else:
            prOk("no profile needs autotune.")
        return

    flaggedNames = [p["name"] for p in flagged]
    pr()
    prWarn(f"{len(flagged)} profile(s) flagged for autotune: {flaggedNames}")
    doAutotune = autotune
    if not doAutotune and sys.stdin.isatty():
        doAutotune = askYesNo("run autotune ('optimize') on these now? [y/N]: ", default=False)
    if doAutotune:
        runOptimize(cfg, esp, cam, ",".join(flaggedNames), trials, studyName=studyName,
                   storageUrl=storageUrl, autoApply=autoApply)
    else:
        prTip(f"not autotuning - re-run with --autotune, or "
              f"'optimizeGalvo.py optimize --profile {','.join(flaggedNames)}'")


# Commands that call esp.startPattern() at some point and therefore need the laser
# actually armed and no interlock tripped to produce a meaningful capture - checked
# up front by requireLaserReady() so a not-ready controller fails fast with a clear
# message instead of surfacing later as "expected 4 dots but found 0" deep in
# detectDots(), or as bogus all-zero/garbage metrics. 'preview' doesn't touch the
# ESP32 at all; 'analyze-live' never starts/stops a pattern by design and does its
# own non-blocking version of this same check inline (see runAnalyzeLive).
LASER_REQUIRED_CMDS = ("calibrate", "measure", "optimize", "diagnose", "autotune-camera",
                      "autotune-colors", "calibrate-warp", "measure-resonance",
                      "tune-dac-range")


def requireLaserReady(esp: EspClient):
    """Raises OptimizerError if the controller isn't actually ready to project
    (E-Stop/scan-fail tripped, or laser not armed) - interactive sessions are asked
    whether to proceed anyway (e.g. bench-testing the capture/scoring pipeline with
    no live beam); non-interactive sessions abort outright, since silently continuing
    would just burn a settle+capture cycle on a blank frame and report misleading
    (or outright wrong) metrics."""
    try:
        status = esp.getStatus()
    except OptimizerError as e:
        raise OptimizerError(
            f"could not read controller status before starting a pattern: {e}"
        ) from e

    problems = []
    if not status.get("estop_ok"):
        problems.append("E-Stop is tripped")
    if not status.get("scanfail_ok"):
        problems.append("scan-fail interlock is tripped")
    if not status.get("laser_armed"):
        problems.append("laser is NOT armed")
    if not problems:
        return

    prWarn("controller reports it is not ready to project: " + "; ".join(problems) + ".")
    prTip("arm the laser and/or clear the interlock (controller panel or WebUI), then "
          "re-run - 'optimizeGalvo.py check' shows the full interlock state.")
    if sys.stdin.isatty() and askYesNo(
            "continue anyway (capture will most likely be blank/meaningless)? [y/N]: ",
            default=False):
        return
    raise OptimizerError("aborting: controller not ready to project (see warning above)")


# ── connection check ────────────────────────────────────────────────────────

def runCheckConnection(cfg: dict, esp: EspClient) -> bool:
    """Reachability/identity check against the ESP32. No camera required."""
    prInfo(f"checking {esp.baseUrl} (timeout {esp.timeout}s) ...")
    try:
        status = esp.getStatus()
    except OptimizerError as e:
        prWarn(f"FAILED: {e}")
        return False

    prOk("controller reachable")
    fwVersionStr = status.get("fw_version", "?")
    prTable([
        ("fw_version",   fwVersionStr),
        ("hostname / ip", f"{status.get('hostname', '?')} / {status.get('ip', '?')}"),
        ("rssi",         f"{status.get('rssi', '?')} dBm"),
        ("uptime_s",     status.get("uptime_s", "?")),
        ("free_heap",    f"{status.get('free_heap', '?')} B"),
        ("free_psram",   f"{status.get('free_psram', '?')} B"),
        ("estop_ok",     bool(status.get("estop_ok"))),
        ("scanfail_ok",  bool(status.get("scanfail_ok"))),
        ("laser_armed",  bool(status.get("laser_armed"))),
        ("debug_mode",   bool(status.get("debug_mode"))),
    ], headers=("field", "value"))
    if not status.get("estop_ok") or not status.get("scanfail_ok"):
        prWarn("a safety interlock is currently tripped - "
              "calib-cam patterns will not project until cleared.")

    fwVersion = parseFwVersion(fwVersionStr)
    minStr = ".".join(map(str, MIN_FW_VERSION_CALIB_CAM))
    if fwVersion is None:
        prInfo(f"could not parse fw_version '{fwVersionStr}' - unable to check "
              f"calib-cam API support (needs >= v{minStr})")
    elif fwVersion < MIN_FW_VERSION_CALIB_CAM:
        prWarn(f"firmware v{fwVersionStr} predates the calib-cam API "
              f"(added in v{minStr}). 'calibrate'/'measure'/'optimize' will fail with "
              f"404 until the ESP32 firmware is updated.")
    return True


# ── first-time setup wizard ──────────────────────────────────────────────────

def probeCameras(maxIndex: int = 4, backend: int = cv2.CAP_DSHOW) -> list[int]:
    """Best-effort scan for openable camera indices 0..maxIndex."""
    found = []
    for i in range(maxIndex + 1):
        cap = cv2.VideoCapture(i, backend)
        if cap.isOpened():
            found.append(i)
        cap.release()
    return found


def runWizard(existingCfg: dict | None = None) -> dict:
    """Interactively (re-)creates camConfig.json. Enter accepts the shown default."""
    pr("=== GalvOS camera-in-the-loop optimizer - setup wizard ===")
    pr("Press Enter to accept the default shown in [brackets].\n")

    cfg = dict(existingCfg) if existingCfg else dict(DEFAULT_CONFIG)

    def ask(key: str, prompt: str, cast=str):
        current = cfg.get(key, DEFAULT_CONFIG[key])
        if len(prompt) + len(str(current)) > TERM_WIDTH:
            pr(prompt)
            raw = input(f"[{current}]: ").strip()
        else:
            raw = input(f"{prompt} [{current}]: ").strip()
        if not raw:
            cfg[key] = current
            return
        try:
            cfg[key] = cast(raw)
        except ValueError:
            pr(f"  invalid value '{raw}', keeping {current}")
            cfg[key] = current

    ask("esp32BaseUrl", "ESP32 controller base URL (mDNS hostname or IP, e.g. "
                        "http://galvos.local or http://192.168.1.50)")

    pr("\nscanning for cameras (DirectShow, indices 0-4) ...")
    found = probeCameras()
    if found:
        pr(f"  found camera index(es): {found}")
    else:
        pr("  no camera detected - plug it in, you can still finish setup and "
              "re-run 'optimizeGalvo.py wizard' later")
    ask("cameraIndex", "camera device index", int)
    ask("frameWidth", "camera frame width", int)
    ask("frameHeight", "camera frame height", int)
    ask("exposure", "camera exposure (DirectShow log2 scale, negative = shorter, "
                    "e.g. -11 ~= 1/2048s)", int)
    ask("dacRange", "DAC reference range (+-) for calibration corner dots", int)
    ask("camPatternChannel", "calib-cam pattern color (0=white 1=R 2=G 3=B)", int)
    ask("requestTimeoutSeconds", "ESP32 HTTP request timeout (seconds)", float)
    ask("requestRetries", "extra retries on ESP32 timeout/connection error before giving "
                         "up (helps with transient WiFi hiccups during long optimize runs)",
       int)
    currentShowView = cfg.get("showCameraView", DEFAULT_CONFIG["showCameraView"])
    cfg["showCameraView"] = askYesNo(
        f"show a live camera view window during calibrate/measure/optimize (y/n) "
        f"[{'y' if currentShowView else 'n'}]: ", default=currentShowView)

    try:
        CONFIG_FILE.write_text(json.dumps(cfg, indent=2))
    except OSError as e:
        raise OptimizerError(f"cannot write {CONFIG_FILE}: {e}") from e
    pr()
    prOk(f"saved -> {CONFIG_FILE.name}")
    prInfo(f"costWeights and other advanced settings were left at their current "
          f"values - edit {CONFIG_FILE.name} directly for those")

    if askYesNo("\ntest connection to the controller now? [Y/n]: ", default=True):
        esp = EspClient.fromConfig(cfg)
        runCheckConnection(cfg, esp)

    return cfg


# ── preview ──────────────────────────────────────────────────────────────────

PREVIEW_WINDOW_NAME = "OV9281 preview"


def runPreview(cfg: dict, cam: Camera, zoomIdx: int = 0):
    exposure = cfg["exposure"]
    liveView = LiveView(
        PREVIEW_WINDOW_NAME, cfg["frameWidth"], cfg["frameHeight"], zoomIdx=zoomIdx,
        hotkeys="[1/2/3] zoom   [+/-] exposure (auto-saved)   [s] screenshot   "
                "[space] pause   [q] quit")
    pr("preview: " + liveView.hotkeys)
    # Wire the window into cam so grabGray()'s own rolling max-smoothing (displaySmoothFrames)
    # drives it - without this, cam.liveView stays None (preview isn't in main()'s viewCmds)
    # and the loop below fell back to displaying/saving a single raw frame, which at high
    # fps/short exposure often only catches part of a fast-scanned pattern (that's what the
    # smoothing exists to fix - see displaySmoothFrames in DEFAULT_CONFIG).
    cam.liveView = liveView
    lastFrame = cam.grabGray()
    try:
        while not liveView.quitRequested:
            if liveView.paused:
                # redisplay the frozen last frame instead of grabbing a new one -
                # this is the one context where "pause" means "freeze the image"
                # rather than "safe to keep the laser dark", since there's no
                # measurement/laser session running here to protect either way
                liveView.update(lastFrame, "PAUSED")
                continue
            frame = cam.grabGray()
            lastFrame = frame
            saturated = float(np.mean(frame >= 250)) * 100
            cam.statusText = f"preview: exp {exposure}  sat {saturated:.1f}%  max {frame.max()}"
            key = liveView.lastKey
            if key in (ord("+"), ord("-")):
                exposure += 1 if key == ord("+") else -1
                cam.cap.set(cv2.CAP_PROP_EXPOSURE, exposure)
                cfg["exposure"] = exposure
                try:
                    CONFIG_FILE.write_text(json.dumps(cfg, indent=2))
                    prOk(f"exposure {exposure} -> saved to {CONFIG_FILE.name}")
                except OSError as e:
                    prWarn(f"exposure {exposure} (could not save {CONFIG_FILE.name}: {e})")
    finally:
        cam.liveView = None
        liveView.close()


# ── camera autotune ──────────────────────────────────────────────────────────

CAMERA_AUTOTUNE_PATTERNS = ("square", "star", "segments", "circle", "spiral",
                            "wireframe", "text", "particles")

# Default pattern set for 'autotune-camera': square gives corner-hotspot coverage,
# circle gives pure path/uniformity with no straight edges or corners, segments
# gives blank-leakage - between the three, every metric computeMetrics reports is
# exercised at least once without needing all five calib patterns every trial.
DEFAULT_CAMERA_AUTOTUNE_PATTERNS = ("square", "circle", "segments")


def _saturationFraction(image: np.ndarray, satLevel: int = 250) -> float:
    return float(np.mean(image >= satLevel))


def runAutotuneCamera(cfg: dict, esp: EspClient, cam: Camera, patterns: list[str],
                      trials: int, studyName: str | None, storageUrl: str | None,
                      fresh: bool, autoApply: bool):
    """Optuna search over the camera-capture knobs that can be changed trial-to-trial
    without reopening the device: exposure, gain, binaryThreshold, and accumFrames
    (bounds in cameraAutotuneRanges, camConfig.json). frameWidth/frameHeight/cameraFps/
    cameraBackend are negotiated once when the camera is opened, so they aren't in
    scope here - use the wizard for those. Firmware scan/dwell parameters are left
    exactly as currently live (no /params override): this isolates capture quality
    only, not pattern tuning - that's what 'optimize' is for.

    Scored with the same path-deviation/blank-leakage/corner-hotspot/brightness-
    uniformity cost 'optimize' uses, plus four cameraAutotuneWeights penalties those
    metrics can't express on their own:
      saturation / backgroundBrightness - a washed-out (overexposed) capture would
        otherwise look perfectly "uniform"
      pathCoverage / offPathNoise / blindCapture - the signal-vs-noise criterion. The
        core metrics get BETTER as the capture goes blind (a higher binaryThreshold
        keeps only the brightest core pixels, shrinking pathDeviationRms and improving
        brightnessNonUniformity), so without these the search is rewarded for blinding
        the camera. That is exactly how this rig ended up at binaryThreshold=133, five
        lit pixels on the whole canvas. The chosen settings are also re-measured at the
        end and refused if they don't produce a valid measurement.

    Background is re-captured fresh every trial (laser off) at that trial's
    exposure/gain - a background from a different exposure would corrupt the
    diff-subtraction computeMetrics relies on. Only homography.npz's matrix is
    reused (exposure-independent, since it's a pure geometric mapping); its stored
    background is refreshed on apply, see below."""
    try:
        import optuna
    except ImportError as e:
        raise OptimizerError(
            "optuna is not installed - run: pip install -r requirements.txt"
        ) from e

    badPatterns = [p for p in patterns if p not in CAMERA_AUTOTUNE_PATTERNS]
    if badPatterns:
        raise OptimizerError(
            f"--patterns has unknown pattern(s) {badPatterns} - choices: "
            f"{list(CAMERA_AUTOTUNE_PATTERNS)}"
        )

    homography, _ = loadHomography()
    try:
        RESULTS_DIR.mkdir(exist_ok=True)
    except OSError as e:
        raise OptimizerError(f"cannot create {RESULTS_DIR.name}/: {e}") from e

    ranges = cfg["cameraAutotuneRanges"]
    weights = cfg["cameraAutotuneWeights"]
    baseline = {"exposure": cfg["exposure"], "gain": cfg["gain"],
               "binaryThreshold": cfg["binaryThreshold"], "accumFrames": cfg["accumFrames"]}

    storageUrl = storageUrl or f"sqlite:///{(RESULTS_DIR / 'optuna_study.db').as_posix()}"
    studyName = studyName or "camera_autotune"
    if fresh:
        studyName = f"{studyName}_{time.strftime('%Y%m%d_%H%M%S')}"

    logFile = RESULTS_DIR / f"autotune_camera_{time.strftime('%Y-%m-%d_%H-%M-%S')}.jsonl"
    trialDurations: list[float] = []
    runIndex = 0

    def objective(trial):
        nonlocal runIndex
        trialStart = time.monotonic()
        if cam.liveView:
            cam.liveView.setProgress(runIndex + 1, trials)

        exposure = trial.suggest_int("exposure", ranges["exposureMin"], ranges["exposureMax"])
        # Only searched if this camera actually has a working Gain control (see
        # Camera.gainSupported) - suggesting a dimension that never affects the capture
        # would just dilute Optuna's sampling of the params that do something.
        gain = (trial.suggest_int("gain", ranges["gainMin"], ranges["gainMax"])
                if cam.gainSupported else baseline["gain"])
        binaryThreshold = trial.suggest_int("binaryThreshold", ranges["binaryThresholdMin"],
                                            ranges["binaryThresholdMax"])
        accumFrames = trial.suggest_int("accumFrames", ranges["accumFramesMin"],
                                        ranges["accumFramesMax"])
        trialCfg = {**cfg, "binaryThreshold": binaryThreshold, "accumFrames": accumFrames}

        cam.setExposureGain(exposure, gain)
        waitWhilePaused(cam)   # safe boundary: no pattern is running yet
        esp.stop()
        time.sleep(cfg["settleSeconds"])
        cam.statusText = f"autotune-camera trial {runIndex + 1}/{trials}: background"
        background = cam.grabBackground()
        backgroundBrightness = float(np.mean(background)) / 255.0

        totalCost = 0.0
        saturationFrac = 0.0
        perPattern = {}
        blindPatterns = []
        coverageSum = 0.0
        offPathSum = 0.0
        for patternIdx, pattern in enumerate(patterns):
            # Exactly one pattern per trial gets saved (raw + measureOnce's own
            # "_annotated" companion = 2 files/trial, not 2 per pattern) - enough to
            # spot-check capture quality and pattern-switch bleed-through trial over
            # trial without results/ filling up with every pattern of every trial.
            saveTo = (RESULTS_DIR / f"autotune_camera_trial{trial.number:04d}_{pattern}.png"
                     if patternIdx == 0 else None)
            m, _ = measureOnce(
                esp, cam, trialCfg, homography, background, pattern, saveTo=saveTo,
                statusPrefix=f"autotune-camera trial {runIndex + 1}/{trials}")

            # ── signal-vs-noise criterion ────────────────────────────────────────
            # Without this, the search is actively rewarded for blinding the camera:
            # a higher binaryThreshold keeps only the brightest core pixels, which
            # SHRINKS pathDeviationRms and improves brightnessNonUniformity. That is
            # how this rig ended up configured at binaryThreshold=133 - five lit
            # pixels on the whole canvas, scoring better than the usable 50.
            # Coverage rewards seeing more of the ideal path; off-path noise stops
            # coverage from being bought by dropping into the sensor noise floor.
            coverageTerm = (weights.get("pathCoverage", 0.0)
                            * (1.0 - m.pathCoveragePct / 100.0))
            offPathTerm = (weights.get("offPathNoise", 0.0)
                           * (m.offPathLitPx / max(m.traceLitPx, 1)))
            coverageSum += coverageTerm
            offPathSum += offPathTerm
            totalCost += coverageTerm + offPathTerm
            if m.valid:
                totalCost += m.cost
            else:
                # m.cost is NaN and the path/uniformity metrics behind it are fallback
                # constants - adding them would be scoring a non-measurement. A flat
                # penalty instead, big enough to wall off the whole blind region.
                blindPatterns.append(pattern)
                totalCost += weights.get("blindCapture", 0.0)
            saturationFrac = max(saturationFrac, _saturationFraction(cam.lastAccumulated))
            trial.set_user_attr(f"metrics_{pattern}", metricsToDict(m))
            perPattern[pattern] = metricsToDict(m)

        totalCost += weights["saturation"] * saturationFrac
        totalCost += weights["backgroundBrightness"] * backgroundBrightness

        duration = time.monotonic() - trialStart
        trialDurations.append(duration)
        runIndex += 1
        avgDuration = sum(trialDurations) / len(trialDurations)
        eta = (trials - runIndex) * avgDuration
        pr(f"[camera-autotune trial {runIndex}/{trials}] (#{trial.number}) "
              f"exp={exposure} gain={gain} thr={binaryThreshold} frames={accumFrames} "
              f"cost={totalCost:.4f} sat={saturationFrac:.3f} bg={backgroundBrightness:.3f} "
              f"cover={coverageSum:.2f} offpath={offPathSum:.2f}"
              + (f" BLIND{blindPatterns}" if blindPatterns else "") +
              f"  {duration:.1f}s, avg {avgDuration:.1f}s/trial, ETA {eta / 60:.1f} min")

        try:
            with logFile.open("a") as f:
                f.write(json.dumps({
                    "trial": trial.number, "cost": totalCost,
                    "params": {"exposure": exposure, "gain": gain,
                              "binaryThreshold": binaryThreshold, "accumFrames": accumFrames},
                    "blindPatterns": blindPatterns,
                    "coveragePenalty": round(coverageSum, 4),
                    "offPathPenalty": round(offPathSum, 4),
                    "saturationFrac": saturationFrac, "backgroundBrightness": backgroundBrightness,
                    "metrics": perPattern, "durationSeconds": round(duration, 2)
                }) + "\n")
        except OSError as e:
            prWarn(f"could not append to per-trial log {logFile.name}: {e}")
        return totalCost

    try:
        study = optuna.create_study(
            study_name=studyName, storage=storageUrl, load_if_exists=True,
            direction="minimize", sampler=optuna.samplers.TPESampler(seed=42))
    except Exception as e:
        raise OptimizerError(f"failed to open Optuna study storage '{storageUrl}': {e}") from e

    priorTrials = len(study.trials)
    if priorTrials:
        pr(f"resuming study '{studyName}' ({storageUrl}) - "
              f"{priorTrials} trial(s) already recorded, adding {trials} more")
        prWarn("costs recorded before v2.19.0 are NOT comparable with new ones: the cost "
               "function gained a blankCorridorLeakage term, and trials whose capture saw "
               "nothing used to be scored as if they were real readings instead of being "
               "dropped. If this study predates v2.19.0, start a fresh one (--fresh) "
               "rather than mixing the two.")
    else:
        pr(f"new study '{studyName}' -> {storageUrl}")
    pr(f"tuning camera capture settings via pattern(s) {list(patterns)} - firmware "
         f"parameters are left exactly as currently live")
    pr(f"per-trial log -> {logFile.name}")

    stoppedEarly = None
    try:
        study.optimize(objective, n_trials=trials, show_progress_bar=True)
    except KeyboardInterrupt:
        if cam.liveView and cam.liveView.quitRequested:
            stoppedEarly = "'q' pressed in the camera view window"
        else:
            stoppedEarly = "interrupted by user (Ctrl+C)"
    except OptimizerError as e:
        stoppedEarly = str(e)
    finally:
        if cam.liveView:
            cam.liveView.clearProgress()
        try:
            esp.stop()
        except OptimizerError:
            pass    # best-effort cleanup - the original error/interrupt is what matters

    if stoppedEarly:
        pr(f"\nautotune-camera stopped early: {stoppedEarly}")
        pr(f"re-run the same command (same --study-name/--storage) to continue - "
              f"{len(study.trials)} trial(s) are safely recorded so far.")

    completed = [t for t in study.trials if t.state.name == "COMPLETE"]
    if not completed:
        pr("no completed trials - nothing to report")
        cam.setExposureGain(baseline["exposure"], baseline["gain"])
        return

    # study.best_params only contains "gain" if it was actually suggest_int'd in some
    # trial (i.e. cam.gainSupported was true) - fall back to the fixed baseline otherwise
    # so this camera's gain-free run doesn't KeyError, and so 'apply' below has a value.
    best = {**baseline, **study.best_params}

    # A resumed study can still hand back a "best" trial that was sampled under a
    # WIDER/different cameraAutotuneRanges from a previous run - Optuna's best_params
    # only reflects what scored lowest historically, it never re-checks that against
    # the range this run was actually called with. Concretely: this is exactly how a
    # camConfig.json ended up with "exposure": -14 even after exposureMin was corrected
    # to this camera's real floor of -13 - an old trial from before that fix was still
    # sitting in optuna_study.db as the recorded best, and got replayed straight back
    # out. Clamp to the CURRENT bounds and say so, rather than silently re-applying a
    # value this run's own search space says isn't even reachable.
    clampBounds = {"exposure": (ranges["exposureMin"], ranges["exposureMax"]),
                  "gain": (ranges["gainMin"], ranges["gainMax"]),
                  "binaryThreshold": (ranges["binaryThresholdMin"], ranges["binaryThresholdMax"]),
                  "accumFrames": (ranges["accumFramesMin"], ranges["accumFramesMax"])}
    for field, (lo, hi) in clampBounds.items():
        clamped = max(lo, min(hi, best[field]))
        if clamped != best[field]:
            prWarn(f"best {field}={best[field]} is outside the current cameraAutotuneRanges "
                  f"[{lo}, {hi}] (likely a stale trial from before the range was last changed, "
                  f"replayed from the resumed study) - clamped to {clamped}")
            best[field] = clamped
    pr()
    pr(bold("=== camera autotune: parameter changes (before -> after) ==="))
    for field in ("exposure", "gain", "binaryThreshold", "accumFrames"):
        before, after = baseline[field], best[field]
        if field == "gain" and not cam.gainSupported:
            pr(f"  {field:<16} {before:>8} (not supported by this camera - left unchanged)")
            continue
        marker = "" if before == after else "  (changed)"
        pr(f"  {field:<16} {before:>8} -> {after}{marker}")
    pr(f"best cost {study.best_value:.4f} (over {len(completed)} completed trial(s))")

    result = {
        "bestCost": study.best_value, "bestParams": best, "baseline": baseline,
        "studyName": studyName, "storage": storageUrl, "trials": trials,
        "totalTrialsInStudy": len(study.trials), "stoppedEarly": stoppedEarly,
        "timestamp": time.strftime("%Y-%m-%d_%H-%M-%S")
    }
    outFile = RESULTS_DIR / f"best_camera_{result['timestamp']}.json"
    try:
        outFile.write_text(json.dumps(result, indent=2))
    except OSError as e:
        prWarn(f"could not save best-params file: {e}")
    prOk(f"saved -> {outFile.name}")

    # Final gate: actually measure once at the chosen settings and check the result is a
    # readable measurement, not a blind one. The search's own penalties should already
    # make this impossible, but this is the value that gets written to camConfig.json and
    # then silently used by every later measure/optimize/diagnose run - the last place it
    # can be caught is here, cheaply, with the camera still open.
    pr("\nverifying the chosen settings produce a readable measurement ...")
    verifyOk = True
    try:
        cam.setExposureGain(best["exposure"], best["gain"])
        verifyCfg = {**cfg, "binaryThreshold": best["binaryThreshold"],
                     "accumFrames": best["accumFrames"]}
        waitWhilePaused(cam)
        esp.stop()
        time.sleep(cfg["settleSeconds"])
        verifyBackground = cam.grabBackground()
        for pattern in patterns:
            vm, _ = measureOnce(esp, cam, verifyCfg, homography, verifyBackground, pattern,
                                statusPrefix="autotune-camera VERIFY")
            if vm.valid:
                prOk(f"  {pattern}: {vm.traceLitPx} lit px, path coverage "
                     f"{vm.pathCoveragePct:.1f}%, off-path {vm.offPathLitPx} px")
            else:
                verifyOk = False
                prWarn(f"  {formatInvalid(vm, pattern)}")
    except OptimizerError as e:
        verifyOk = False
        prWarn(f"verification capture failed: {e}")
    if not verifyOk:
        prWarn("the chosen camera settings do NOT produce a usable measurement - refusing "
               "to offer them for apply. " + INVALID_MEASUREMENT_HINT)
        prTip(f"widen cameraAutotuneRanges in {CONFIG_FILE.name} (especially "
              f"binaryThresholdMin/Max and exposureMin/Max), re-run with --fresh, and "
              f"check the annotated screenshots in {RESULTS_DIR.name}/.")
        cam.setExposureGain(baseline["exposure"], baseline["gain"])
        return

    interactive = sys.stdin.isatty()
    doApply = autoApply
    if not doApply and interactive:
        doApply = askYesNo(f"\napply these camera settings to {CONFIG_FILE.name} and "
                           f"refresh the calibration background now? [y/N]: ", default=False)

    if doApply:
        cam.setExposureGain(best["exposure"], best["gain"])
        cfg["exposure"] = best["exposure"]
        cfg["gain"] = best["gain"]
        cfg["binaryThreshold"] = best["binaryThreshold"]
        cfg["accumFrames"] = best["accumFrames"]
        try:
            CONFIG_FILE.write_text(json.dumps(cfg, indent=2))
            prOk(f"saved -> {CONFIG_FILE.name}")
        except OSError as e:
            prWarn(f"could not save {CONFIG_FILE.name}: {e}")

        # homography.npz's background was captured at the OLD exposure/gain - stale
        # from here on, since diff-subtraction (computeMetrics/calibrate) assumes it
        # matches the currently configured settings. Refresh it in place so
        # 'measure'/'optimize'/'diagnose' don't silently score against a mismatched
        # background after this.
        try:
            esp.stop()
            time.sleep(cfg["settleSeconds"])
            newBackground = cam.grabBackground()
            np.savez(HOMOGRAPHY_FILE, homography=homography, background=newBackground)
            prOk(f"refreshed background in {HOMOGRAPHY_FILE.name} for the new exposure/gain")
        except (OptimizerError, OSError) as e:
            prWarn(f"could not refresh {HOMOGRAPHY_FILE.name} background: {e} - "
                 f"run 'calibrate' again before the next measurement")
    else:
        cam.setExposureGain(baseline["exposure"], baseline["gain"])
        prInfo(f"not applied - camera restored to its previous exposure/gain; best values "
             f"are only in {outFile.name}")


# ── color calibration (visibility threshold + gain matching) ─────────────────
# Automates the WebUI's two manual RGB calibration tools (Base R/G/B "Basiswert"
# sliders + Three Circles gain matching) with camera measurement instead of eyeballing.
# Unlike measure/optimize/diagnose, this needs no homography.npz - brightness matching
# is purely photometric (how much light lands on the sensor), not geometric.

CALIB_CHANNEL_INDEX = {"r": 1, "g": 2, "b": 3}   # calib-cam/calib-thresh-test channel convention
CALIB_CHANNEL_NAME = {1: "R", 2: "G", 3: "B"}

# Min connected-component pixel count to call a single static dot/dwell-point
# "visible" for autotune-colors - much lower than liveAnalysisMinComponentPx's
# default (60, tuned for a full traced pattern outline), since a single point at
# threshold-floor brightness is far smaller than that.
COLOR_TUNE_MIN_LIT_PX = 8


def _parseColorChannels(spec: str) -> list[int]:
    letters = [c.strip().lower() for c in spec.split(",") if c.strip()]
    unknown = [c for c in letters if c not in CALIB_CHANNEL_INDEX]
    if unknown:
        raise OptimizerError(f"unknown channel(s) in --channels: {unknown} - use r, g, b")
    if not letters:
        raise OptimizerError("--channels must name at least one of r, g, b")
    return [CALIB_CHANNEL_INDEX[c] for c in letters]


def measureBrightness(cam: Camera, background: np.ndarray, cfg: dict,
                      nFrames: int = 6, minAreaPx: float = COLOR_TUNE_MIN_LIT_PX
                      ) -> tuple[float, int, float]:
    """Homography-free brightness read for autotune-colors: no pattern geometry is
    scored here, only how much light actually lands on the sensor - so unlike
    measureOnce/computeMetrics this never warps to the DAC canvas. Captures nFrames
    max-accumulated (Camera.grabAccumulated), diffs against background (laser off),
    thresholds at binaryThreshold, and reuses _filteredComponents' noise-gated
    connected-component filter (the same one analyze-live uses) so a stray sensor
    speck can't register as "visible". Returns (mean brightness of kept pixels, 0.0
    if none found; kept pixel count; fraction of kept pixels raw-sensor-saturated
    >=250, see computeMetrics's saturationFrac)."""
    capture = cam.grabAccumulated(nFrames)
    diff = cv2.subtract(capture, background)
    _, mask = cv2.threshold(diff, cfg["binaryThreshold"], 255, cv2.THRESH_BINARY)
    _, filtered, _ = _filteredComponents(mask, minAreaPx)
    litPixels = filtered > 0
    litCount = int(np.count_nonzero(litPixels))
    if litCount == 0:
        return 0.0, 0, 0.0
    litVals = diff[litPixels].astype(float)
    return float(np.mean(litVals)), litCount, float(np.mean(litVals >= 250))


def sweepThreshold(esp: EspClient, cam: Camera, cfg: dict, background: np.ndarray,
                   channelIdx: int, iterations: int, marginUnits: int) -> dict:
    """Binary search on thresh_<c> (the 'Basiswert' visibility floor) using the
    existing manual calib-thresh-test beam (static, minimal-level, gain/gamma/dimmer
    bypassed - see EspClient.calibThreshTest) - automates exactly the procedure the
    WebUI's Base R/G/B sliders are for. Converges to the lowest thresh_<c> that's
    reliably camera-visible, then adds marginUnits of headroom (mirrors the "adjust
    until it just clips, then back off" convention the DAC Range Box calib pattern
    already uses)."""
    field = f"thresh_{CALIB_CHANNEL_NAME[channelIdx].lower()}"
    samples = []
    esp.calibThreshTest(active=True, channel=channelIdx)
    try:
        lo, hi = 0, 254
        while lo < hi:
            mid = (lo + hi) // 2
            esp.setCalibLive(**{field: mid})
            time.sleep(cfg["settleSeconds"])
            brightness, litPx, _ = measureBrightness(cam, background, cfg, nFrames=4)
            visible = litPx >= COLOR_TUNE_MIN_LIT_PX
            samples.append({"thresh": mid, "brightness": brightness, "litPx": litPx,
                           "visible": visible})
            if len(samples) >= iterations:
                break
            if visible:
                hi = mid
            else:
                lo = mid + 1
    finally:
        esp.calibThreshTest(active=False, channel=channelIdx)
    found = min(hi + marginUnits, 254)
    return {"field": field, "found": found, "floor": hi, "samples": samples}


def sweepGain(esp: EspClient, cam: Camera, cfg: dict, background: np.ndarray,
             channelIdx: int, targetBrightness: float, iterations: int) -> dict:
    """Binary search on gain_<c> against a static corner-dwell pattern (calib idx
    'corners4' - the same one 'calibrate' uses for homography, deliberately NOT
    scanned so PWM duty is measured without conflating it with scan-speed/dwell-time)
    until the measured brightness reaches targetBrightness - the weakest channel's
    own max brightness, picked by the caller since you can only dim a stronger
    channel down to match, never brighten the weak one past gain=255."""
    field = f"gain_{CALIB_CHANNEL_NAME[channelIdx].lower()}"
    samples = []
    esp.startPattern("corners4", channel=channelIdx)
    try:
        lo, hi = 0, 255
        while lo < hi:
            mid = (lo + hi) // 2
            esp.setCalibLive(**{field: mid})
            time.sleep(cfg["settleSeconds"])
            brightness, litPx, sat = measureBrightness(cam, background, cfg,
                                                        nFrames=cfg["accumFrames"])
            samples.append({"gain": mid, "brightness": brightness, "litPx": litPx,
                           "saturationFrac": sat})
            if len(samples) >= iterations:
                break
            if brightness < targetBrightness:
                lo = mid + 1
            else:
                hi = mid
    finally:
        esp.stop()
    return {"field": field, "found": hi, "samples": samples}


def interactiveExposureAdjust(cam: Camera, cfg: dict):
    """Lets the user dial in camera exposure by eye against a live feed before
    autotune-colors measures anything - same '+'/'-' auto-saved-to-camConfig.json
    exposure control 'preview' offers, just inline here instead of a separate prior
    command, since a too-dark exposure would otherwise silently bias every reading
    below (a near-threshold or weak-channel beam can be invisible to an underexposed
    camera even though it's physically lit, but there's no scored ground truth here
    for a search to catch that against, unlike autotune-camera). Reuses the already-
    open cam.liveView rather than opening a second window. No-op (keeps the
    configured exposure as-is) if there's no view window (--no-view) or the session
    isn't interactive - nothing to look at or press a key with."""
    liveView = cam.liveView
    if not liveView:
        prInfo("no camera view window (--no-view) - using exposure from camConfig.json as-is")
        return
    if not sys.stdin.isatty():
        prInfo("non-interactive session - using exposure from camConfig.json as-is")
        return

    exposure = cfg["exposure"]
    savedHotkeys = liveView.hotkeys
    liveView.hotkeys = ("[+/-] exposure (auto-saved)   [1/2/3] zoom   "
                        "[c] continue   [q] abort")
    pr("adjust exposure so a faint/near-threshold beam would still be visible without "
         "blowing out a full-brightness one, then press 'c' to continue: " + liveView.hotkeys)
    try:
        while True:
            frame = cam.grabGray()
            saturated = float(np.mean(frame >= 250)) * 100
            cam.statusText = f"set exposure, then 'c': exp {exposure}  sat {saturated:.1f}%  max {frame.max()}"
            key = liveView.lastKey
            if liveView.quitRequested or key == ord("q"):
                raise KeyboardInterrupt()
            if key in (ord("+"), ord("-")):
                exposure += 1 if key == ord("+") else -1
                cam.cap.set(cv2.CAP_PROP_EXPOSURE, exposure)
                cfg["exposure"] = exposure
                try:
                    CONFIG_FILE.write_text(json.dumps(cfg, indent=2))
                    prOk(f"exposure {exposure} -> saved to {CONFIG_FILE.name}")
                except OSError as e:
                    prWarn(f"exposure {exposure} (could not save {CONFIG_FILE.name}: {e})")
            elif key == ord("c"):
                return
    finally:
        liveView.hotkeys = savedHotkeys


def runAutotuneColors(cfg: dict, esp: EspClient, cam: Camera, channels: list[int],
                      doThreshold: bool, doGain: bool, iterations: int,
                      thresholdMargin: int, autoApply: bool):
    interactive = sys.stdin.isatty()

    # A too-dark camera exposure would silently distort every measurement below (a
    # near-threshold or weak-channel beam can be invisible to an underexposed camera
    # even though it's physically lit) - unlike autotune-camera's own Optuna search,
    # there's no scored ground truth here to search against, so a human eyeballs it
    # once against a live feed before anything is measured. Persists to camConfig.json
    # exactly like 'preview's exposure adjustment does (no restore-on-exit: the user
    # just dialed in a real setting, not a transient search).
    interactiveExposureAdjust(cam, cfg)

    pr("reading current gain/threshold from the ESP32 ...")
    before = esp.getConfig()
    origThresh = {c: before.get(f"thresh_{CALIB_CHANNEL_NAME[c].lower()}") for c in (1, 2, 3)}
    origGain = {c: before.get(f"gain_{CALIB_CHANNEL_NAME[c].lower()}") for c in (1, 2, 3)}
    if any(v is None for v in list(origThresh.values()) + list(origGain.values())):
        raise OptimizerError(
            "ESP32's GET /api/config did not report gain_r/g/b or thresh_r/g/b - "
            "firmware may predate this feature."
        )

    thresholdResults: dict[int, dict] = {}
    gainResults: dict[int, dict] = {}
    restored = False

    def restoreOriginals():
        nonlocal restored
        if restored:
            return
        esp.setCalibLive(thresh_r=origThresh[1], thresh_g=origThresh[2], thresh_b=origThresh[3],
                         gain_r=origGain[1], gain_g=origGain[2], gain_b=origGain[3])
        restored = True

    try:
        # Defensive: a previous crashed run could have left calib-thresh-test active,
        # which would otherwise light up the "laser off" background capture below.
        esp.calibThreshTest(active=False, channel=1)
        esp.stop()
        time.sleep(0.3)
        cam.statusText = "autotune-colors: background (laser off)"
        background = cam.grabBackground()

        if doThreshold:
            pr(bold("\n=== phase 1: visibility threshold (Basiswert) ==="))
            for c in channels:
                waitWhilePaused(cam)
                cam.statusText = f"threshold: channel {CALIB_CHANNEL_NAME[c]}"
                pr(f"  channel {CALIB_CHANNEL_NAME[c]}: searching ...")
                result = sweepThreshold(esp, cam, cfg, background, c, iterations, thresholdMargin)
                thresholdResults[c] = result
                if not any(s["visible"] for s in result["samples"]):
                    prWarn(f"  channel {CALIB_CHANNEL_NAME[c]}: never detected visible "
                          f"across the full threshold sweep - check the laser/optics for "
                          f"this channel, or that it's actually in camera frame.")
                pr(f"  channel {CALIB_CHANNEL_NAME[c]}: found thresh={result['found']} "
                      f"(was {origThresh[c]})")
            # Apply the found thresholds live so phase 2's gain search measures against
            # the corrected floor, not the original one.
            esp.setCalibLive(**{thresholdResults[c]["field"]: thresholdResults[c]["found"]
                                for c in channels})

        if doGain:
            pr(bold("\n=== phase 2: channel brightness (gain) matching ==="))
            baseline = {}
            for c in channels:
                waitWhilePaused(cam)
                cam.statusText = f"gain baseline: channel {CALIB_CHANNEL_NAME[c]}"
                esp.startPattern("corners4", channel=c)
                esp.setCalibLive(**{f"gain_{CALIB_CHANNEL_NAME[c].lower()}": 255})
                time.sleep(cfg["settleSeconds"])
                brightness, litPx, sat = measureBrightness(cam, background, cfg,
                                                           nFrames=cfg["accumFrames"])
                esp.stop()
                if litPx == 0:
                    raise OptimizerError(
                        f"channel {CALIB_CHANNEL_NAME[c]} not detected at all at full "
                        f"gain - cannot brightness-match. Check the laser/optics for this "
                        f"channel, or run 'preview' to confirm it's in camera frame."
                    )
                if sat > cfg["diagnoseThresholds"].get("saturationFrac", 0.10):
                    prWarn(f"  channel {CALIB_CHANNEL_NAME[c]}: {sat * 100:.0f}% of the "
                          f"detected beam is sensor-saturated at full gain - readings may "
                          f"be unreliable. Consider 'autotune-camera' or lowering exposure "
                          f"first.")
                baseline[c] = brightness
                pr(f"  channel {CALIB_CHANNEL_NAME[c]}: baseline brightness at gain=255 "
                      f"= {brightness:.1f}")

            referenceChannel = min(baseline, key=baseline.get)
            target = baseline[referenceChannel]
            pr(f"  reference: channel {CALIB_CHANNEL_NAME[referenceChannel]} (weakest, "
                  f"target brightness {target:.1f}) - other channel(s) matched down to it")

            for c in channels:
                if c == referenceChannel:
                    gainResults[c] = {"field": f"gain_{CALIB_CHANNEL_NAME[c].lower()}",
                                      "found": 255, "samples": [], "baseline": baseline[c]}
                    continue
                waitWhilePaused(cam)
                cam.statusText = f"gain: channel {CALIB_CHANNEL_NAME[c]}"
                result = sweepGain(esp, cam, cfg, background, c, target, iterations)
                result["baseline"] = baseline[c]
                gainResults[c] = result
                pr(f"  channel {CALIB_CHANNEL_NAME[c]}: found gain={result['found']} "
                      f"(was {origGain[c]})")

        pr()
        pr(bold("=== autotune-colors: summary ==="))
        rows = []
        for c in (1, 2, 3):
            if c not in channels:
                continue
            name = CALIB_CHANNEL_NAME[c]
            if c in thresholdResults:
                rows.append((f"{name} thresh", f"{origThresh[c]} -> {thresholdResults[c]['found']}"))
            if c in gainResults:
                rows.append((f"{name} gain", f"{origGain[c]} -> {gainResults[c]['found']}"))
        prTable(rows, headers=("field", "before -> after"))

        try:
            RESULTS_DIR.mkdir(exist_ok=True)
            timestamp = time.strftime("%Y-%m-%d_%H-%M-%S")
            outFile = RESULTS_DIR / f"colors_{timestamp}.json"
            outFile.write_text(json.dumps({
                "timestamp": timestamp,
                "channels": [CALIB_CHANNEL_NAME[c] for c in channels],
                "before": {"thresh": origThresh, "gain": origGain},
                "threshold": thresholdResults,
                "gain": gainResults,
            }, indent=2, default=str))
            prOk(f"saved -> {outFile.name}")
        except OSError as e:
            prWarn(f"could not save results file: {e}")

        changedAny = bool(thresholdResults) or bool(gainResults)
        doApply = autoApply
        if not doApply and interactive and changedAny:
            doApply = askYesNo("\napply these found value(s) to the ESP32 now? [y/N]: ",
                               default=False)

        if doApply and changedAny:
            fields = {}
            for c in channels:
                if c in thresholdResults:
                    fields[thresholdResults[c]["field"]] = thresholdResults[c]["found"]
                if c in gainResults:
                    fields[gainResults[c]["field"]] = gainResults[c]["found"]
            esp.setCalibLive(**fields)
            restored = True   # the applied values ARE the intended new live state now
            prOk("applied (live until reboot - persisting to NVS is offered next)")

            doSave = autoApply
            if not doSave and interactive:
                doSave = askYesNo("persist to NVS so it survives a reboot? [y/N]: ",
                                  default=False)
            if doSave:
                esp.calibSave()
                prOk("saved to NVS")
            else:
                prInfo("not persisted - applied values are live until the ESP32 reboots")
        elif changedAny and not interactive:
            prInfo("not applied - non-interactive session without --apply; found values "
                  "are only in the results JSON, original gain/threshold restored")
        else:
            prInfo("not applied - original gain/threshold restored")
    finally:
        restoreOriginals()


# ── main ─────────────────────────────────────────────────────────────────────

def main():
    global CONFIG_FILE
    _enableWindowsAnsi()
    parser = argparse.ArgumentParser(
        prog="optimizeGalvo.py",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description="GalvOS camera-in-the-loop galvo pattern optimizer.\n\n"
                     "Drives an OV9281 (or similar mono/global-shutter USB) camera plus the\n"
                     "GalvOS ESP32-S3 REST API to calibrate pixel<->DAC space and auto-tune\n"
                     "scan/corner/dwell parameters against camera-measured beam quality.",
        epilog=textwrap.dedent(f"""\
            {"-" * 78}
            typical workflow (run in order):
              0. wizard     first-time setup - runs automatically if no config exists yet
              1. check      verify the ESP32 controller is reachable, first thing to run
              2. preview    aim/focus the camera, dial in exposure
              3. calibrate  compute pixel -> DAC homography (required once per camera setup)
              4. measure    sanity-check current params on one pattern
              5. optimize   Optuna search loop, writes best params to results/ (safe to
                            interrupt and re-run - resumes from persistent SQLite storage)
              6. diagnose   measure currently-live output, flag geometry vs. optimizer-
                            settings problems, optionally autotune (= 'optimize') the latter
              7. autotune-camera  Optuna search over exposure/gain/binaryThreshold/
                            accumFrames (camera capture quality, not firmware params)

            standalone (run any time, doesn't fit the tuning order above):
              analyze-live  structural (no-reference) read of whatever preset is
                            live right now - flags gaps/breaks or an unexpectedly
                            open shape without needing a known ideal geometry, and
                            never starts/stops a pattern on the ESP32
              calibrate-warp  solves the firmware's /api/warp/* keystone grid -
                            independent of 'calibrate'/homography.npz, does NOT
                            require it to have been run first
              measure-resonance  sweeps a galvo axis, extracts fRes/Q ->
                            ring_freq_hz/ring_damping_ratio for the ZV shaper -
                            independent of 'calibrate'/homography.npz, no
                            pixel<->DAC mapping needed (relative amplitude only)
              tune-dac-range  camera closed-loop auto-tune of per-axis galvo
                            gain/offset framing (clip/underscan vs. the camera
                            frame border) - independent of 'calibrate'/
                            homography.npz, no pixel<->DAC mapping needed

            {"-" * 78}
            files:
              {CONFIG_FILE.name:<18} runtime config (created via the wizard on first run;
                                   override the path with --config)
              {HOMOGRAPHY_FILE.name:<18} pixel->DAC homography, written by 'calibrate',
                                   required by 'measure', 'optimize', and 'diagnose'
              {SEARCH_SPACE_FILE.name:<18} parameter ranges per optimize profile - edit to
                                   match your firmware's accepted parameter limits
              {RESULTS_DIR.name + "/":<18} measurement snapshots, optuna_study.db (resumable
                                   search state), per-trial .jsonl logs, and best-params JSON.
                                   Stale files (everything except *.db) trigger a delete-y/n
                                   prompt at the start of every interactive run.

            {"-" * 78}
            requirements: opencv-python, numpy, optuna, requests (see requirements.txt).
            If wheels for your Python version aren't published yet, use a 3.12/3.13 venv.

            [i] run 'optimizeGalvo.py <command> --help' for details on any command.
            optimizeGalvo.py v{SCRIPT_VERSION}
            """),
    )
    parser.add_argument("--version", action="version",
                        version=f"optimizeGalvo.py v{SCRIPT_VERSION}")
    parser.add_argument(
        "--config", metavar="PATH",
        help=f"path to the config JSON file (default: {CONFIG_FILE.name} next to this "
             f"script). Use this to keep separate configs for multiple rigs/cameras.")
    parser.add_argument(
        "--no-view", action="store_true", dest="noView",
        help="disable the live camera view window for 'calibrate'/'measure'/'optimize' "
             "even if showCameraView is true in the config (e.g. for headless/CI runs). "
             "'preview' always shows its window regardless of this flag.")
    parser.add_argument(
        "--zoom", type=int, choices=[1, 2, 3], default=1,
        help="initial digital zoom level for any camera view window opened by this run "
             "(1x/2x/3x, default: 1x). Change it live with keys '1'/'2'/'3' while the "
             "window has focus - crops to the center and rescales, so the window size "
             "stays constant while showing more detail.")
    sub = parser.add_subparsers(dest="cmd", required=True)

    sub.add_parser(
        "wizard",
        help="interactive first-time setup / reconfigure",
        description="Interactively prompts for each config value (ESP32 base URL, camera "
                     "index and resolution, exposure, DAC calibration range, HTTP timeout), "
                     "showing the current/default value in brackets - press Enter to keep "
                     "it. Also probes for available camera indices before asking. Runs "
                     "automatically the first time any command is used and no config file "
                     "exists yet (interactive sessions only); run it directly at any later "
                     "point to change settings. Offers to test the ESP32 connection right "
                     "after saving.")

    sub.add_parser(
        "check",
        help="verify the ESP32 controller is reachable (no camera needed)",
        description="GETs /api/status from the configured esp32BaseUrl and prints firmware "
                     "version, network info (hostname/IP/RSSI/uptime), and safety-interlock "
                     "state (E-Stop, scan-fail, laser armed). Does NOT open the camera, so it "
                     "works even without one attached. Run this first whenever things aren't "
                     "working, to rule out a wrong esp32BaseUrl / WiFi / mDNS issue before "
                     "chasing camera or optics problems. Exits with code 1 on failure.")

    sub.add_parser(
        "preview",
        help="live camera feed to set focus / exposure / ND filter",
        description="Opens the configured camera and shows a live grayscale preview window "
                     "with saturation %, max pixel value, and measured fps overlaid. Press "
                     "'+'/'-' to adjust exposure live (auto-saved to camConfig.json on every "
                     "keypress), '1'/'2'/'3' for 1x/2x/3x digital zoom, 's' to save the current "
                     "frame to results/snapshot_<timestamp>.png, 'space' to freeze/unfreeze the "
                     "image, 'q' to close the window. Use this "
                     "to physically aim and focus the camera on the projection surface, and "
                     "to dial in exposure so the beam trace is visible but not blown out, "
                     "before running 'calibrate'.")

    sub.add_parser(
        "calibrate",
        help="compute the pixel->DAC homography from 4 reference dots",
        description="Stops any running pattern and captures a dark background frame, then "
                     "asks the ESP32 to project the 'corners4' pattern (4 dots at +-dacRange "
                     "from camConfig.json, one per DAC-space corner) and detects their pixel "
                     "positions in the camera image. Solves the perspective homography between "
                     "camera pixels and DAC coordinates and writes it (with the background "
                     "frame) to homography.npz. Required once before 'measure' or 'optimize' "
                     "can run; re-run whenever the camera or projection surface is moved. Shows "
                     "a live camera view window throughout (see --no-view/--zoom) so you can "
                     "watch the 4 dots appear and confirm focus/framing before it proceeds. "
                     "Saves the raw corners4 capture to results/calibrate_<timestamp>.png, plus "
                     "a _labeled version marking which detected dot was identified as "
                     "TL/TR/BR/BL - check that against the dots' real physical layout if "
                     "measured patterns ever look rotated/mirrored relative to their ideal.")

    pMeasure = sub.add_parser(
        "measure",
        help="project one pattern and print beam-quality metrics",
        description="Runs a single named calibration pattern on the ESP32 with the currently "
                     "effective parameters, captures an accumulated frame, and computes/prints "
                     "path-deviation RMS, blank-segment leakage, corner hotspot ratio, and "
                     "brightness non-uniformity, plus the weighted cost (see costWeights in "
                     "camConfig.json). Saves the captured frame to "
                     "results/measure_<pattern>.png. Requires an existing homography.npz - "
                     "run 'calibrate' first. Shows a live camera view window while capturing "
                     "(see --no-view/--zoom).")
    pMeasure.add_argument(
        "--pattern", default="square",
        choices=list(CAMERA_AUTOTUNE_PATTERNS),
        help="calibration pattern to project and measure (default: square). "
             "'segments' also reports blank-leakage; 'square'/'star'/'wireframe'/'text' "
             "also report corner hotspot; 'particles' reports blob count/elongation/"
             "centroid error instead of corner hotspot; all patterns report path "
             "deviation and brightness uniformity.")

    pOpt = sub.add_parser(
        "optimize",
        help="Optuna search for the best scan/dwell parameters",
        description="Tunes one or more firmware optimizer profiles (Vector/Smooth/Waves/"
                     "MultiObject) with an Optuna TPE search, each via its own camera "
                     "pattern(s). Flow: queries the ESP32 (GET /api/config) for its profiles, "
                     "current parameter values, and member presets; shows an interactive "
                     "multi-select menu (or takes --profile) listing the presets each choice "
                     "affects; runs one study per selected profile over the parameter ranges "
                     "from its entry in searchSpace.json; then prints a before/after report "
                     "per profile - every parameter as changed (before -> after) or unchanged "
                     "with the reason (not searched / search kept baseline / inactive behind "
                     "a disabled *_enabled gate) - and offers to apply + persist the result "
                     "(see --apply; without applying, tuned values vanish when the calib-cam "
                     "session ends). Each trial POSTs candidate params (/api/calib-cam/"
                     "params), measures the profile's pattern(s), and sums their cost. "
                     "Requires an existing homography.npz - run 'calibrate' first. ESP32 requests that "
                     "time out or fail to connect are retried automatically (requestRetries "
                     "in the config) before aborting, so a single transient WiFi hiccup "
                     "during a long run doesn't need manual intervention. Study state "
                     "is kept in a persistent SQLite database (see --storage), so Ctrl+C or a "
                     "crash doesn't lose progress - just re-run the same command to resume. "
                     "Prints per-trial cost, duration, and an ETA for the remaining trials, "
                     "and writes a per-trial JSONL log plus results/best_<profile>_<timestamp>"
                     ".json with the best parameters found on completion. Shows a live camera "
                     "view window across all trials (see --no-view/--zoom), with the current "
                     "trial/pattern, measured fps, and a trial-count progress bar overlaid. "
                     "'space' pauses between trials (never mid-capture) and 'q' aborts the "
                     "study early, exactly like Ctrl+C - already-completed trials are kept. "
                     "In an interactive session, offers to then project real presets (Milky "
                     "Way, Text, 3D Cube, ...) live via the normal preset API afterwards, for "
                     "a qualitative visual check of the tuned result - no scoring, just the "
                     "camera view.")
    optProfileGroup = pOpt.add_mutually_exclusive_group()
    optProfileGroup.add_argument(
        "--profile", default=None,
        help="firmware profile(s) to tune, comma-separated, or 'all' (e.g. 'Vector' or "
             "'Vector,Smooth'). Camera-tunable profiles: Vector, Smooth, Waves, "
             "MultiObject - each is tuned via its own camera pattern(s), with the "
             "parameter ranges from its entry in searchSpace.json. Omit (and omit "
             "--preset) to pick interactively from a menu that also lists each "
             "profile's member presets (non-interactive sessions must pass one of "
             "--profile/--preset explicitly).")
    optProfileGroup.add_argument(
        "--preset", dest="presetName", default=None,
        help="tune whichever single firmware profile drives this real preset (e.g. "
             "'Milky Way'), looked up via the ESP32's live profile->preset membership "
             "(GET /api/config) - a shortcut for --profile when you know which preset "
             "looks wrong but not which optimizer profile governs it. Errors out if "
             "the preset belongs to a profile with no camera pattern (Wireframe/"
             "Trails/Text) - those aren't camera-tunable at all.")
    pOpt.add_argument(
        "--trials", type=int, default=20,
        help="number of Optuna trials to run this invocation (default: 20). More trials "
             "find better parameters but each trial costs one settle+capture per profile "
             "pattern. Studies persist and resume (see --study-name/--storage), so it's "
             "cheap to start small and extend later with more trials if needed.")
    pOpt.add_argument(
        "--study-name", dest="studyName", default=None,
        help="Optuna study name (default: each tuned profile's own name, e.g. 'Vector'). "
             "When tuning more than one profile, this is used as a prefix per profile "
             "(e.g. --study-name run1 -> 'run1_Vector', 'run1_Smooth', ...). Re-running "
             "with the same name(s) and --storage resumes/extends those studies instead "
             "of starting over - safe to re-run after a crash or Ctrl+C.")
    pOpt.add_argument(
        "--storage", dest="storageUrl", default=None,
        help="Optuna storage URL (default: sqlite:///results/optuna_study.db). Persistent "
             "SQLite storage means completed trials survive a crash or interruption.")
    pOpt.add_argument(
        "--fresh", action="store_true",
        help="start a brand-new study instead of resuming an existing one with the same "
             "name (appends a timestamp to the study name)")
    pOpt.add_argument(
        "--apply", action="store_true", dest="autoApply",
        help="apply each profile's best values to the ESP32 (/api/optimizer-live) and "
             "persist them to NVS (/api/optimizer-save) without asking. Without this "
             "flag, interactive sessions are prompted per profile and once for the NVS "
             "save; non-interactive sessions apply nothing (results only go to the "
             "JSON file). Needed because the calib-cam session restores the pre-tuning "
             "snapshot when it stops - tuned values do not stick by themselves.")

    pDiag = sub.add_parser(
        "diagnose",
        help="analyze current output for geometry vs. optimizer-setting problems",
        description="Measures each selected firmware profile's camera pattern(s) with "
                     "whatever parameters are currently live on the ESP32 (no overrides "
                     "applied - this is a read of the actual current setup, not a search) "
                     "and classifies the result per profile: OK, a GEOMETRY ISSUE (the "
                     "projected shape's size or position is off vs. the ideal - points at "
                     "galvo X/Y gain or DAC-range calibration drift, or a moved camera/"
                     "projection surface, and re-running 'calibrate' is the fix, not "
                     "retuning), or an OPTIMIZER SETTINGS ISSUE (path deviation/blank "
                     "leakage/corner hotspot/brightness uniformity out of tolerance - see "
                     "diagnoseThresholds in camConfig.json - while geometry is clean, which "
                     "means it IS fixable by retuning), or an INVALID MEASUREMENT (the "
                     "camera did not actually see the beam well enough to score anything - "
                     "no verdict is given at all, rather than inventing one from fallback "
                     "constants). Profiles flagged with a settings "
                     "issue can be handed straight to 'optimize' as an autotune step (see "
                     "--autotune), reusing the exact same search/apply/persist flow. "
                     "Requires an existing homography.npz - run 'calibrate' first.")
    pDiag.add_argument(
        "--profile", default=None,
        help="firmware profile(s) to diagnose, comma-separated, or 'all' - same meaning "
             "as 'optimize --profile'. Omit to pick interactively from a menu.")
    pDiag.add_argument(
        "--autotune", action="store_true",
        help="automatically run 'optimize' on any profile flagged with an optimizer "
             "settings issue, without asking first")
    pDiag.add_argument(
        "--trials", type=int, default=20,
        help="Optuna trials per profile if autotuning (default: 20) - same meaning as "
             "'optimize --trials'")
    pDiag.add_argument(
        "--study-name", dest="studyName", default=None,
        help="Optuna study name if autotuning - same meaning as 'optimize --study-name'")
    pDiag.add_argument(
        "--storage", dest="storageUrl", default=None,
        help="Optuna storage URL if autotuning - same meaning as 'optimize --storage'")
    pDiag.add_argument(
        "--apply", action="store_true", dest="autoApply",
        help="if autotuning, apply + persist the result without asking - same meaning "
             "as 'optimize --apply'")

    pCamTune = sub.add_parser(
        "autotune-camera",
        help="Optuna search for the best camera capture settings",
        description="Tunes exposure, gain, binaryThreshold, and accumFrames (bounds in "
                     "cameraAutotuneRanges, camConfig.json) with an Optuna TPE search - "
                     "every camera-capture knob that can reasonably be adjusted without "
                     "reopening the device (frameWidth/frameHeight/cameraFps/cameraBackend "
                     "are negotiated once at camera-open time, so use 'wizard' for those). "
                     "Firmware scan/dwell parameters are left exactly as currently live (no "
                     "override) - this isolates capture quality only; use 'optimize' to tune "
                     "pattern parameters. Scored with the same path-deviation/blank-leakage/"
                     "corner-hotspot/brightness-uniformity cost 'optimize' uses, plus a "
                     "saturation and background-brightness penalty (cameraAutotuneWeights) "
                     "that catches a washed-out capture those metrics alone wouldn't. "
                     "Background is re-captured every trial (laser off) at that trial's "
                     "exposure/gain, since a background from a different exposure would "
                     "corrupt the diff-subtraction the metrics rely on. Requires an existing "
                     "homography.npz (only its matrix is reused; run 'calibrate' first). "
                     "Prints a before/after report and, on apply, updates camConfig.json AND "
                     "refreshes homography.npz's background to match the new settings. Shows "
                     "a live camera view window across all trials (see --no-view/--zoom).")
    pCamTune.add_argument(
        "--patterns", default="square,circle,segments",
        help="comma-separated calib patterns to measure per trial (default: "
             "square,circle,segments - covers corner hotspot, pure path/uniformity, and "
             "blank leakage between them). Choices: "
             + ", ".join(CAMERA_AUTOTUNE_PATTERNS) + ".")
    pCamTune.add_argument(
        "--trials", type=int, default=20,
        help="number of Optuna trials to run this invocation (default: 20)")
    pCamTune.add_argument(
        "--study-name", dest="studyName", default=None,
        help="Optuna study name (default: 'camera_autotune'). Re-running with the same "
             "name and --storage resumes/extends that study.")
    pCamTune.add_argument(
        "--storage", dest="storageUrl", default=None,
        help="Optuna storage URL (default: sqlite:///results/optuna_study.db)")
    pCamTune.add_argument(
        "--fresh", action="store_true",
        help="start a brand-new study instead of resuming an existing one with the same name "
             "(appends a timestamp to the study name)")
    pCamTune.add_argument(
        "--apply", action="store_true", dest="autoApply",
        help="apply the best camera settings to camConfig.json and refresh homography.npz's "
             "background without asking")

    pColors = sub.add_parser(
        "autotune-colors",
        help="camera-measured RGB visibility-threshold + brightness matching",
        description="Automates the WebUI's two manual RGB calibration tools with camera "
                     "measurement instead of eyeballing. Phase 1 (visibility threshold / "
                     "'Basiswert'): for each channel, binary-searches thresh_r/g/b using the "
                     "existing static minimal-level calib-thresh-test beam, converging on the "
                     "lowest duty that's reliably camera-visible (plus a small margin). Phase "
                     "2 (channel brightness / gain matching): projects the static 'corners4' "
                     "dwell pattern per channel (no scanning, so PWM duty is measured without "
                     "conflating it with scan-speed/dwell-time), finds each channel's max "
                     "brightness at gain=255, then binary-searches gain_r/g/b on the other "
                     "channel(s) down to match the weakest channel's brightness (you can only "
                     "dim a stronger channel to match, never brighten the weak one further). "
                     "Prints a before/after table, saves the full sweep history to "
                     "results/colors_<timestamp>.json, and offers to apply (/api/calib-live) "
                     "+ persist (/api/calib-save) the result - see --apply. gain_r/g/b/"
                     "thresh_r/g/b have no session-snapshot/rollback in firmware (unlike "
                     "optimizer-live params) - every value tested during the search is a live, "
                     "permanent-until-changed mutation, so this script always restores the "
                     "ORIGINAL values live afterwards unless you confirm applying the found "
                     "ones (or on Ctrl+C/'q'/error). Does NOT require homography.npz - "
                     "brightness matching is photometric, not geometric, so 'calibrate' is not "
                     "a prerequisite. Starts with an interactive exposure check against a live "
                     "feed ('+'/'-' to adjust, auto-saved to camConfig.json, 'c' to continue) - "
                     "a too-dark exposure (e.g. one tuned for a full-brightness pattern "
                     "elsewhere) can make a near-threshold or weak-channel beam invisible and "
                     "silently bias every reading below, and there's no scored ground truth "
                     "here for a search to catch that automatically. Skipped in non-interactive "
                     "sessions or with --no-view. Shows a live camera view window throughout "
                     "(see --no-view/--zoom).")
    pColors.add_argument(
        "--channels", default="r,g,b",
        help="comma-separated channel(s) to tune (default: r,g,b). Useful to re-check a "
             "single channel after an optics change without re-running all three.")
    pColors.add_argument(
        "--skip-threshold", action="store_true", dest="skipThreshold",
        help="skip phase 1 (visibility threshold) - keep thresh_r/g/b as currently live and "
             "only run the gain-matching phase")
    pColors.add_argument(
        "--skip-gain", action="store_true", dest="skipGain",
        help="skip phase 2 (brightness/gain matching) - only run the threshold sweep")
    pColors.add_argument(
        "--iterations", type=int, default=8,
        help="binary-search iterations per channel per phase (default: 8 - enough to "
             "resolve the full 0-255 duty range to within ~1 unit)")
    pColors.add_argument(
        "--threshold-margin", type=int, default=3, dest="thresholdMargin",
        help="duty units of headroom added above the found visibility floor in phase 1 "
             "(default: 3) - mirrors the 'adjust until it just clips, then back off' "
             "convention used elsewhere in this calib UI")
    pColors.add_argument(
        "--apply", action="store_true", dest="autoApply",
        help="apply the found value(s) (/api/calib-live) and persist to NVS (/api/calib-save) "
             "without asking. Without this flag, interactive sessions are prompted once for "
             "apply and once for the NVS save; non-interactive sessions apply nothing and the "
             "original gain/threshold are restored (results only go to the JSON file).")

    sub.add_parser(
        "analyze-live",
        help="capture + structurally analyze whatever preset is currently live",
        description="Unlike measure/diagnose/optimize, this NEVER calls the calib-cam "
                     "start/stop API - it captures exactly one frame of whatever the "
                     "ESP32 is already projecting (any preset, ILDA file, or custom "
                     "output) and leaves it running throughout, before and after. "
                     "Since there's no known 'ideal' shape for an arbitrary preset "
                     "the way there is for the 6 calib patterns, this runs a no-"
                     "reference structural read instead of the usual path-deviation "
                     "scoring: whether the beam trace forms one continuous piece "
                     "(vs. a real gap/disconnected segment) and whether it encloses "
                     "an area (closed loop) or not, plus a sensor-saturation check. "
                     "Looks up the active preset's name/category via GET /api/state "
                     "+ /api/presets to label the report and word its one heuristic "
                     "flag - never a hard pass/fail, since plenty of presets are "
                     "legitimately multi-piece (particles, starfields, multi-object "
                     "scenes) or open by design (lines, waves). Always saves a raw + "
                     "annotated screenshot to results/analyze_live_<timestamp>.png "
                     "regardless of the verdict, for a follow-up look by eye or "
                     "another tool. Can NOT tell you a shape is the wrong shape, "
                     "only that it has a break or an unexpectedly open loop - see "
                     "the saved screenshot for anything more specific. Requires an "
                     "existing homography.npz - run 'calibrate' first.")

    pWarp = sub.add_parser(
        "calibrate-warp",
        help="solve the /api/warp/* keystone-correction grid via camera feedback",
        description="Solves GalvOS's N x N (2..5) warp-correction grid (Prompt 7a's "
                     "/api/warp/*), independent of 'calibrate'/homography.npz above (a "
                     "different purpose - that one scores optimizer trials, this one "
                     "corrects projector keystone/perspective). Resets the firmware to "
                     "an identity grid, then projects each identity-grid control point "
                     "as a single bright dwell dot (POST /api/debug/hw - bypasses "
                     "pattern generation, warp, AND applyCalibration() entirely, so "
                     "each point lands at a precisely known DAC position regardless of "
                     "whatever warp grid was previously active) and measures its pixel "
                     "position (median of --frames captures, threshold, subpixel "
                     "centroid via image moments). Rejects a point if its blob area or "
                     "brightness falls outside warpCalibMin/MaxBlobAreaPx/"
                     "warpCalibMinPeakVal in camConfig.json, and aborts with the full "
                     "list of failed indices rather than silently using bad data. For "
                     "--grid-size 2, solves one homography from the 4 corner "
                     "measurements and inverts it - exact, no further measurement "
                     "needed. For --grid-size 3..5, seeds every control point "
                     "(interior points too) from that same corner homography, then "
                     "iteratively re-measures and corrects each one (max 3 rounds, "
                     "stops early once within warpCalibToleranceCameraPx) since a "
                     "single homography can't exactly represent the firmware's "
                     "piecewise-bilinear grid. Prints before/after residual (mean/max, "
                     "camera pixels) and the resulting normalized grid JSON, then POSTs "
                     "it via /api/warp/set (enabled=true) unless --dry-run. Target "
                     "rectangle: --target-rect, or click 4 corners interactively if "
                     "omitted (needs a terminal). Shows a live camera view window "
                     "throughout (see --no-view/--zoom).")
    pWarp.add_argument(
        "--grid-size", type=int, choices=[2, 3, 4, 5], default=2, dest="gridSize",
        help="warp grid size N (N x N control points), matching the firmware's "
             "WARP_GRID_MAX range (default: 2 = plain 4-corner keystone)")
    pWarp.add_argument(
        "--target-rect", dest="targetRect", default=None,
        metavar="X0,Y0,X1,Y1",
        help="target rectangle in camera pixel space (top-left and bottom-right "
             "corners, comma-separated) that the warp grid's outer edge should map "
             "onto. Omit to click the 4 corners interactively instead (needs a "
             "terminal / --no-view is ignored for this one step).")
    pWarp.add_argument(
        "--frames", type=int, default=None,
        help="frames to median-stack per control point capture (default: "
             "warpCalibFrames in camConfig.json, currently used to seed it if unset)")
    pWarp.add_argument(
        "--dry-run", action="store_true", dest="dryRun",
        help="solve and print the resulting grid JSON, but do not POST it to the "
             "ESP32 (/api/warp/set is skipped)")

    pRes = sub.add_parser(
        "measure-resonance",
        help="sweep a galvo axis and measure its mechanical resonance (fRes/Q)",
        description="Sweeps one galvo axis over resonanceMinFreqHz-resonanceMaxFreqHz "
                     "Hz (default 50-2000, camConfig.json) via a firmware-generated sine "
                     "drive (POST /api/debug/resonance - requires firmware "
                     f"v{'.'.join(map(str, MIN_FW_VERSION_RESONANCE))}+) and reads each "
                     "step's driven streak SPATIAL EXTENT (not centroid - a symmetric "
                     "back-and-forth streak's centroid is amplitude-blind) as an "
                     "amplitude proxy. Two passes: a coarse log-spaced sweep across the "
                     "full range locates an approximate peak, then a fine linear-spaced "
                     "pass around it resolves an accurate -3dB bandwidth. Computes "
                     "Q = fRes/bandwidth, ring_damping_ratio = 1/(2Q), and ring_freq_hz "
                     "corrected from the driven-peak frequency back to the undamped "
                     "natural frequency the firmware's ZV shaper actually wants "
                     "(point_optimizer.cpp computeZvShaper()). Prints the result and the "
                     "exact /api/optimizer-live call to apply it - NEVER applies it "
                     "automatically. Saves a CSV (freq_hz, amplitude_px) and a Bode-plot "
                     "PNG to results/resonance_<axis>_<timestamp>.{csv,png}. No "
                     "homography needed (only RELATIVE amplitude matters here, unlike "
                     "'calibrate'/'calibrate-warp'). Test amplitude is deliberately "
                     "conservative (resonanceAmpFraction of the safe DAC range, further "
                     "clamped firmware-side) - see docs/feature-prompts/DECISIONS.md, "
                     "Prompt 13 for why a driven amplitude sweep needs that caution near "
                     "resonance specifically. Shows a live camera view window throughout "
                     "(see --no-view/--zoom).")
    pRes.add_argument(
        "--axis", choices=["x", "y"], default=None,
        help="galvo axis to drive (default: resonanceAxis in camConfig.json, 'x')")

    pTune = sub.add_parser(
        "tune-dac-range",
        help="camera closed-loop auto-tune of per-axis galvo gain/offset framing",
        description="Projects the static 'square' calib-cam pattern (thin rectangle "
                     "outline, full-contrast single channel, sharp corners) and reads "
                     "where its bounding box sits in the CAPTURED FRAME: a side within "
                     "~3% of the frame border counts as clipped, both sides of an axis "
                     "clearing ~5.5% counts as underscanning, otherwise OK (settled, "
                     "converged). Each "
                     "iteration solves and live-applies (POST /api/calib-live) the "
                     "galvo_x/y_gain + galvo_x/y_offset that would realize the next "
                     "candidate DAC-code range, re-measures, and adjusts again - "
                     "shrinking a clipped side, expanding an underscanning axis, "
                     "freezing a side once it settles within a small deadband of the "
                     "clip threshold (avoids oscillating forever right at the edge). "
                     "Distinct from Prompt 9a's dacClipX/Y (that flags DAC-CODE "
                     "clipping against the fixed dac_limit_min/max safety clamp, not "
                     "touched here) - this is a camera-pixel framing read, and (like "
                     "calibrate-warp/measure-resonance) needs no prior homography.npz. "
                     "Prints the tuned gain/offset table, then asks before persisting "
                     "via /api/calib-save (declining leaves them live but reverts on "
                     "next reboot) - or reverts to the original calibration entirely on "
                     "--dry-run or a declined non-converged run. Shows a live camera "
                     "view window throughout (see --no-view/--zoom).")
    pTune.add_argument(
        "--max-iterations", type=int, default=30, dest="maxIterations",
        help="stop after this many iterations even if not fully converged (default: 30)")
    pTune.add_argument(
        "--dry-run", action="store_true", dest="dryRun",
        help="run the tuning loop and print the result, but restore the original "
             "gain/offset afterwards instead of asking to save")

    parser.add_argument(
        "--debug", action="store_true",
        help="on error, print a full Python traceback instead of a short message "
             "(for troubleshooting a bug in this script itself)")

    args = parser.parse_args()

    if args.config:
        CONFIG_FILE = Path(args.config)

    pr(f"optimizeGalvo.py v{SCRIPT_VERSION}")

    try:
        if args.cmd != "wizard":
            cleanupResultsDir()
        dispatch(args)
    except OptimizerError as e:
        prWarn(e, file=sys.stderr)
        sys.exit(1)
    except (KeyboardInterrupt, EOFError):
        pr(file=sys.stderr)
        prWarn("interrupted", file=sys.stderr)
        sys.exit(130)
    except Exception as e:
        if args.debug:
            raise
        prWarn(f"unexpected {type(e).__name__}: {e}", file=sys.stderr)
        prTip("re-run with --debug for a full traceback", file=sys.stderr)
        sys.exit(1)


def dispatch(args):
    if args.cmd == "wizard":
        existing = None
        if CONFIG_FILE.exists():
            try:
                existing = json.loads(CONFIG_FILE.read_text())
            except json.JSONDecodeError as e:
                raise OptimizerError(
                    f"{CONFIG_FILE.name} is not valid JSON ({e}) - fix it by hand or "
                    f"delete it and re-run the wizard."
                ) from e
        runWizard(existing)
        return

    cfg = loadConfig()

    if args.cmd == "check":
        esp = EspClient.fromConfig(cfg)
        sys.exit(0 if runCheckConnection(cfg, esp) else 1)

    if args.cmd == "autotune-colors" and args.skipThreshold and args.skipGain:
        raise OptimizerError(
            "--skip-threshold and --skip-gain together skip both phases - nothing to do"
        )

    showView = cfg.get("showCameraView", True) and not args.noView
    viewCmds = ("calibrate", "measure", "optimize", "diagnose", "autotune-camera",
               "autotune-colors", "analyze-live", "calibrate-warp", "measure-resonance",
               "tune-dac-range")
    liveView = LiveView("GalvOS camera view", cfg["frameWidth"], cfg["frameHeight"],
                        zoomIdx=args.zoom - 1) \
        if showView and args.cmd in viewCmds else None
    if args.cmd in viewCmds:
        pr("camera view: " + (liveView.hotkeys if liveView
                                 else "disabled (--no-view or showCameraView=false)"))

    esp = EspClient.fromConfig(cfg)
    if args.cmd in LASER_REQUIRED_CMDS:
        requireLaserReady(esp)

    cam = Camera(cfg, liveView=liveView)
    try:
        if args.cmd == "preview":
            runPreview(cfg, cam, zoomIdx=args.zoom - 1)
        elif args.cmd == "calibrate":
            runCalibrate(cfg, esp, cam)
        elif args.cmd == "measure":
            homography, background = loadHomography()
            try:
                RESULTS_DIR.mkdir(exist_ok=True)
            except OSError as e:
                raise OptimizerError(f"cannot create {RESULTS_DIR.name}/: {e}") from e
            m, _ = measureOnce(esp, cam, cfg, homography, background, args.pattern,
                              saveTo=RESULTS_DIR / f"measure_{args.pattern}.png")
            if not m.valid:
                pr()
                pr(f"{'!' * 70}")
                prWarn(formatInvalid(m, args.pattern))
                prWarn("Every number below is a fallback constant, not a reading - do not "
                       "act on it. " + INVALID_MEASUREMENT_HINT)
                pr(f"{'!' * 70}")
            print(json.dumps(metricsToDict(m), indent=2))
        elif args.cmd == "optimize":
            runOptimize(cfg, esp, cam, args.profile, args.trials,
                       studyName=args.studyName, storageUrl=args.storageUrl,
                       fresh=args.fresh, autoApply=args.autoApply,
                       presetName=args.presetName)
        elif args.cmd == "diagnose":
            runDiagnose(cfg, esp, cam, args.profile, args.autotune, args.trials,
                       args.studyName, args.storageUrl, args.autoApply)
        elif args.cmd == "autotune-camera":
            patterns = [p.strip() for p in args.patterns.split(",") if p.strip()]
            runAutotuneCamera(cfg, esp, cam, patterns, args.trials,
                             args.studyName, args.storageUrl, args.fresh, args.autoApply)
        elif args.cmd == "autotune-colors":
            channels = _parseColorChannels(args.channels)
            runAutotuneColors(cfg, esp, cam, channels,
                             doThreshold=not args.skipThreshold, doGain=not args.skipGain,
                             iterations=args.iterations, thresholdMargin=args.thresholdMargin,
                             autoApply=args.autoApply)
        elif args.cmd == "analyze-live":
            runAnalyzeLive(cfg, esp, cam)
        elif args.cmd == "calibrate-warp":
            targetRect = _parseTargetRect(args.targetRect)
            runCalibrateWarp(cfg, esp, cam, args.gridSize, targetRect, args.frames, args.dryRun)
        elif args.cmd == "measure-resonance":
            runMeasureResonance(cfg, esp, cam, args.axis)
        elif args.cmd == "tune-dac-range":
            runTuneDacRange(cfg, esp, cam, args.maxIterations, args.dryRun)
        if liveView:
            holdLiveView(cam, cfg.get("resultViewHoldSeconds", MIN_RESULT_VIEW_HOLD_SECONDS))
    finally:
        # 'preview' and 'analyze-live' never start/stop a pattern on the ESP32 -
        # stopping here would interrupt whatever was already live before the command
        # ran, defeating the entire point of both (aim the camera / analyze the
        # current output without disturbing it).
        if args.cmd not in ("preview", "analyze-live"):
            try:
                esp.stop()
            except OptimizerError:
                pass
        if liveView:
            liveView.close()
        cam.release()


if __name__ == "__main__":
    main()
