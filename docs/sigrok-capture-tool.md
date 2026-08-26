# sigrokCapture.py

Hardware measurement tool wrapping `sigrok-cli` for two instruments:

- **Kingst LA1010** (logic analyzer) — probes the DAC8562 SPI bus
  (SCLK/GPIO12, DIN/GPIO11, /SYNC/GPIO10) and decodes DAC8562 write
  frames into a `[syncTimestamp, channel, rawCode, frameDurationUs,
  interFrameGapUs]` DataFrame. Optionally also probes the laser RGB TTL
  lines (GPIO7/8/21) via `--r-channel`/`--g-channel`/`--b-channel`, adding
  `laserR`/`laserG`/`laserB` columns (raw GPIO level at each frame's
  `/SYNC`-low edge, inverted convention: 1=OFF, 0=ON) — for checking
  blanking-edge alignment against DAC frame timing.
- **Hantek 6022BE** (oscilloscope) — probes galvo analog X/Y at
  J_GALVO Pin2/Pin4, for ring-parameter extraction (ZV-shaper
  verification) and general waveform capture.

## Usage

```bash
sigrokCapture.py --mode spi    --duration 3 --output spi.csv
sigrokCapture.py --mode spi    --duration 3 --output spi.csv \
    --r-channel CH4 --g-channel CH5 --b-channel CH6   # + laser TTL
sigrokCapture.py --mode analog --channels x,y --duration 3 --output xy.csv
sigrokCapture.py --mode scan
sigrokCapture.py --mode show --driver kingst-la2016
```

Pass `--sigrok-cli <path>` if `sigrok-cli.exe` isn't on `PATH`.

## Confirmed against real hardware (2026-08-24)

- Frame format: 24-bit MSB-first, cmd byte `0x18`=DAC-A/X, `0x19`=DAC-B/Y,
  sampled falling-edge — 12k+ real frames decoded with 0 malformed/
  non-write, matches firmware's own reported `points_per_sec` within 0.1%.
- LA1010 needs WinUSB (Zadig) + KingstVIS-extracted firmware; driver id
  `kingst-la2016`; channels `CH0`-`CH15`.
- Hantek needs `hantek-6xxx` driver (not `fx2lafw`); channels `CH1`/`CH2`;
  `vdiv` is a per-channel-group config, not global; single-channel-only
  capture crashes the driver (`AnalogCapture` always requests both
  channels and filters afterward).

## Known limitations

- `hantek-6xxx` does not reliably honor `--time` — requested durations came
  back at ~42-52% of what was asked (2000ms→~1.05s, 3000ms→~1.31s,
  5000ms→~2.10s actual). `AnalogCapture.capture()` detects this and prints
  a warning rather than silently returning a short capture. Over-request
  duration if you need a guaranteed minimum window.
- `AnalogCapture`'s `--samplerate`/`_fmt_rate()` doesn't work against
  `hantek-6xxx` as shipped: this driver only accepts samplerate as a
  discrete `"N unit"` string (`sigrok-cli -d hantek-6xxx --show` lists
  `48/30/24/16/8/4/1 MHz, 500/200/100 kHz`), not a bare Hz integer —
  `--config samplerate=1000000` fails with `not applicable` even though
  1 MHz is on the list. Confirmed live 2026-08-24: this driver also
  refuses to accept a `--config samplerate=...` and a `--channel-group
  ... --config vdiv=...` in the *same* `sigrok-cli` invocation at all
  (whichever comes second errors, either `not applicable` or `superfluous
  option "--channel-group"`). Worked around live by dropping the
  samplerate override (captures ran at the device's current/default
  8 MHz) and setting `vdiv` on only one channel-group per invocation —
  two separate captures (one per axis) instead of one combined X+Y
  capture. `AnalogCapture` itself hasn't been patched for this yet.
- Setting `vdiv` via `--channel-group` on only one of CH1/CH2 leaves the
  other at its default (250 mV/div) — confirmed to clip hard (~20-24% of
  samples pinned at the ±1.25 V rail) for a galvo signal in the low-volt
  range this rig actually produces. Don't trust the non-configured
  channel's amplitude in that capture; read it from the other axis's
  dedicated capture instead.

## Confirmed use case: vertex-0 undershoot (`star`, Vector profile)

2026-08-24: used to root-cause `optimizer-range-audit-2026-08-17.md`'s
long-open `star` scale-undershoot defect (see that doc's Open Item #2 and
2026-08-24 changelog entry) — four sessions of camera/Optuna work
couldn't localize it past "somewhere outside `OptimizerLiveConfig`'s
search space." One `SpiCapture` + two axis-split `AnalogCapture`s pinned
it to the analog domain in a single session: the DAC is commanded
correctly at the defective vertex, the galvo just doesn't get there.

## Resolved: "stuck at 0x8000" is the arm gate, not generate/cache latency

The `SpiCapture` in the session above initially caught 12.7k frames
parked at DAC code `0x8000` (center) with the `star` calib-cam pattern
selected and reporting real (non-degenerate) frame content over
`/api/state`. Root cause, confirmed by reading `galvo_out.cpp`: with
`gState.laser_armed == false`, output is unconditionally forced to
`writeDAC8562XY(0x8000, 0x8000)` regardless of what the pattern engine is
generating — a deliberate safety gate, not a bug, and not
generate/cache-latency-related as first guessed. Any capture meant to see
real geometry needs the laser armed first (`POST /api/arm` body `"1"`,
or the WebUI Arm control) — check `/api/state`'s `laser_armed` field
before capturing, not just `calib_active`/`frame_n`.
