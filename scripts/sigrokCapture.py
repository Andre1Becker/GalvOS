#!/usr/bin/env python3
"""
sigrokCapture.py — GalvOS hardware measurement via sigrok-cli

Two sigrok-compatible instruments, both driven through the `sigrok-cli`
binary (no libsigrok Python bindings required -- those are fragile to get
installed on Windows, sigrok-cli is a stable, documented CLI surface):

  Kingst LA1010 (logic analyzer) -- probes the DAC8562 SPI bus and decodes
  the exact 24-bit write-frame format used by galvo_out.cpp's
  writeDAC8562()/writeDAC8562XY() (verified against source, not assumed --
  see SpiCapture's docstring).

  Hantek 6022BE (oscilloscope) -- probes the galvo analog X/Y outputs and
  extracts ZV-shaper ringing parameters (ring_freq_hz / ring_damping_ratio,
  same naming as optimizeGalvo.py's measure-resonance workflow) from a step
  response, with an FFT-based method as cross-validation.

Physical wiring (see CLAUDE.md "Key GPIO Map" / hardware/schematics):
  SCLK   = ESP32-S3 GPIO12  (PIN_GALVO_SCK,  include/pinmap.h:43)
  DIN    = ESP32-S3 GPIO11  (PIN_GALVO_MOSI, include/pinmap.h:44)
  /SYNC  = ESP32-S3 GPIO10  (PIN_GALVO_CS,   include/pinmap.h:45, active-low)
  J_GALVO Pin2/Pin4 = analog X/Y galvo drive, referenced to AGND
  Laser R TTL = ESP32-S3 GPIO7  (PIN_LASER_R, optional, inverted logic)
  Laser G TTL = ESP32-S3 GPIO8  (PIN_LASER_G, optional, inverted logic)
  Laser B TTL = ESP32-S3 GPIO21 (PIN_LASER_B, optional, inverted logic)

These GPIO numbers are the board's own pin names, NOT sigrok channel
names -- the LA1010's channel numbering is independent and depends on
which of its probe leads you actually clipped to which board pin. Map that
with --sclk-channel/--din-channel/--sync-channel/--sd-cs-channel (defaults
CH0/CH1/CH2, matching the LA1010's own channel names -- confirmed live via
`sigrok-cli -d kingst-la2016 --show`; see --mode show below to re-verify
your own probe assignment). The laser RGB TTL lines are optional extra
probes on any free LA1010 channel(s) -- map with --r-channel/--g-channel/
--b-channel; when set, SpiCapture.decode() adds laserR/laserG/laserB
columns (raw GPIO level, 1=OFF/0=ON per CLAUDE.md's inverted convention)
for checking blanking-edge alignment against DAC frame timing.

NOTE (2026): the SD card was rewired off the DAC's SPI2 bus onto its own
independent SPI3 bus (GPIO5/6/1/42) in firmware v5.90.0 -- see CLAUDE.md's
"RESOLVED -- SD rewire complete" entry. On current hardware there is no
bus contention left to filter. --sd-cs-channel is kept for defensive
parsing (older board revisions, or genuine probing mistakes) and is a
no-op if omitted.

Usage:
  sigrokCapture.py --mode spi    --duration 2 --output spi_frames.csv
  sigrokCapture.py --mode spi    --duration 2 --output spi_frames.csv \
      --r-channel CH4 --g-channel CH5 --b-channel CH6  # + laser TTL
  sigrokCapture.py --mode analog --channels x,y --duration 0.5 --output xy.csv
  sigrokCapture.py --mode scan   [--driver kingst-la2016]
  sigrokCapture.py --mode show   --driver kingst-la2016

Driver IDs -- confirmed live via `sigrok-cli --scan`/`sigrok-cli -d <id>
--show` against this project's actual LA1010 + 6022BE (libsigrok
0.6.0-git/sigrok-cli 0.8.0-git on Windows); override with --driver if your
libsigrok build differs:
  Kingst LA1010  -> "kingst-la2016" (mainline libsigrok's Kingst driver
                     covers the whole LA1010/LA1016/LA2016/LA5016/LA5032
                     family). Channels: CH0-CH15 (+PWM1/PWM2, unused here).
                     Needs its MCU firmware + FPGA bitstream extracted from
                     KingstVIS (sigrok-fwextract-kingst-la2016, see
                     sigrok-util) dropped into sigrok-cli's
                     share/sigrok-firmware/ dir, AND the device rebound to
                     WinUSB via Zadig -- libusb otherwise fails to open it
                     with LIBUSB_ERROR_NOT_SUPPORTED even once the firmware
                     files are in place, since it's a distinct problem
                     (driver binding, not firmware availability).
  Hantek 6022BE  -> "hantek-6xxx" (needs sigrok-firmware-fx2lafw >= 0.1.4
                     uploaded to the device; NOT "fx2lafw", which is the
                     logic-analyzer-only driver for bare Cypress FX2
                     boards). Channels: CH1/CH2. vdiv is a PER-CHANNEL-GROUP
                     config (`--channel-group CH1 --config vdiv="1 V"`), not
                     a global key -- AnalogCapture applies it that way.
                     Also confirmed live: this driver does NOT reliably
                     honor `--time` -- requested durations came back at
                     ~42-44% of what was asked (3000ms -> ~1.31s actual,
                     5000ms -> ~2.10s actual). AnalogCapture.capture() warns
                     when this happens; over-request duration if you need a
                     guaranteed minimum window.

Exit codes: 0 ok, 1 sigrok-cli/parse error, 2 bad arguments, 3 sigrok-cli
not found, 4 device not found, 5 capture timed out, 130 interrupted.
"""

import argparse
import contextlib
import io
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
import pandas as pd

__version__ = "1.0.0"


# ── console helpers (ASCII-only status prefixes, matches optimizeGalvo.py's
#    convention -- avoids Windows console code-page issues with emoji) ──────
def prOk(*values):
    print("[+]", *values)


