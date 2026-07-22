#!/usr/bin/env python3
"""
GalvOS camera-in-the-loop optimizer profile tuner.

Host: Windows 11, OV9281 global-shutter USB camera (mono, 720p, 120 fps).
Target: GalvOS ESP32-S3 controller via REST (/api/calib-cam/*, /api/status).

Workflow:
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

Usage:
  python optimizeGalvo.py wizard
  python optimizeGalvo.py check
  python optimizeGalvo.py preview
  python optimizeGalvo.py calibrate
  python optimizeGalvo.py measure --pattern square
  python optimizeGalvo.py optimize --profile default --trials 60
  python optimizeGalvo.py optimize --preset "Milky Way" --trials 60
  python optimizeGalvo.py diagnose --autotune
  python optimizeGalvo.py --config myRig.json check

calibrate/measure/optimize/diagnose open a live camera view window (disable with
--no-view or showCameraView:false in the config); press '1'/'2'/'3' in that window
for 1x/2x/3x digital zoom (centered crop, rescaled - window size stays constant),
's' to save the current frame to results/snapshot_<timestamp>.png.

Run `python optimizeGalvo.py <command> --help` for details on any command.

Note: requires opencv-python, numpy, optuna, requests (see requirements.txt).
If wheels for your Python version are not yet published, use a Python
3.12/3.13 venv.
"""

import argparse
import collections
import json
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
SCRIPT_VERSION = "2.3.0"

# GalvOS firmware version that introduced /api/calib-cam/* (see firmware git log:
# "fw: v6.03.0 -- camera-in-the-loop calibration API (calib-cam)").
MIN_FW_VERSION_CALIB_CAM = (6, 3, 0)

# Firmware optimizer profile names, in index order - mirrors the OPT_PROFILE_*
# constants in GalvOS's include/config.h. The firmware API (GET /api/config)
# reports profiles by index only, not by name, so this list is how the script
# turns that into something readable. Keep in sync with config.h if it changes.
FIRMWARE_PROFILE_NAMES = (
    "Vector", "Smooth", "Waves", "Wireframe",
    "MultiObject", "Particles", "Trails", "Text",
)