def prWarn(*values):
    print("[!]", *values)


def prInfo(*values):
    print("[i]", *values)


def prErr(*values):
    print("[!]", *values, file=sys.stderr)


# ── errors ───────────────────────────────────────────────────────────────
class SigrokError(RuntimeError):
    """Base class for sigrok-cli invocation/parsing failures."""


class SigrokNotFoundError(SigrokError):
    """sigrok-cli executable could not be located."""


class DeviceNotFoundError(SigrokError):
    """No matching sigrok device was found for the requested driver."""


class CaptureTimeoutError(SigrokError):
    """sigrok-cli did not complete within the expected time."""


# ── sigrok-cli process wrapper ──────────────────────────────────────────
def _resolve_sigrok_cli(explicit=None):
    if explicit:
        p = Path(explicit)
        if not p.exists():
            raise SigrokNotFoundError(f"--sigrok-cli path does not exist: {explicit}")
        return str(p)
    found = shutil.which("sigrok-cli") or shutil.which("sigrok-cli.exe")
    if found:
        return found
    raise SigrokNotFoundError(
        "sigrok-cli not found on PATH. Install libsigrok/sigrok-cli (bundled with "
        "PulseView, see https://sigrok.org/wiki/Downloads) and ensure it's on PATH, "
        "or pass --sigrok-cli <path-to-sigrok-cli.exe>."
    )


def _run_sigrok(args, timeout, sigrok_cli=None, driver=None):
    exe = _resolve_sigrok_cli(sigrok_cli)
    cmd = [exe] + args
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except FileNotFoundError as e:
        raise SigrokNotFoundError(f"failed to execute '{exe}': {e}") from e
    except subprocess.TimeoutExpired as e:
        raise CaptureTimeoutError(
            f"sigrok-cli did not finish within {timeout:.1f}s (cmd: {' '.join(cmd)}). "
            "Device may be unresponsive, unplugged, or the requested "
            "duration/sample count is too large for this timeout."
        ) from e

    if proc.returncode != 0:
        stderr = (proc.stderr or "").strip()
        lowered = stderr.lower()
        if "no devices found" in lowered or ("not found" in lowered and driver):
            raise DeviceNotFoundError(
                f"sigrok-cli could not find a device for driver '{driver}'. Run "
                f"`sigrok-cli --scan` (or `sigrok-cli -d {driver} --scan`) to verify "
                "the instrument is connected, powered, and the driver id is correct "
                "for the libsigrok version installed on this machine.\n"
                f"sigrok-cli stderr:\n{stderr}"
            )
        raise SigrokError(
            f"sigrok-cli exited with code {proc.returncode}.\n"
            f"cmd: {' '.join(cmd)}\nstderr:\n{stderr}"
        )
    return proc


def scan(driver=None, sigrok_cli=None):
    """Run `sigrok-cli --scan` (optionally scoped to one driver) and return stdout."""
    args = ["--scan"] if not driver else ["-d", driver, "--scan"]
    return _run_sigrok(args, timeout=30, sigrok_cli=sigrok_cli, driver=driver).stdout


def show(driver, sigrok_cli=None):
    """Run `sigrok-cli -d <driver> --show` -- lists real channel names/config
    options for the connected device. Run this BEFORE a real capture to
    verify channel naming instead of trusting the --*-channel defaults."""
    return _run_sigrok(["-d", driver, "--show"], timeout=30, sigrok_cli=sigrok_cli, driver=driver).stdout


@contextlib.contextmanager
def _tmp_or_named(explicit_path, suffix):
    """Yield a Path to write the raw sigrok-cli capture into. Uses
    explicit_path if given (kept on disk afterwards, for debugging a bad
    decode), otherwise a scratch tempfile that's deleted on exit."""
    if explicit_path is not None:
        path = Path(explicit_path)
        path.parent.mkdir(parents=True, exist_ok=True)
        yield path
        return
    fd, name = tempfile.mkstemp(suffix=suffix)
    os.close(fd)
    path = Path(name)
    try:
        yield path
    finally:
        try:
            path.unlink(missing_ok=True)
        except OSError:
            pass


def _fmt_rate(hz):
    # Plain integer Hz -- avoids relying on sigrok-cli's "100m"/"1g" suffix
    # parsing, which is a convenience shorthand, not a stricter format.
    return str(int(round(hz)))


def _fmt_vdiv(value):
    # hantek-6xxx's vdiv config wants "<number> V"/"<number> mV" (confirmed
    # via `sigrok-cli -d hantek-6xxx --channel-group CH1 --show`: this
    # 6022BE's discrete steps are 100 mV, 250 mV, 500 mV, 1 V). Accepts
    # '1', '1V', '500m', '500mV', '500 mV', ... and normalizes to that form;
    # anything unrecognized is passed through as-is (sigrok-cli's own error
    # will surface if it's rejected).
    s = str(value).strip()
    m = re.fullmatch(r"([0-9.]+)\s*(m|M)?\s*[Vv]?", s)
    if not m:
        return s
    number, milli = m.group(1), m.group(2)
    return f"{number} {'mV' if milli else 'V'}"


# ── minimal VCD parser (stdlib-only; no libsigrok Python bindings needed) ──
_TIMESCALE_UNITS = {"s": 1.0, "ms": 1e-3, "us": 1e-6, "ns": 1e-9, "ps": 1e-12, "fs": 1e-15}


def _parse_vcd(path):
    """Parse a sigrok-cli VCD capture into {signal_name: (times_s, values)}.

    Only handles 1-bit scalar signals (all channels used here are single
    digital lines) -- vector ('b...') value changes are ignored. Robust to
    the $timescale/$var header being wrapped across multiple lines by
    reading the whole header as one blob before regexing it.
    """
    text = Path(path).read_text(encoding="ascii", errors="replace")
    marker = "$enddefinitions"
    idx = text.find(marker)
    if idx == -1:
        raise SigrokError(f"'{path}' does not look like a VCD file (no $enddefinitions)")
    header, body = text[:idx], text[idx:]

    ts_m = re.search(r"\$timescale\s+(\d+)\s*(s|ms|us|ns|ps|fs)\s*\$end", header)
    timescale_s = (int(ts_m.group(1)) * _TIMESCALE_UNITS[ts_m.group(2)]) if ts_m else 1e-9
    if ts_m is None:
        prWarn(f"could not parse $timescale in {path}, assuming 1ns")

    id_to_name = {}
    for m in re.finditer(r"\$var\s+\S+\s+\d+\s+(\S+)\s+(\S+)", header):
        id_to_name[m.group(1)] = m.group(2)

    name_times, name_values = {}, {}
    for name in id_to_name.values():
        name_times[name] = []
        name_values[name] = []

    cur_time = 0
    for raw_line in body.splitlines():
        # Tokenize by whitespace rather than treating each line as one
        # token: confirmed live that this libsigrok build's vcd writer packs
        # multiple space-separated value changes onto one dump line (e.g.
        # '#0  0! 0" 0#'), AND sometimes glues a bare "FRAME-BEGIN"/
        # "FRAME-END" marker (not standard VCD -- appears to be specific to
        # this build) directly onto the last token with NO separator at all
        # (observed live: "0#FRAME-END" as one single token).
        for tok in raw_line.split():
            c0 = tok[0]
            if c0 == "#":
                try:
                    cur_time = int(tok[1:])
                except ValueError:
                    pass
                continue
            if c0 in "01xXzZ":
                if len(tok) < 2:
                    continue
                # VCD scalar idents are always a single printable-ASCII char
                # for <=94 signals -- the scheme every VCD writer uses, and
                # we only ever declare <=4 (SCLK/DIN/SYNC/SDCS). Only look at
                # that one char and ignore anything glued on after it (e.g.
                # the "FRAME-END" case above), rather than treating the rest
                # of the token as part of the identifier.
                name = id_to_name.get(tok[1])
                if name is None:
                    continue
                name_times[name].append(cur_time)
                name_values[name].append(1 if c0 == "1" else 0)
                continue
            # 'b'/'B' vector values, bare $dumpvars/$end/$comment markers,
            # and non-VCD framing markers (FRAME-BEGIN/FRAME-END): not
            # applicable to our 1-bit probes, silently skipped.

    traces = {}
    for name, times in name_times.items():
        if not times:
            continue
        t = np.asarray(times, dtype=np.float64) * timescale_s
        v = np.asarray(name_values[name], dtype=np.int8)
        order = np.argsort(t, kind="stable")
        traces[name] = (t[order], v[order])
    return traces


def _level_low_windows(t, v):
    """[(t_start, t_end), ...] for every interval where the signal reads 0."""
    windows = []
    cur_start = None
    for i in range(len(t)):
        if v[i] == 0 and cur_start is None:
            cur_start = t[i]
        elif v[i] == 1 and cur_start is not None:
            windows.append((cur_start, t[i]))
            cur_start = None
    return windows


def _edges_in_window(t, v, t_start, t_end, rising):
    lo = np.searchsorted(t, t_start, side="left")
    hi = np.searchsorted(t, t_end, side="left")
    want = 1 if rising else 0
    return [t[i] for i in range(lo, hi) if v[i] == want]


def _value_at(t, v, when):
    """Value of a signal at time `when` (the level in force since its most
    recent transition at or before `when`); 0 if `when` predates any known
    transition (should not happen once $dumpvars' initial values are captured)."""
    idx = np.searchsorted(t, when, side="right") - 1
    return int(v[idx]) if idx >= 0 else 0


def _overlaps_low(t, v, t_start, t_end):
    if _value_at(t, v, t_start) == 0:
        return True
    lo = np.searchsorted(t, t_start, side="left")
    hi = np.searchsorted(t, t_end, side="left")
    return any(v[i] == 0 for i in range(lo, hi))