# Which camera pattern(s) tune each firmware optimizer profile - mirrors
# calib_patterns.cpp's profileOf() for the "Cam ..." pattern set. Only these four
# profiles are camera-tunable - corners4/Particles is the homography reference
# with no scoreable ideal geometry, and Wireframe/Trails/Text have no cam pattern.
FW_PROFILE_PATTERNS = {
    "Vector":      ["square", "star"],
    "Smooth":      ["circle"],
    "Waves":       ["spiral"],
    "MultiObject": ["segments"],
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


# ── configuration ────────────────────────────────────────────────────────────

CONFIG_FILE = Path(__file__).parent / "camConfig.json"
HOMOGRAPHY_FILE = Path(__file__).parent / "homography.npz"
SEARCH_SPACE_FILE = Path(__file__).parent / "searchSpace.json"
RESULTS_DIR = Path(__file__).parent / "results"

DEFAULT_CONFIG = {
    "esp32BaseUrl": "http://galvos.local",
    "cameraIndex": 0,
    "frameWidth": 1280,
    "frameHeight": 720,
    "cameraFps": None,          # explicitly request this capture fps from the driver
                                # (null = let the driver auto-select; see the printed
                                # "driver-reported ... fps" line and the live overlay's
                                # measured fps to check what you're actually getting)
    "cameraBackend": "dshow",  # dshow (default) / msmf / any - try msmf if manual
                                # exposure won't stick under dshow (common on Windows
                                # with some UVC drivers) and fps stays low regardless
                                # of scene brightness
    "displaySmoothFrames": 3,  # rolling max-projection over this many raw frames,
                                # display-only - fixes preview flicker at high fps/
                                # short exposure without touching measurement accuracy
                                # (grabAccumulated's own accumFrames is separate). 1 = off
    "exposure": -11,            # DirectShow log2 scale, ~1/2048 s
    "gain": 0,
    "accumFrames": 24,          # frames max()-accumulated per measurement (~1 s @ 24 fps effective)
    "settleSeconds": 0.6,       # wait after param change before capture
    "binaryThreshold": 40,      # 0..255, beam trace threshold after background subtraction
    "dacRange": 30000,          # reference dot coordinate (+-) used for homography
    "showCameraView": True,     # live preview window during calibrate/measure/optimize
    "presetPreviewSeconds": 4.0,  # how long each real preset is shown in the optional
                                   # post-optimize visual sanity-check
    "costWeights": {
        "pathDeviationRms": 1.0,
        "blankLeakage": 2.0,
        "cornerHotspot": 0.5,
        "brightnessNonUniformity": 0.7
    },
    "diagnoseThresholds": {
        # Above these: 'diagnose' flags the profile. Geometry ones (scale/offset) are
        # NOT fixable by autotune - they mean the projected shape's size/position is
        # off, which points at galvo gain/DAC calibration drift or a moved camera/
        # surface, not a scan-parameter problem. The rest mirror costWeights' metrics
        # and DO trigger an autotune offer.
        "geometryScalePct": 5.0,        # abs(scaleErrorX/YPct) above this -> geometry issue
        "geometryOffsetUnits": 600.0,   # abs(offsetX/YUnits), DAC units, above this -> geometry issue
        "pathDeviationRms": 8.0,
        "blankLeakage": 15.0,
        "cornerHotspot": 0.35,
        "brightnessNonUniformity": 0.5
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
    if cfg.get("cameraBackend") not in ("dshow", "msmf", "any"):
        problems.append("cameraBackend must be 'dshow', 'msmf', or 'any'")
    if not isinstance(cfg.get("displaySmoothFrames"), int) or cfg["displaySmoothFrames"] < 1:
        problems.append("displaySmoothFrames must be an integer >= 1")
    if not isinstance(cfg.get("cameraIndex"), int) or cfg["cameraIndex"] < 0:
        problems.append("cameraIndex must be a non-negative integer")
    if not isinstance(cfg.get("accumFrames"), int) or cfg["accumFrames"] < 1:
        problems.append("accumFrames must be an integer >= 1")
    if not isinstance(cfg.get("binaryThreshold"), (int, float)) or not (0 <= cfg["binaryThreshold"] <= 255):
        problems.append("binaryThreshold must be a number between 0 and 255")
    costWeights = cfg.get("costWeights")
    if not isinstance(costWeights, dict) or not all(
            isinstance(v, (int, float)) for v in costWeights.values()):
        problems.append("costWeights must be an object of numeric weights")
    diagnoseThresholds = cfg.get("diagnoseThresholds")
    if not isinstance(diagnoseThresholds, dict) or not all(
            isinstance(v, (int, float)) and v >= 0 for v in diagnoseThresholds.values()):
        problems.append("diagnoseThresholds must be an object of non-negative numeric thresholds")
    if not isinstance(cfg.get("presetPreviewSeconds"), (int, float)) or cfg["presetPreviewSeconds"] <= 0:
        problems.append("presetPreviewSeconds must be a positive number")
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
            print(f"warning: {CONFIG_FILE.name} has unrecognized key(s), check for typos: "
                  f"{unknownKeys}")
        cfg = {**DEFAULT_CONFIG, **onDisk}
        for key in nestedDictKeys:
            if key in onDisk:
                unknownSub = sorted(set(onDisk[key]) - set(DEFAULT_CONFIG[key]))
                if unknownSub:
                    print(f"warning: {CONFIG_FILE.name} '{key}' has unrecognized key(s): "
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
                print(f"warning: could not add missing default key(s) to {CONFIG_FILE.name}: {e}")
            else:
                added = missingKeys + missingSub
                print(f"added missing default key(s) to {CONFIG_FILE.name}: {added}")

        validateConfig(cfg)
        return cfg

    if sys.stdin.isatty():
        print(f"no {CONFIG_FILE.name} found - running first-time setup wizard\n")
        cfg = runWizard()
        validateConfig(cfg)
        return cfg

    cfg = dict(DEFAULT_CONFIG)
    try:
        CONFIG_FILE.write_text(json.dumps(cfg, indent=2))
    except OSError as e:
        raise OptimizerError(f"cannot write {CONFIG_FILE}: {e}") from e
    print(f"created {CONFIG_FILE.name} with defaults (non-interactive session, skipped "
          f"the setup wizard - run 'optimizeGalvo.py wizard' to configure it)")
    return cfg


# ── ESP32 REST client ────────────────────────────────────────────────────────

class EspClient:
    def __init__(self, baseUrl: str, timeoutSeconds: float, retries: int = 2,
                retryDelaySeconds: float = 1.0):
        self.baseUrl = baseUrl.rstrip("/")
        self.timeout = timeoutSeconds
        self.retries = retries
        self.retryDelaySeconds = retryDelaySeconds

    def _request(self, method: str, path: str, payload: dict | None = None) -> dict:
        url = f"{self.baseUrl}{path}"
        attempt = 0
        while True:
            try:
                if method == "POST":
                    resp = requests.post(url, json=payload or {}, timeout=self.timeout)
                else:
                    resp = requests.get(url, timeout=self.timeout)
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
                print(f"warning: {path} {'timed out' if isinstance(e, requests.exceptions.Timeout) else 'connection failed'} "
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

    def startPattern(self, pattern: str) -> dict:
        return self._post("/api/calib-cam/start", {"pattern": pattern})

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

    def getPresets(self) -> list[dict]:
        """GET /api/presets - every real preset pattern (Milky Way, Text, 3D Cube, ...),
        as opposed to the synthetic calib-cam probes used for the Optuna search."""
        result = self._get("/api/presets")
        if not isinstance(result, list):
            raise OptimizerError("ESP32 /api/presets response was not a JSON array")
        return result

    def setPreset(self, idx: int) -> None:
        """POST /api/preset {idx} - activate a real preset; -1 deactivates it."""
        self._post("/api/preset", {"idx": idx})

    def applyOptimizerLive(self, profileIndex: int, params: dict) -> None:
        """POST /api/optimizer-live - permanently applies params to a firmware profile
        (until reboot). Needed after tuning: the calib-cam session restores a snapshot
        of the profile on /stop, so values tuned inside the session don't stick."""
        self._post("/api/optimizer-live", {"profile": profileIndex, **params})

    def saveOptimizer(self) -> None:
        """POST /api/optimizer-save - persists all optimizer profiles to NVS."""
        self._post("/api/optimizer-save")


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
            print(f"warning: could not save snapshot: {e}")
            return
        if ok:
            print(f"saved snapshot -> {path.relative_to(Path(__file__).parent)}")
        else:
            print(f"warning: cv2.imwrite reported failure for {path.name}")

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
        self.cap.set(cv2.CAP_PROP_GAIN, cfg["gain"])
        for _ in range(3):          # flush pipeline after settings change
            self.cap.read()

        self.liveView = liveView
        self.statusText = ""
        self._displayBuffer = collections.deque(maxlen=max(1, cfg.get("displaySmoothFrames", 3)))
        self.lastAccumulated: np.ndarray | None = None
        reportedFps = self.cap.get(cv2.CAP_PROP_FPS)
        readExposure = self.cap.get(cv2.CAP_PROP_EXPOSURE)
        print(f"camera: {cfg['frameWidth']}x{cfg['frameHeight']} @ driver-reported "
              f"{reportedFps:.1f} fps (nominal, not necessarily the achieved rate - "
              f"watch the live overlay for measured fps), backend={backendName}")
        if appliedAutoExpVal is not None:
            print(f"camera exposure: requested {cfg['exposure']}, driver confirms "
                  f"{readExposure:.2f} (manual mode value {appliedAutoExpVal})")
        else:
            print(f"camera exposure: requested {cfg['exposure']}, driver reports "
                  f"{readExposure:.2f} - none of the manual-exposure mode values "
                  f"{AUTO_EXPOSURE_MANUAL_CANDIDATES} stuck, so auto-exposure is very "
                  f"likely still active. If the measured fps in the live overlay is "
                  f"low AND changes with scene brightness, this is why - auto-exposure "
                  f"is lengthening integration time in normal lighting, which caps a "
                  f"global-shutter sensor's fps at 1/exposure_time. Try "
                  f"'cameraBackend': 'msmf' in {CONFIG_FILE.name} (or 'dshow' if "
                  f"already on msmf), or your camera vendor's own control app if "
                  f"neither UVC backend exposes a working manual-exposure switch.")

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
            acc = np.maximum(acc, self.grabGray())
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
    r = cfg["dacRange"]
    # DAC convention: x right, y down -> TL=(-r,-r), TR=(r,-r), BR=(r,r), BL=(-r,r)
    dacCorners = np.array([[-r, -r], [r, -r], [r, r], [-r, r]], dtype=np.float32)

    esp.stop()
    time.sleep(0.3)
    cam.statusText = "calibrate: background (laser off)"
    background = cam.grabBackground()

    waitWhilePaused(cam)  # safe boundary: laser is off, nothing running yet
    esp.startPattern("corners4")
    time.sleep(cfg["settleSeconds"])
    cam.statusText = "calibrate: corners4"
    image = cam.grabAccumulated(cfg["accumFrames"])
    esp.stop()

    diff = cv2.subtract(image, background)
    pixelCorners = orderCorners(detectDots(diff, 4, cfg["binaryThreshold"]))
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
    print(f"homography saved -> {HOMOGRAPHY_FILE.name}")
    print(h)


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
    raise ValueError(f"unknown pattern {pattern}")


def rasterizePolylines(polylines: list[np.ndarray], canvasSize: int, r: int,
                       thickness: int = 1) -> np.ndarray:
    scale = canvasSize / (2 * r)
    canvas = np.zeros((canvasSize, canvasSize), np.uint8)
    for line in polylines:
        pts = ((line + r) * scale).astype(np.int32)
        cv2.polylines(canvas, [pts], False, 255, thickness)
    return canvas


# ── metrics ──────────────────────────────────────────────────────────────────

CANVAS = 800   # DAC space rasterized to CANVAS x CANVAS px


@dataclass
class Metrics:
    pathDeviationRms: float
    blankLeakage: float
    cornerHotspot: float
    brightnessNonUniformity: float
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


def computeMetrics(capture: np.ndarray, background: np.ndarray, homography: np.ndarray,
                   pattern: str, cfg: dict) -> Metrics:
    r = cfg["dacRange"]
    diff = cv2.subtract(capture, background)
    dacImage = warpToDacCanvas(diff, homography, r)
    _, trace = cv2.threshold(dacImage, cfg["binaryThreshold"], 255, cv2.THRESH_BINARY)

    lit, gaps = idealPolylines(pattern, r)
    idealMask = rasterizePolylines(lit, CANVAS, r)

    # 1. path deviation: distance of every lit pixel from ideal path (DAC units RMS)
    distToIdeal = cv2.distanceTransform(cv2.bitwise_not(idealMask), cv2.DIST_L2, 5)
    tracePixels = trace > 0
    dacPerPixel = (2 * r) / CANVAS
    if tracePixels.any():
        pathDeviationRms = float(np.sqrt(np.mean(distToIdeal[tracePixels] ** 2)) * dacPerPixel)
    else:
        pathDeviationRms = float(2 * r)    # nothing visible -> worst case

    # 2. blank leakage: mean brightness inside gap corridors (should be dark)
    if gaps:
        gapMask = rasterizePolylines(gaps, CANVAS, r, thickness=9)
        blankLeakage = float(np.mean(dacImage[gapMask > 0]))
    else:
        blankLeakage = 0.0

    # 3. corner hotspot: corner ROI brightness / edge ROI brightness (dwell tuning)
    cornerHotspot = 0.0
    if pattern in ("square", "star"):
        vertices = lit[0][:-1]
        scale = CANVAS / (2 * r)
        roiHalf = 12
        cornerVals, edgeVals = [], []
        edgeMids = (vertices + np.roll(vertices, -1, axis=0)) / 2
        for group, sink in ((vertices, cornerVals), (edgeMids, edgeVals)):
            for vx, vy in group:
                cx, cy = int((vx + r) * scale), int((vy + r) * scale)
                roi = dacImage[max(cy - roiHalf, 0):cy + roiHalf, max(cx - roiHalf, 0):cx + roiHalf]
                sink.append(float(roi.mean()))
        edgeMean = max(np.mean(edgeVals), 1.0)
        cornerHotspot = float(abs(np.mean(cornerVals) / edgeMean - 1.0))

    # 4. brightness uniformity along ideal path (adaptive density check)
    pathVals = dacImage[idealMask > 0].astype(float)
    litVals = pathVals[pathVals > cfg["binaryThreshold"]]
    if len(litVals) > 10:
        brightnessNonUniformity = float(np.std(litVals) / max(np.mean(litVals), 1.0))
    else:
        brightnessNonUniformity = 1.0

    # 5. overall geometry: lit shape's size/position vs. ideal, in DAC units. Compared
    # against the ideal's own polyline coordinates (not idealMask) for exact values,
    # unaffected by rasterization. 1st/99th percentile instead of true min/max so a
    # handful of stray noise pixels surviving the threshold can't blow up the extent.
    idealPts = np.vstack(lit)
    idealExtentX = float(idealPts[:, 0].max() - idealPts[:, 0].min())
    idealExtentY = float(idealPts[:, 1].max() - idealPts[:, 1].min())
    idealCenterX = float((idealPts[:, 0].max() + idealPts[:, 0].min()) / 2.0)
    idealCenterY = float((idealPts[:, 1].max() + idealPts[:, 1].min()) / 2.0)
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

    w = cfg["costWeights"]
    cost = (w["pathDeviationRms"] * pathDeviationRms / 100.0
            + w["blankLeakage"] * blankLeakage / 10.0
            + w["cornerHotspot"] * cornerHotspot
            + w["brightnessNonUniformity"] * brightnessNonUniformity)
    return Metrics(pathDeviationRms, blankLeakage, cornerHotspot, brightnessNonUniformity,
                   scaleErrorXPct, scaleErrorYPct, offsetXUnits, offsetYUnits, cost)


def measureOnce(esp: EspClient, cam: Camera, cfg: dict, homography: np.ndarray,
                background: np.ndarray, pattern: str, saveTo: Path | None = None,
                statusPrefix: str = "measure",
                params: dict | None = None) -> tuple[Metrics, dict | None]:
    """params, if given, is applied via /api/calib-cam/params right after starting
    the pattern - the ESP32 only accepts /params while a calib-cam session is
    active, and it resets any previous overrides on every /start, so overrides
    must be (re-)applied per pattern rather than once per optimize trial."""
    waitWhilePaused(cam)  # safe boundary: no pattern is running yet
    esp.startPattern(pattern)
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
    esp.stop()
    if saveTo:
        try:
            cv2.imwrite(str(saveTo), capture)
        except cv2.error as e:
            print(f"warning: could not save capture to {saveTo}: {e}")
    metrics = computeMetrics(capture, background, homography, pattern, cfg)
    return metrics, effective


# ── optimization ─────────────────────────────────────────────────────────────

def loadSearchSpaceFile() -> dict:
    if not SEARCH_SPACE_FILE.exists():
        raise OptimizerError(
            f"missing {SEARCH_SPACE_FILE.name} - create it with at least one profile "
            f"before running 'optimize' (see the shipped example for the format)."
        )
    try:
        spaces = json.loads(SEARCH_SPACE_FILE.read_text())
    except json.JSONDecodeError as e:
        raise OptimizerError(f"{SEARCH_SPACE_FILE.name} is not valid JSON: {e}") from e
    except OSError as e:
        raise OptimizerError(f"cannot read {SEARCH_SPACE_FILE}: {e}") from e

    if not isinstance(spaces, dict):
        raise OptimizerError(f"{SEARCH_SPACE_FILE.name} must contain a JSON object")
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
        print(f"only one tunable profile - using '{tunable[0]['name']}'")
        return tunable

    if not sys.stdin.isatty():
        raise OptimizerError(
            f"multiple tunable profiles available ({sorted(byName)}) - pass --profile "
            f"with comma-separated names or 'all' in a non-interactive session"
        )

    print("\ntunable firmware profiles:")
    for i, p in enumerate(tunable, 1):
        patterns = spaces[p["name"]].get("patterns", FW_PROFILE_PATTERNS[p["name"]])
        nParams = len(spaces[p["name"]].get("params", {}))
        members = ", ".join(p["members"]) if p["members"] else "(no presets)"
        active = "  [active]" if p["active"] else ""
        print(f"  {i}. {p['name']}{active}  -  cam pattern(s): {', '.join(patterns)}, "
              f"{nParams} param(s)")
        print(f"       presets: {members}")
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
        print(f"  invalid choice(s) {bad or [raw]} - enter numbers 1-{len(tunable)}, "
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
    print(f"\n=== {profileName}: parameter changes (before -> after) ===")
    if changed:
        print("changed:")
        for field, before, after in changed:
            print(f"  {field:<30} {formatValue(before):>10} -> {formatValue(after)}")
    else:
        print("changed: (none)")
    print("unchanged:")
    for field, value, reason in unchanged:
        print(f"  {field:<30} {formatValue(value):>10}   {reason}")


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

    def objective(trial):
        nonlocal runIndex
        trialStart = time.monotonic()
        if cam.liveView:
            cam.liveView.setProgress(runIndex + 1, trials)
        params = suggestParams(trial, space)
        totalCost = 0.0
        perPattern = {}
        effectivePerPattern = {}
        for pattern in patterns:
            # params are re-applied for every pattern, not once per trial: the ESP32
            # only accepts /params while a calib-cam session is active, and each
            # /start resets any previous overrides back to the profile baseline.
            m, effective = measureOnce(
                esp, cam, cfg, homography, background, pattern,
                statusPrefix=f"optimize {profileName} trial {runIndex + 1}/{trials}",
                params=params)
            totalCost += m.cost
            trial.set_user_attr(f"metrics_{pattern}", vars(m))
            perPattern[pattern] = vars(m)
            effectivePerPattern[pattern] = effective
        trial.set_user_attr("effectiveParams", effectivePerPattern)

        duration = time.monotonic() - trialStart
        trialDurations.append(duration)
        runIndex += 1
        avgDuration = sum(trialDurations) / len(trialDurations)
        eta = (trials - runIndex) * avgDuration
        print(f"[{profileName} trial {runIndex}/{trials}] (#{trial.number}) "
              f"cost={totalCost:.4f}  {duration:.1f}s, avg {avgDuration:.1f}s/trial, "
              f"ETA {eta / 60:.1f} min")

        try:
            with logFile.open("a") as f:
                f.write(json.dumps({
                    "trial": trial.number, "cost": totalCost, "params": params,
                    "effectiveParams": effectivePerPattern, "metrics": perPattern,
                    "durationSeconds": round(duration, 2)
                }) + "\n")
        except OSError as e:
            print(f"warning: could not append to per-trial log {logFile.name}: {e}")
        return totalCost

    try:
        study = optuna.create_study(
            study_name=studyName, storage=storageUrl, load_if_exists=True,
            direction="minimize", sampler=optuna.samplers.TPESampler(seed=42))
    except Exception as e:
        raise OptimizerError(f"failed to open Optuna study storage '{storageUrl}': {e}") from e

    priorTrials = len(study.trials)
    if priorTrials:
        print(f"resuming study '{studyName}' ({storageUrl}) - "
              f"{priorTrials} trial(s) already recorded, adding {trials} more")
    else:
        print(f"new study '{studyName}' -> {storageUrl}")
    print(f"per-trial log -> {logFile.name}")

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
        print(f"\noptimize stopped early: {stoppedEarly}")
        print(f"re-run the same command (same --study-name/--storage) to continue - "
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
    print("querying ESP32 for available optimizer profiles ...")
    config = esp.getConfig()
    fwProfiles = fetchFirmwareProfiles(esp, config)
    activeFw = next((p for p in fwProfiles if p["active"]), None)
    print(f"ESP32 reports {len(fwProfiles)} optimizer profile(s)"
          + (f", currently active: {activeFw['name']}" if activeFw else "") + ".")

    if presetName:
        profile = resolveProfileForPreset(fwProfiles, presetName)
        print(f"preset '{presetName}' -> firmware profile '{profile}'")

    selected = selectFirmwareProfiles(spaces, profile, fwProfiles)

    print(f"\ntuning {len(selected)} profile(s), {trials} trial(s) each - "
          f"presets that will be affected:")
    for p in selected:
        members = ", ".join(p["members"]) if p["members"] else "(no presets)"
        print(f"  {p['name']}: {members}")

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

        print(f"\n=== tuning profile '{name}' via pattern(s) "
              f"{space['patterns']} ===")
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
                print(f"warning: could not save best-params file: {e}")
            print(f"\nbest cost {study.best_value:.4f} (over {len(completed)} completed "
                  f"trial(s)) - saved -> {outFile.name}")
            summary.append((name, study.best_value, len(changed)))

            # The calib-cam session restored the pre-tuning snapshot on /stop, so
            # tuned values are NOT live on the controller yet - apply explicitly.
            doApply = autoApply
            if not doApply and interactive and changed:
                answer = input(f"\napply these {len(changed)} changed value(s) to the "
                               f"'{name}' profile on the ESP32 now? [y/N]: ").strip().lower()
                doApply = answer in ("y", "yes")
            if doApply and changed:
                esp.applyOptimizerLive(idx, bestParams)
                appliedAny = True
                print(f"applied to '{name}' (live until reboot - persisting to NVS is "
                      f"offered at the end)")
            elif changed and not interactive:
                print("(not applied - non-interactive session without --apply; values "
                      "are only in the results JSON)")
        else:
            print(f"no completed trials for '{name}' - nothing to report")

        if stoppedEarly and ("interrupted" in stoppedEarly or "'q' pressed" in stoppedEarly):
            remaining = [q["name"] for q in selected[selected.index(p) + 1:]]
            if remaining:
                print(f"skipping remaining profile(s) {remaining} after interrupt")
            break

    if summary:
        print("\n=== summary ===")
        for name, cost, nChanged in summary:
            print(f"  {name:<12} best cost {cost:.4f}, {nChanged} param(s) changed")

    if appliedAny:
        doSave = autoApply
        if not doSave and interactive:
            answer = input("\npersist the applied values to NVS so they survive a "
                           "reboot? [y/N]: ").strip().lower()
            doSave = answer in ("y", "yes")
        if doSave:
            esp.saveOptimizer()
            print("saved to NVS")
        else:
            print("(not persisted - applied values are live until the ESP32 reboots)")

    if interactive and summary:
        answer = input("\npreview real presets live now? [y/N]: ").strip().lower()
        if answer in ("y", "yes"):
            suggestedNames = [m for p in selected for m in p["members"]]
            runPresetPreview(esp, cam, cfg, suggestedNames=suggestedNames or None)


def runPresetPreview(esp: EspClient, cam: Camera, cfg: dict,
                     suggestedNames: list[str] | None = None):
    """Projects real presets (Milky Way, Text, 3D Cube, ...) live through the normal
    preset API - a qualitative visual sanity-check of tuned parameters via the camera
    view, no automated scoring. Independent of the synthetic calib-cam probes used
    for the actual Optuna search."""
    presets = esp.getPresets()
    if not presets:
        print("ESP32 reports no presets available - skipping preview")
        return

    if suggestedNames:
        suggested = [p for p in presets if p["name"] in suggestedNames]
        if suggested:
            print("\npresets affected by the profile(s) just tuned:")
            for p in suggested:
                print(f"  {p['idx']:>3}. {p['name']}  ({p['cat']})")

    print("\nall available presets:")
    for p in presets:
        print(f"  {p['idx']:>3}. {p['name']}  ({p['cat']})")

    byIdx = {p["idx"]: p for p in presets}
    seconds = cfg.get("presetPreviewSeconds", 4.0)
    print(f"\nenter a preset number to project it live for {seconds:.0f}s each "
          f"(camera window: 'q' skips early, '1'/'2'/'3' zoom); Enter or 'q' here stops.")

    active = False
    try:
        while True:
            raw = input("preset #: ").strip().lower()
            if raw in ("", "q"):
                break
            if not raw.isdigit() or int(raw) not in byIdx:
                print(f"  invalid choice '{raw}' - enter one of the numbers above, "
                      f"or Enter/'q' to stop")
                continue
            idx = int(raw)
            esp.setPreset(idx)
            active = True
            print(f"projecting '{byIdx[idx]['name']}' ...")
            cam.statusText = f"preview: {byIdx[idx]['name']}"
            if cam.liveView:
                cam.liveView.quitRequested = False
            start = time.monotonic()
            while time.monotonic() - start < seconds:
                cam.grabGray()
                if cam.liveView and cam.liveView.quitRequested:
                    break
    finally:
        if active:
            esp.setPreset(-1)


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
    for pattern in patterns:
        m = allMetrics[pattern]
        if abs(m.scaleErrorXPct) > thresholds["geometryScalePct"]:
            geometryIssues.append(f"{pattern}: X size off by {m.scaleErrorXPct:+.1f}% vs. ideal")
        if abs(m.scaleErrorYPct) > thresholds["geometryScalePct"]:
            geometryIssues.append(f"{pattern}: Y size off by {m.scaleErrorYPct:+.1f}% vs. ideal")
        if abs(m.offsetXUnits) > thresholds["geometryOffsetUnits"]:
            geometryIssues.append(f"{pattern}: X position off by {m.offsetXUnits:+.0f} DAC units")
        if abs(m.offsetYUnits) > thresholds["geometryOffsetUnits"]:
            geometryIssues.append(f"{pattern}: Y position off by {m.offsetYUnits:+.0f} DAC units")
        if m.pathDeviationRms > thresholds["pathDeviationRms"]:
            settingsIssues.append(f"{pattern}: path deviation {m.pathDeviationRms:.1f} DAC units "
                                  f"(threshold {thresholds['pathDeviationRms']})")
        if m.blankLeakage > thresholds["blankLeakage"]:
            settingsIssues.append(f"{pattern}: blank leakage {m.blankLeakage:.1f} "
                                  f"(threshold {thresholds['blankLeakage']})")
        if m.cornerHotspot > thresholds["cornerHotspot"]:
            settingsIssues.append(f"{pattern}: corner hotspot ratio {m.cornerHotspot:.2f} "
                                  f"(threshold {thresholds['cornerHotspot']})")
        if m.brightnessNonUniformity > thresholds["brightnessNonUniformity"]:
            settingsIssues.append(f"{pattern}: brightness non-uniformity "
                                  f"{m.brightnessNonUniformity:.2f} "
                                  f"(threshold {thresholds['brightnessNonUniformity']})")
    if geometryIssues:
        verdict = "GEOMETRY ISSUE"
    elif settingsIssues:
        verdict = "OPTIMIZER SETTINGS ISSUE"
    else:
        verdict = "OK"
    return verdict, geometryIssues, settingsIssues


def printDiagnosis(name: str, verdict: str, geometryIssues: list[str], settingsIssues: list[str]):
    print(f"\n{name}: {verdict}")
    for g in geometryIssues:
        print(f"  [geometry] {g}")
    for s in settingsIssues:
        print(f"  [settings] {s}")
    if verdict == "GEOMETRY ISSUE":
        print("  -> not fixable by autotune. Re-run 'calibrate' (camera/projection surface "
              "may have moved), and if it persists, check the galvo X/Y gain and DAC range "
              "calibration.")
    elif verdict == "OK":
        print("  -> within tolerance, no action needed.")


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

    print("querying ESP32 for available optimizer profiles ...")
    config = esp.getConfig()
    fwProfiles = fetchFirmwareProfiles(esp, config)
    selected = selectFirmwareProfiles(spaces, profile, fwProfiles)
    thresholds = cfg["diagnoseThresholds"]

    print(f"\ndiagnosing {len(selected)} profile(s) against currently-live parameters "
         f"(no overrides applied) ...")

    results = []    # (name, verdict, geometryIssues, settingsIssues)
    flagged = []    # firmware-profile dicts flagged OPTIMIZER SETTINGS ISSUE
    try:
        for p in selected:
            name = p["name"]
            space = validateProfileSpace(name, spaces[name])
            allMetrics = {}
            for pattern in space["patterns"]:
                m, _ = measureOnce(esp, cam, cfg, homography, background, pattern,
                                   statusPrefix=f"diagnose {name}")
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

    print("\n=== diagnosis ===")
    for name, verdict, geometryIssues, settingsIssues in results:
        printDiagnosis(name, verdict, geometryIssues, settingsIssues)

    if not flagged:
        print("\nno profile needs autotune.")
        return

    flaggedNames = [p["name"] for p in flagged]
    print(f"\n{len(flagged)} profile(s) flagged for autotune: {flaggedNames}")
    doAutotune = autotune
    if not doAutotune and sys.stdin.isatty():
        answer = input("run autotune ('optimize') on these now? [y/N]: ").strip().lower()
        doAutotune = answer in ("y", "yes")
    if doAutotune:
        runOptimize(cfg, esp, cam, ",".join(flaggedNames), trials, studyName=studyName,
                   storageUrl=storageUrl, autoApply=autoApply)
    else:
        print(f"(not autotuning - re-run with --autotune, or "
              f"'optimizeGalvo.py optimize --profile {','.join(flaggedNames)}')")


# ── connection check ────────────────────────────────────────────────────────

def runCheckConnection(cfg: dict, esp: EspClient) -> bool:
    """Reachability/identity check against the ESP32. No camera required."""
    print(f"checking {esp.baseUrl} (timeout {esp.timeout}s) ...")
    try:
        status = esp.getStatus()
    except OptimizerError as e:
        print(f"FAILED: {e}")
        return False

    print("OK - controller reachable")
    fwVersionStr = status.get("fw_version", "?")
    print(f"  fw_version      : {fwVersionStr}")
    print(f"  hostname / ip   : {status.get('hostname', '?')} / {status.get('ip', '?')}")
    print(f"  rssi            : {status.get('rssi', '?')} dBm")
    print(f"  uptime_s        : {status.get('uptime_s', '?')}")
    print(f"  free_heap       : {status.get('free_heap', '?')} B")
    print(f"  free_psram      : {status.get('free_psram', '?')} B")
    print(f"  estop_ok        : {bool(status.get('estop_ok'))}")
    print(f"  scanfail_ok     : {bool(status.get('scanfail_ok'))}")
    print(f"  laser_armed     : {bool(status.get('laser_armed'))}")
    print(f"  debug_mode      : {bool(status.get('debug_mode'))}")
    if not status.get("estop_ok") or not status.get("scanfail_ok"):
        print("  NOTE: a safety interlock is currently tripped - "
              "calib-cam patterns will not project until cleared.")

    fwVersion = parseFwVersion(fwVersionStr)
    minStr = ".".join(map(str, MIN_FW_VERSION_CALIB_CAM))
    if fwVersion is None:
        print(f"  NOTE: could not parse fw_version '{fwVersionStr}' - unable to check "
              f"calib-cam API support (needs >= v{minStr})")
    elif fwVersion < MIN_FW_VERSION_CALIB_CAM:
        print(f"  WARNING: firmware v{fwVersionStr} predates the calib-cam API "
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
    print("=== GalvOS camera-in-the-loop optimizer - setup wizard ===")
    print("Press Enter to accept the default shown in [brackets].\n")

    cfg = dict(existingCfg) if existingCfg else dict(DEFAULT_CONFIG)

    def ask(key: str, prompt: str, cast=str):
        current = cfg.get(key, DEFAULT_CONFIG[key])
        raw = input(f"{prompt} [{current}]: ").strip()
        if not raw:
            cfg[key] = current
            return
        try:
            cfg[key] = cast(raw)
        except ValueError:
            print(f"  invalid value '{raw}', keeping {current}")
            cfg[key] = current

    def parseYesNo(s: str) -> bool:
        if s.lower() not in ("y", "yes", "n", "no", "true", "false", "1", "0"):
            raise ValueError(s)
        return s.lower() in ("y", "yes", "true", "1")

    ask("esp32BaseUrl", "ESP32 controller base URL (mDNS hostname or IP, e.g. "
                        "http://galvos.local or http://192.168.1.50)")

    print("\nscanning for cameras (DirectShow, indices 0-4) ...")
    found = probeCameras()
    if found:
        print(f"  found camera index(es): {found}")
    else:
        print("  no camera detected - plug it in, you can still finish setup and "
              "re-run 'optimizeGalvo.py wizard' later")
    ask("cameraIndex", "camera device index", int)
    ask("frameWidth", "camera frame width", int)
    ask("frameHeight", "camera frame height", int)
    ask("exposure", "camera exposure (DirectShow log2 scale, negative = shorter, "
                    "e.g. -11 ~= 1/2048s)", int)
    ask("dacRange", "DAC reference range (+-) for calibration corner dots", int)
    ask("requestTimeoutSeconds", "ESP32 HTTP request timeout (seconds)", float)
    ask("requestRetries", "extra retries on ESP32 timeout/connection error before giving "
                         "up (helps with transient WiFi hiccups during long optimize runs)",
       int)
    ask("showCameraView", "show a live camera view window during calibrate/measure/optimize "
                          "(y/n)", parseYesNo)

    try:
        CONFIG_FILE.write_text(json.dumps(cfg, indent=2))
    except OSError as e:
        raise OptimizerError(f"cannot write {CONFIG_FILE}: {e}") from e
    print(f"\nsaved -> {CONFIG_FILE.name}")
    print("(costWeights and other advanced settings were left at their current "
          f"values - edit {CONFIG_FILE.name} directly for those)")

    testNow = input("\ntest connection to the controller now? [Y/n]: ").strip().lower()
    if testNow in ("", "y", "yes"):
        esp = EspClient.fromConfig(cfg)
        runCheckConnection(cfg, esp)

    return cfg


# ── preview ──────────────────────────────────────────────────────────────────

PREVIEW_WINDOW_NAME = "OV9281 preview"


def runPreview(cfg: dict, cam: Camera, zoomIdx: int = 0):
    exposure = cfg["exposure"]
    liveView = LiveView(
        PREVIEW_WINDOW_NAME, cfg["frameWidth"], cfg["frameHeight"], zoomIdx=zoomIdx,
        hotkeys="[1/2/3] zoom   [+/-] exposure   [s] save   [space] pause   [q] quit")
    print("preview: " + liveView.hotkeys)
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
            key = liveView.update(frame, cam.statusText)
            if key in (ord("+"), ord("-")):
                exposure += 1 if key == ord("+") else -1
                cam.cap.set(cv2.CAP_PROP_EXPOSURE, exposure)
                print(f"exposure {exposure}")
    finally:
        liveView.close()


# ── main ─────────────────────────────────────────────────────────────────────

def main():
    global CONFIG_FILE
    parser = argparse.ArgumentParser(
        prog="optimizeGalvo.py",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description="GalvOS camera-in-the-loop galvo pattern optimizer.\n\n"
                     "Drives an OV9281 (or similar mono/global-shutter USB) camera plus the\n"
                     "GalvOS ESP32-S3 REST API to calibrate pixel<->DAC space and auto-tune\n"
                     "scan/corner/dwell parameters against camera-measured beam quality.",
        epilog=textwrap.dedent(f"""\
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

            files:
              {CONFIG_FILE.name:<18} runtime config (created via the wizard on first run;
                                   override the path with --config)
              {HOMOGRAPHY_FILE.name:<18} pixel->DAC homography, written by 'calibrate',
                                   required by 'measure', 'optimize', and 'diagnose'
              {SEARCH_SPACE_FILE.name:<18} parameter ranges per optimize profile - edit to
                                   match your firmware's accepted parameter limits
              {RESULTS_DIR.name + "/":<18} measurement snapshots, optuna_study.db (resumable
                                   search state), per-trial .jsonl logs, and best-params JSON

            requirements: opencv-python, numpy, optuna, requests (see requirements.txt).
            If wheels for your Python version aren't published yet, use a 3.12/3.13 venv.

            run 'optimizeGalvo.py <command> --help' for details on any command.
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
                     "'+'/'-' to adjust exposure live, '1'/'2'/'3' for 1x/2x/3x digital zoom, "
                     "'s' to save the current frame to results/snapshot_<timestamp>.png, "
                     "'space' to freeze/unfreeze the image, 'q' to close the window. Use this "
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
                     "watch the 4 dots appear and confirm focus/framing before it proceeds.")

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
        choices=["square", "star", "segments", "circle", "spiral"],
        help="calibration pattern to project and measure (default: square). "
             "'segments' also reports blank-leakage; 'square'/'star' also report corner "
             "hotspot; all patterns report path deviation and brightness uniformity.")

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
                     "means it IS fixable by retuning). Profiles flagged with a settings "
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

    parser.add_argument(
        "--debug", action="store_true",
        help="on error, print a full Python traceback instead of a short message "
             "(for troubleshooting a bug in this script itself)")

    args = parser.parse_args()

    if args.config:
        CONFIG_FILE = Path(args.config)

    print(f"optimizeGalvo.py v{SCRIPT_VERSION}")

    try:
        dispatch(args)
    except OptimizerError as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)
    except (KeyboardInterrupt, EOFError):
        print("\ninterrupted", file=sys.stderr)
        sys.exit(130)
    except Exception as e:
        if args.debug:
            raise
        print(f"error: unexpected {type(e).__name__}: {e}", file=sys.stderr)
        print("(re-run with --debug for a full traceback)", file=sys.stderr)
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

    showView = cfg.get("showCameraView", True) and not args.noView
    liveView = LiveView("GalvOS camera view", cfg["frameWidth"], cfg["frameHeight"],
                        zoomIdx=args.zoom - 1) \
        if showView and args.cmd in ("calibrate", "measure", "optimize", "diagnose") else None
    if args.cmd in ("calibrate", "measure", "optimize", "diagnose"):
        print("camera view: " + (liveView.hotkeys if liveView
                                 else "disabled (--no-view or showCameraView=false)"))

    cam = Camera(cfg, liveView=liveView)
    esp = EspClient.fromConfig(cfg)
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
            print(json.dumps(vars(m), indent=2))
        elif args.cmd == "optimize":
            runOptimize(cfg, esp, cam, args.profile, args.trials,
                       studyName=args.studyName, storageUrl=args.storageUrl,
                       fresh=args.fresh, autoApply=args.autoApply,
                       presetName=args.presetName)
        elif args.cmd == "diagnose":
            runDiagnose(cfg, esp, cam, args.profile, args.autotune, args.trials,
                       args.studyName, args.storageUrl, args.autoApply)
    finally:
        try:
            esp.stop()
        except OptimizerError:
            pass
        if liveView:
            liveView.close()
        cam.release()


if __name__ == "__main__":
    main()