# ── SpiCapture ───────────────────────────────────────────────────────────
class SpiCapture:
    """Captures and decodes the DAC8562 SPI write bus via a Kingst LA1010.

    Frame format -- verified against BOTH the TI DAC8562 datasheet AND the
    firmware's own writeDAC8562()/writeDAC8562XY() (src/output/galvo_out.cpp:
    110-280), not assumed:

      24 bits total, MSB first, /SYNC (CS) active-low framing:
        byte0 (DB23-16) = 0x18 (DAC-A / X channel) or 0x19 (DAC-B / Y channel)
                           -- CMD=011 "write input register n and update DAC-n",
                           ADDR=000/001 (galvo_out.cpp:141, :236-237)
        byte1 (DB15-8)  = data high byte
        byte2 (DB7-0)   = data low byte
      SPI mode 1 (CPOL=0, CPHA=1), SCLK = 40 MHz (galvo_out.cpp:954-955:
        devcfg.clock_speed_hz = 40 * 1000 * 1000; devcfg.mode = 1).
        Standard SPI mode 1 timing (matches the TI DAC8562 datasheet's own
        "data latched into the input shift register on the falling edge of
        SCLK" convention): DIN changes on the rising edge, is sampled by the
        DAC on the falling edge. That's the DEFAULT assumption below, but
        it's cross-checked at runtime, not trusted blindly: decode() tries
        both edges and picks whichever one makes byte0 actually come out as
        0x18/0x19 (the only two values the firmware ever sends) across the
        capture -- see _choose_sample_edge(). The chosen edge and its
        validity fraction are reported in the summary so a mismatch is
        visible rather than silently decoding garbage.
      LDAC is tied to GND (synchronous mode) -- no separate LDAC pulse to
      track; the update happens on the frame's own 24th SCLK edge.

    Two channels = two separate 24-bit frames (two CS pulses), X then Y,
    per point (writeDAC8562XY() toggles CS between them).

    Optionally also probes the laser RGB TTL lines (GPIO7/8/21,
    PIN_LASER_R/G/B) via --r-channel/--g-channel/--b-channel, for checking
    blanking-edge alignment against DAC frame timing. When set, decode()
    adds laserR/laserG/laserB columns: the raw GPIO level in force at each
    frame's /SYNC-low edge. CLAUDE.md's inverted convention applies here
    too -- 1 = laser OFF (fail-safe default), 0 = laser ON.
    """

    FRAME_BITS = 24
    CMD_BYTE = {0x18: "A", 0x19: "B"}  # DAC-A=X, DAC-B=Y (galvo_out.cpp:141)
    # devcfg.clock_speed_hz, galvo_out.cpp:954 -- theoretical frame-rate ceiling.
    SCLK_HZ_THEORETICAL = 40_000_000

    def __init__(self, driver="kingst-la2016", samplerate_hz=100_000_000,
                 sclk_channel="CH0", din_channel="CH1", sync_channel="CH2",
                 sd_cs_channel=None, r_channel=None, g_channel=None, b_channel=None,
                 voltage_threshold=None, sigrok_cli=None):
        if samplerate_hz < 100_000_000:
            prWarn(f"samplerate {samplerate_hz/1e6:.1f} MHz is below the requested "
                   ">=100 MHz minimum for resolving a 40 MHz SCLK edge-accurately")
        self.driver = driver
        self.samplerate_hz = samplerate_hz
        self.sclk_channel = sclk_channel
        self.din_channel = din_channel
        self.sync_channel = sync_channel
        self.sd_cs_channel = sd_cs_channel
        # Optional laser RGB TTL probes -- GPIO7/8/21 (PIN_LASER_R/G/B). Not
        # part of the SPI bus itself; along for the ride so decode() can
        # report the blanking level in force at each DAC frame's timestamp,
        # for checking blanking-edge-to-DAC-frame alignment. Raw GPIO level
        # is reported as-is -- CLAUDE.md's inverted convention applies:
        # 1 = laser OFF (fail-safe default), 0 = laser ON.
        self.r_channel = r_channel
        self.g_channel = g_channel
        self.b_channel = b_channel
        # kingst-la2016's discrete voltage_threshold steps (confirmed via
        # `sigrok-cli -d kingst-la2016 --show`): 0.4/0.6/0.9/1.2/1.4(device
        # default)/2.0/2.5/4.0 V. ESP32-S3 GPIOs are 3.3V CMOS (ideal
        # threshold ~1.65V, i.e. between the 1.4V default and the 2.0V
        # step) -- left as the device default unless overridden, since both
        # neighbors decode 3.3V logic reliably in practice.
        self.voltage_threshold = voltage_threshold
        self.sigrok_cli = sigrok_cli

    def _channel_map_arg(self):
        parts = [f"{self.sclk_channel}=SCLK", f"{self.din_channel}=DIN", f"{self.sync_channel}=SYNC"]
        if self.sd_cs_channel is not None:
            parts.append(f"{self.sd_cs_channel}=SDCS")
        if self.r_channel is not None:
            parts.append(f"{self.r_channel}=LASER_R")
        if self.g_channel is not None:
            parts.append(f"{self.g_channel}=LASER_G")
        if self.b_channel is not None:
            parts.append(f"{self.b_channel}=LASER_B")
        return ",".join(parts)

    def capture(self, duration_s=None, num_samples=None, keep_raw_path=None):
        """Capture N samples or a fixed duration, then decode(). Exactly one
        of duration_s/num_samples must be given."""
        if (duration_s is None) == (num_samples is None):
            raise ValueError("capture() needs exactly one of duration_s or num_samples")
        with _tmp_or_named(keep_raw_path, suffix=".vcd") as vcd_path:
            config = f"samplerate={_fmt_rate(self.samplerate_hz)}"
            if self.voltage_threshold is not None:
                config += f":voltage_threshold={self.voltage_threshold}"
            args = [
                "-d", self.driver,
                "--config", config,
                "--channels", self._channel_map_arg(),
                "-O", "vcd", "-o", str(vcd_path),
            ]
            if duration_s is not None:
                args += ["--time", str(int(round(duration_s * 1000)))]
                timeout = duration_s + 30
            else:
                args += ["--samples", str(int(num_samples))]
                timeout = (num_samples / self.samplerate_hz) + 30
            _run_sigrok(args, timeout=timeout, sigrok_cli=self.sigrok_cli, driver=self.driver)
            return self.decode(vcd_path)

    def _choose_sample_edge(self, cs_windows, sclk_t, sclk_v, din_t, din_v, max_windows=2000):
        sample = cs_windows[:max_windows]
        scores = {}
        for rising in (False, True):
            valid = total = 0
            for (t_start, t_end) in sample:
                edges = _edges_in_window(sclk_t, sclk_v, t_start, t_end, rising=rising)
                if len(edges) != self.FRAME_BITS:
                    continue
                total += 1
                word = 0
                for e in edges:
                    word = (word << 1) | _value_at(din_t, din_v, e)
                if ((word >> 16) & 0xFF) in self.CMD_BYTE:
                    valid += 1
            scores["rising" if rising else "falling"] = (valid / total) if total else 0.0
        edge = "falling" if scores["falling"] >= scores["rising"] else "rising"
        if scores[edge] < 0.9:
            prWarn(f"sample-edge auto-detect only reached {scores[edge]*100:.1f}% valid "
                   f"frames on the '{edge}' edge (falling={scores['falling']*100:.1f}%, "
                   f"rising={scores['rising']*100:.1f}%) -- check channel mapping/wiring")
        return edge, scores

    def decode(self, vcd_path):
        """Decode a VCD capture into (DataFrame, summary dict)."""
        traces = _parse_vcd(vcd_path)
        for req in ("SCLK", "DIN", "SYNC"):
            if req not in traces:
                raise SigrokError(
                    f"capture is missing expected channel '{req}' in {vcd_path} -- check "
                    f"--sclk-channel/--din-channel/--sync-channel mapping (run "
                    f"`sigrok-cli -d {self.driver} --show` to list actual channel names)"
                )
        sclk_t, sclk_v = traces["SCLK"]
        din_t, din_v = traces["DIN"]
        sync_t, sync_v = traces["SYNC"]
        sdcs = traces.get("SDCS")
        laser_r = traces.get("LASER_R")
        laser_g = traces.get("LASER_G")
        laser_b = traces.get("LASER_B")

        cs_windows = _level_low_windows(sync_t, sync_v)
        if not cs_windows:
            raise SigrokError(
                "no /SYNC (CS) low pulses found in capture -- check wiring/channel mapping "
                "and that the DAC is actively being driven during the capture window"
            )

        edge_choice, edge_stats = self._choose_sample_edge(cs_windows, sclk_t, sclk_v, din_t, din_v)
        rising = edge_choice == "rising"

        rows = []
        malformed = 0       # frame didn't have exactly 24 SCLK edges while CS was low
        non_write = 0       # 24 edges decoded fine, but byte0 wasn't 0x18/0x19 (e.g. init cmds)
        sd_dropped = 0
        prev_end = None
        for (t_start, t_end) in cs_windows:
            edges = _edges_in_window(sclk_t, sclk_v, t_start, t_end, rising=rising)
            if len(edges) != self.FRAME_BITS:
                malformed += 1
                prev_end = t_end
                continue
            word = 0
            for e in edges:
                word = (word << 1) | _value_at(din_t, din_v, e)
            byte0 = (word >> 16) & 0xFF
            channel = self.CMD_BYTE.get(byte0)
            if channel is None:
                non_write += 1
                prev_end = t_end
                continue
            if sdcs is not None and _overlaps_low(sdcs[0], sdcs[1], t_start, t_end):
                sd_dropped += 1
                prev_end = t_end
                continue
            gap_us = ((t_start - prev_end) * 1e6) if prev_end is not None else float("nan")
            row = {
                "syncTimestamp": t_start,
                "channel": channel,
                "rawCode": np.uint16(word & 0xFFFF),
                "frameDurationUs": (t_end - t_start) * 1e6,
                "interFrameGapUs": gap_us,
            }
            # Laser TTL level in force at the frame's /SYNC-low edge -- raw
            # GPIO value, CLAUDE.md's inverted convention applies (1 = OFF,
            # 0 = ON). Only present if the corresponding --*-channel was set.
            if laser_r is not None:
                row["laserR"] = _value_at(laser_r[0], laser_r[1], t_start)
            if laser_g is not None:
                row["laserG"] = _value_at(laser_g[0], laser_g[1], t_start)
            if laser_b is not None:
                row["laserB"] = _value_at(laser_b[0], laser_b[1], t_start)
            rows.append(row)
            prev_end = t_end

        columns = ["syncTimestamp", "channel", "rawCode", "frameDurationUs", "interFrameGapUs"]
        for name, trace in (("laserR", laser_r), ("laserG", laser_g), ("laserB", laser_b)):
            if trace is not None:
                columns.append(name)
        df = pd.DataFrame(rows, columns=columns)
        if not df.empty:
            df["rawCode"] = df["rawCode"].astype(np.uint16)

        summary = self._summarize(df, malformed, non_write, sd_dropped, edge_choice, edge_stats)
        return df, summary

    def _summarize(self, df, malformed, non_write, sd_dropped, edge_choice, edge_stats):
        out = {
            "frameCount": len(df),
            "malformedFrameCount": malformed,
            "nonWriteFrameCount": non_write,
            "sdContentionDroppedCount": sd_dropped,
            "sampleEdge": edge_choice,
            "sampleEdgeValidityFraction": edge_stats,
        }
        if df.empty:
            out["note"] = "no valid DAC8562 write frames decoded"
            return out

        sync = df["syncTimestamp"].to_numpy()
        periods_us = np.diff(sync) * 1e6
        gaps_us = df["interFrameGapUs"].to_numpy()[1:]  # row 0's gap is NaN (no prior frame)
        theoretical_hz = self.SCLK_HZ_THEORETICAL / self.FRAME_BITS
        avg_hz = (1.0 / np.mean(periods_us) * 1e6) if len(periods_us) else float("nan")
        max_hz = (1.0 / np.min(periods_us) * 1e6) if len(periods_us) else float("nan")
        counts = df["channel"].value_counts().to_dict()
        out.update({
            "avgFrameRateHz": avg_hz,
            "maxFrameRateHz": max_hz,
            "theoreticalMaxFrameRateHz": theoretical_hz,
            "avgPctOfTheoretical": (avg_hz / theoretical_hz * 100.0) if theoretical_hz else float("nan"),
            "interFrameGapJitterUsStd": float(np.std(gaps_us)) if len(gaps_us) else float("nan"),
            "channelCounts": counts,
            # X+Y are two frames per galvo point -- cross-checks against the
            # device's own /api/state points_per_sec (see galvos-output-rate
            # -ceiling project note: 43kpps firmware setting = 100% delivered).
            "estimatedPointsPerSec": (avg_hz / 2.0) if counts.get("A") and counts.get("B") else float("nan"),
        })
        return out


# ── AnalogCapture ────────────────────────────────────────────────────────
class AnalogCapture:
    """Captures galvo analog X/Y output via a Hantek 6022BE, and extracts
    ZV-shaper ringing parameters from a step response.

    Field names (ring_freq_hz, ring_damping_ratio) match optimizeGalvo.py's
    measure-resonance workflow so results from either tool are directly
    comparable/interchangeable as firmware ZV-shaper inputs.
    """

    def __init__(self, driver="hantek-6xxx", samplerate_hz=1_000_000,
                 ch1_channel="CH1", ch2_channel="CH2", vdiv=None, sigrok_cli=None):
        self.driver = driver
        self.samplerate_hz = samplerate_hz
        self.ch1_channel = ch1_channel
        self.ch2_channel = ch2_channel
        self.vdiv = vdiv
        self.sigrok_cli = sigrok_cli

    def capture(self, duration_s, channels=("x", "y"), keep_raw_path=None):
        """Returns {'t': ndarray seconds, 'x': ndarray volts?, 'y': ndarray volts?,
        'samplerate_hz': float} -- only the requested channel keys are present."""
        wanted = list(channels)
        for c in wanted:
            if c not in ("x", "y"):
                raise ValueError(f"unknown analog channel '{c}', expected 'x' and/or 'y'")
        if not wanted:
            raise ValueError("no channels requested")

        # Always request BOTH channels from sigrok-cli, even if the caller
        # only wants one: confirmed live that enabling only CH2 crashes
        # sigrok-cli outright (STATUS_ACCESS_VIOLATION / exit 0xC0000005) --
        # this scope's ADC samples both channels simultaneously and the
        # hantek-6xxx driver mishandles a single-enabled-channel request.
        # _parse_csv() then only returns the columns actually asked for.
        chan_map = [f"{self.ch1_channel}=X", f"{self.ch2_channel}=Y"]
        vdiv_args = []
        if "x" in wanted:
            if self.vdiv is not None:
                vdiv_args += ["--channel-group", self.ch1_channel,
                              "--config", f"vdiv={_fmt_vdiv(self.vdiv)}"]
        if "y" in wanted:
            if self.vdiv is not None:
                vdiv_args += ["--channel-group", self.ch2_channel,
                              "--config", f"vdiv={_fmt_vdiv(self.vdiv)}"]

        with _tmp_or_named(keep_raw_path, suffix=".csv") as csv_path:
            # vdiv is confirmed (`sigrok-cli -d hantek-6xxx --channel-group
            # CH1 --show`) to be a PER-CHANNEL-GROUP config, not a global
            # key -- must be set via --channel-group before --channels, one
            # pair of flags per requested channel.
            args = ["-d", self.driver, "--config", f"samplerate={_fmt_rate(self.samplerate_hz)}"]
            args += vdiv_args
            args += [
                "--channels", ",".join(chan_map),
                "--time", str(int(round(duration_s * 1000))),
                "-O", "csv", "-o", str(csv_path),
            ]
            _run_sigrok(args, timeout=duration_s + 30, sigrok_cli=self.sigrok_cli, driver=self.driver)
            result = self._parse_csv(csv_path, wanted)
            actual_s = (len(result["t"]) - 1) / self.samplerate_hz if len(result["t"]) > 1 else 0.0
            # Confirmed live on real hardware (hantek-6xxx, libsigrok
            # 0.6.0-git-883c2ac): --time is NOT honored reliably by this
            # driver -- a 3000ms request returned ~1.31s of samples, a
            # 5000ms request returned ~2.10s (both ~42-44% of asked-for
            # duration). Root cause not pinned down (driver marks
            # multi-channel analog capture "untested"); rather than
            # silently handing back a short capture, warn loudly so a
            # caller relying on `duration_s` worth of data notices.
            if actual_s < 0.9 * duration_s:
                prWarn(f"requested {duration_s:.3f}s but only captured {actual_s:.3f}s "
                       f"({len(result['t'])} samples) -- hantek-6xxx driver does not "
                       "reliably honor --time; re-run with a longer --duration if you "
                       "need the full window")
            return result

    def _parse_csv(self, csv_path, wanted):
        # sigrok-cli's csv output module labels analog columns by UNIT
        # ("V"), not by the --channels remap name, and repeats that same
        # label for every channel -- confirmed live (a real X,Y capture's
        # raw header came back "V,V", both literally the string "V", so
        # pandas would dedup them to "V"/"V.1" -- neither matches "X"/"Y").
        # Channel identity therefore has to come from COLUMN ORDER instead:
        # sigrok-cli preserves the exact order channels were given in
        # --channels (its own "; Channels (N/N): X, Y" comment line
        # confirms this). capture() always requests X then Y (see its own
        # comment on the single-channel driver crash), so the raw file
        # always has exactly these two columns in this order regardless of
        # `wanted` -- only the returned dict is filtered down to `wanted`.
        text = Path(csv_path).read_text(encoding="ascii", errors="replace")
        lines = [ln for ln in text.splitlines() if ln.strip() and not ln.lstrip().startswith(";")]
        if len(lines) < 2:
            raise SigrokError(f"sigrok-cli CSV output {csv_path} has no data rows")
        data_lines = lines[1:]  # lines[0] is the unit-label header row (e.g. "V,V")

        both = ["x", "y"]
        df = pd.read_csv(io.StringIO("\n".join(data_lines)), header=None)
        if len(df.columns) != len(both):
            raise SigrokError(
                f"expected {len(both)} data column(s) (X, Y), got {len(df.columns)} in "
                f"{csv_path} -- capture may have failed partway; inspect the raw file "
                "with --keep-raw"
            )
        df.columns = both

        n = len(df)
        result = {"t": np.arange(n, dtype=np.float64) / self.samplerate_hz,
                  "samplerate_hz": float(self.samplerate_hz)}
        for c in wanted:
            result[c] = df[c].to_numpy(dtype=np.float64)
        return result

    @staticmethod
    def extract_ring_params(t, signal, step_index=None, min_peaks=2):
        """Time-domain step-response fit: locates the commanded step,
        finds the decaying oscillation's positive peaks, and fits a
        log-decrement line (least squares over all peaks, not just a
        single pair -- more noise-robust) to recover the damped frequency
        and decay rate, then converts back to the UNDAMPED natural
        frequency the firmware's ZV shaper actually wants (ring_freq_hz =
        wn/2pi -- same relation point_optimizer.cpp's ZV shaper and
        optimizeGalvo.py's measure-resonance both use).
        """
        t = np.asarray(t, dtype=np.float64)
        signal = np.asarray(signal, dtype=np.float64)
        if len(t) != len(signal):
            raise ValueError("t and signal must be the same length")
        if step_index is None:
            step_index = int(np.argmax(np.abs(np.diff(signal))))

        tail_t = t[step_index:]
        tail_v = signal[step_index:]
        if len(tail_v) < 10:
            raise ValueError("not enough samples after the detected step to analyze ringing")

        tail_len = max(5, len(tail_v) // 10)
        settled = float(np.median(tail_v[-tail_len:]))
        osc = tail_v - settled

        peak_idx = [i for i in range(1, len(osc) - 1)
                    if osc[i] > osc[i - 1] and osc[i] > osc[i + 1] and osc[i] > 0]
        if len(peak_idx) < min_peaks:
            raise ValueError(
                f"found only {len(peak_idx)} oscillation peak(s) after the step; need "
                f">= {min_peaks} to fit ring_freq_hz/ring_damping_ratio (capture longer, "
                "or the response may already be critically/over-damped)"
            )

        peak_t = tail_t[peak_idx]
        peak_v = osc[peak_idx]
        damped_freq_hz = 1.0 / float(np.mean(np.diff(peak_t)))
        slope, _intercept = np.polyfit(peak_t, np.log(peak_v), 1)
        decay_rate = -float(slope)  # zeta * wn  [1/s]
        damped_omega = 2.0 * math.pi * damped_freq_hz
        wn = math.sqrt(max(decay_rate, 0.0) ** 2 + damped_omega ** 2)
        zeta = (decay_rate / wn) if wn > 0 else 0.0
        return {
            "ring_freq_hz": wn / (2.0 * math.pi),
            "ring_damping_ratio": zeta,
            "damped_freq_hz": damped_freq_hz,
            "num_peaks_used": len(peak_idx),
            "step_index": step_index,
            "method": "time_domain_log_decrement",
        }

    @staticmethod
    def extract_ring_params_fft(t, signal, step_index=None, freq_band_hz=None):
        """FFT-based cross-check: peak frequency + -3dB bandwidth around it.
        Q = fRes/bandwidth, ring_damping_ratio = 1/(2Q) -- same formula
        optimizeGalvo.py's measure-resonance Bode-curve fit documents.

        Like extract_ring_params(), this isolates the post-step tail (and
        subtracts its settled value) before transforming -- the step edge
        itself has a broad, dominant low-frequency spectrum (a step's FFT
        rolls off as ~1/f) that swamps the actual ringing peak if the raw
        signal is transformed directly.
        """
        t = np.asarray(t, dtype=np.float64)
        signal = np.asarray(signal, dtype=np.float64)
        if len(t) != len(signal):
            raise ValueError("t and signal must be the same length")
        if step_index is None:
            step_index = int(np.argmax(np.abs(np.diff(signal))))

        tail_t = t[step_index:]
        tail_v = signal[step_index:]
        n = len(tail_v)
        if n < 16:
            raise ValueError("need at least 16 samples after the detected step for an "
                              "FFT-based ring estimate")
        dt = float(np.mean(np.diff(tail_t)))
        if dt <= 0:
            raise ValueError("non-monotonic or degenerate time axis")

        tail_len = max(5, n // 10)
        settled = float(np.median(tail_v[-tail_len:]))
        sig = (tail_v - settled) * np.hanning(n)
        spec = np.fft.rfft(sig)
        freqs = np.fft.rfftfreq(n, d=dt)
        mag = np.abs(spec)

        mask = freqs > 0
        if freq_band_hz is not None:
            lo, hi = freq_band_hz
            mask &= (freqs >= lo) & (freqs <= hi)
        if not np.any(mask):
            raise ValueError("no FFT bins in the requested frequency band")

        idx = np.where(mask)[0][np.argmax(mag[mask])]
        peak_freq = float(freqs[idx])
        peak_mag = mag[idx]
        half_power = peak_mag / math.sqrt(2.0)
        li = idx
        while li > 0 and mag[li] > half_power:
            li -= 1
        ri = idx
        while ri < len(mag) - 1 and mag[ri] > half_power:
            ri += 1
        bandwidth_hz = float(freqs[ri] - freqs[li])
        if bandwidth_hz <= 0:
            raise ValueError(
                "could not resolve a -3dB bandwidth around the FFT peak -- "
                "capture a longer window for finer frequency resolution"
            )
        q = peak_freq / bandwidth_hz
        return {
            "ring_freq_hz": peak_freq,
            "ring_damping_ratio": 1.0 / (2.0 * q),
            "q_factor": q,
            "bandwidth_hz": bandwidth_hz,
            "step_index": step_index,
            "method": "fft_bandwidth",
        }


# ── CLI ──────────────────────────────────────────────────────────────────
def _build_argparser():
    p = argparse.ArgumentParser(
        prog="sigrokCapture.py",
        description="GalvOS hardware measurement via sigrok-cli: DAC8562 SPI bus decode "
                    "(Kingst LA1010) and galvo analog X/Y capture (Hantek 6022BE).",
    )
    p.add_argument("--mode", required=True, choices=["spi", "analog", "scan", "show"])
    p.add_argument("--duration", type=float, help="capture duration in seconds (spi/analog)")
    p.add_argument("--samples", type=int, help="capture sample count instead of --duration (spi only)")
    p.add_argument("--output", type=Path, help="output CSV path (spi/analog)")
    p.add_argument("--samplerate", type=float,
                    help="samplerate in Hz (default: 100e6 for spi, 1e6 for analog)")
    p.add_argument("--driver", help="sigrok driver id override "
                    "(default: kingst-la2016 for spi, hantek-6xxx for analog)")
    p.add_argument("--sigrok-cli", dest="sigrok_cli", help="explicit path to sigrok-cli(.exe)")
    p.add_argument("--keep-raw", type=Path,
                    help="also keep the raw sigrok-cli capture (VCD/CSV) at this path")

    spi = p.add_argument_group("spi mode")
    spi.add_argument("--sclk-channel", default="CH0")
    spi.add_argument("--din-channel", default="CH1")
    spi.add_argument("--sync-channel", default="CH2")
    spi.add_argument("--sd-cs-channel", default=None,
                      help="optional SD-CS probe channel id; frames overlapping SD-CS low "
                          "are dropped as bus contention (no-op on current hardware -- SD "
                          "moved to an independent SPI3 bus, see CLAUDE.md)")
    spi.add_argument("--voltage-threshold", default=None,
                      help="LA1010 input logic threshold in volts. Confirmed discrete steps "
                          "(sigrok-cli -d kingst-la2016 --show): 0.4/0.6/0.9/1.2/1.4 "
                          "(device default)/2.0/2.5/4.0. Left at the device default unless set.")
    spi.add_argument("--r-channel", default=None,
                      help="optional laser R TTL probe channel (GPIO7/PIN_LASER_R); adds a "
                          "laserR column (raw GPIO level, 1=OFF/0=ON) to the decoded frames")
    spi.add_argument("--g-channel", default=None,
                      help="optional laser G TTL probe channel (GPIO8/PIN_LASER_G); adds a "
                          "laserG column (raw GPIO level, 1=OFF/0=ON) to the decoded frames")
    spi.add_argument("--b-channel", default=None,
                      help="optional laser B TTL probe channel (GPIO21/PIN_LASER_B); adds a "
                          "laserB column (raw GPIO level, 1=OFF/0=ON) to the decoded frames")

    analog = p.add_argument_group("analog mode")
    analog.add_argument("--channels", default="x,y", help="analog channels to capture (x,y)")
    analog.add_argument("--ch1-channel", default="CH1")
    analog.add_argument("--ch2-channel", default="CH2")
    analog.add_argument("--vdiv", default=None,
                          help="Hantek volts/div, applied to both requested channels via "
                              "--channel-group. This 6022BE's confirmed discrete steps "
                              "(sigrok-cli -d hantek-6xxx --channel-group CH1 --show): "
                              "100mV, 250mV (device default), 500mV, 1V. Bare numbers are "
                              "read as volts, e.g. '1' -> '1 V'; '500m'/'500mV' also accepted.")
    return p


def main(argv=None):
    args = _build_argparser().parse_args(argv)
    try:
        if args.mode == "scan":
            print(scan(driver=args.driver, sigrok_cli=args.sigrok_cli))
            return 0

        if args.mode == "show":
            if not args.driver:
                prErr("--mode show requires --driver <id>")
                return 2
            print(show(args.driver, sigrok_cli=args.sigrok_cli))
            return 0

        if args.mode == "spi":
            if args.duration is None and args.samples is None:
                prErr("--mode spi requires --duration or --samples")
                return 2
            if not args.output:
                prErr("--mode spi requires --output")
                return 2
            cap = SpiCapture(
                driver=args.driver or "kingst-la2016",
                samplerate_hz=args.samplerate or 100_000_000,
                sclk_channel=args.sclk_channel, din_channel=args.din_channel,
                sync_channel=args.sync_channel, sd_cs_channel=args.sd_cs_channel,
                r_channel=args.r_channel, g_channel=args.g_channel, b_channel=args.b_channel,
                voltage_threshold=args.voltage_threshold, sigrok_cli=args.sigrok_cli,
            )
            prInfo(f"capturing SPI bus via {cap.driver} @ {cap.samplerate_hz/1e6:.1f} MHz ...")
            df, summary = cap.capture(duration_s=args.duration, num_samples=args.samples,
                                        keep_raw_path=args.keep_raw)
            df.to_csv(args.output, index=False)
            prOk(f"wrote {len(df)} frames to {args.output}")
            for k, v in summary.items():
                print(f"  {k}: {v}")
            if df.empty:
                prWarn("zero valid DAC8562 write frames decoded -- check wiring/channel mapping")
                return 1
            return 0

        if args.mode == "analog":
            if args.duration is None:
                prErr("--mode analog requires --duration")
                return 2
            if not args.output:
                prErr("--mode analog requires --output")
                return 2
            wanted = [c.strip().lower() for c in args.channels.split(",") if c.strip()]
            cap = AnalogCapture(
                driver=args.driver or "hantek-6xxx",
                samplerate_hz=args.samplerate or 1_000_000,
                ch1_channel=args.ch1_channel, ch2_channel=args.ch2_channel,
                vdiv=args.vdiv, sigrok_cli=args.sigrok_cli,
            )
            prInfo(f"capturing analog {wanted} via {cap.driver} @ {cap.samplerate_hz/1e6:.3f} MHz ...")
            result = cap.capture(args.duration, channels=wanted, keep_raw_path=args.keep_raw)
            out_df = pd.DataFrame({"timeSec": result["t"]})
            for c in wanted:
                out_df[c] = result[c]
            out_df.to_csv(args.output, index=False)
            prOk(f"wrote {len(out_df)} samples to {args.output}")
            return 0

        prErr(f"unknown mode '{args.mode}'")
        return 2

    except SigrokNotFoundError as e:
        prErr(str(e))
        return 3
    except DeviceNotFoundError as e:
        prErr(str(e))
        return 4
    except CaptureTimeoutError as e:
        prErr(str(e))
        return 5
    except SigrokError as e:
        prErr(str(e))
        return 1
    except KeyboardInterrupt:
        prWarn("interrupted")
        return 130


if __name__ == "__main__":
    sys.exit(main())
