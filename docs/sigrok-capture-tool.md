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
- `kingst-la2016` does not reliably honor `--time` either, and it's far
  worse than `hantek-6xxx` above, and channel-count-dependent: confirmed
  live 2026-08-26, a 3-channel capture (SCLK/DIN/SYNC only) delivered
  ~42ms of a 50ms request (84%), adding one more channel (`--r-channel`)
  dropped that to ~4.5ms (9%), and adding all three RGB channels dropped
  it further to ~1ms (2%) — the real captured window shrinks hard as soon
  as more than 3 channels are enabled, independent of what's requested.
  `--samples` doesn't dodge this either: it just hangs outright (30s+,
  never returns any data) with `--r-channel`/etc. set, at any sample
  count tried (200k samples through 20M). Root cause not pinned down
  (driver logs `High USB stream bandwidth: 600Mbps` in the failing
  cases) — treat any capture with laser-TTL channels enabled as
  effectively limited to a ~1-5ms real window regardless of
  `--duration`, and plan on firing many quick captures back-to-back
  (each invocation restarts the acquisition, so each one samples a
  different real-time slice) rather than trusting one longer request.

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

## Resolved: `/api/debug/hw` "stuck at 0x8000" was `calib_thresh_test`, not a debug-output bug

Confirmed live 2026-08-26: `POST /api/debug/hw` with a non-zero `x`/`y`
produced no coordinate change at all — DAC held at `0x8000` (center)
across every capture, indistinguishable from the arm-gate case above but
with `laser_armed == true`. Root cause, found by reading `galvo_out.cpp`'s
output-task branch order: `gState.calib_thresh_test` is checked *before*
`s_hw_debug_active` (`galvo_out.cpp:678` vs `:713`), and forces
`writeDAC8562XY(0x8000, 0x8000)` unconditionally while active — left
`true` from an earlier session, it silently overrides `/api/debug/hw`
with no error or indication anywhere in `/api/state`/`/api/status` (only
`/api/debug/hw`'s own `GET` reports `active`, and that reports the
*debug* output's own active flag, not the thing overriding it). Fix:
`POST /api/calib-thresh-test {"active":false}` before trusting
`/api/debug/hw`. Worth checking this whenever `/api/debug/hw` output
looks frozen and `laser_armed` is already confirmed true.

**Separate, unresolved:** `/api/debug/hw`'s `x` field also looks
sign-inverted — sending `x:3000` and `x:-3000` alternately both produced
DAC code `29768` (the code for `x:-3000`) once the override above was
cleared. Not investigated further; flagged for whoever needs
`/api/debug/hw`'s `x` axis next.

## Investigation: RGB blanking-edge alignment vs. DAC frame timing (2026-08-26)

Original goal (`docs/10-known-issues-and-todos.md`'s blanking-alignment
item): probe GPIO7/8/21 alongside the SPI bus and check whether a
color-channel's blank/unblank edge lands on the same DAC frame as the
corresponding coordinate write.

That framing turned out to be electrically unanswerable as stated. LEDC
is a free-running hardware PWM timer — its output edges are on an
independent clock from the bit-banged SPI loop, so a raw GPIO edge
cannot land "on" a specific SPI frame by construction, PWM chatter or
not. The firmware already knows this: `galvo_out.cpp:56-74` documents
LEDC's own turn-on/turn-off latency (up to 2 PWM periods, ~40µs) and
compensates for it with `LASER_ON_HOLD_TICKS`/`LASER_OFF_HOLD_TICKS`
(2 output ticks each) — holding the DAC at the old position on a
lit→blank transition, and holding the laser off on a blank→lit
transition, so LEDC has time to physically catch up before the galvo
starts moving again ([galvo_out.cpp:839-853](../src/output/galvo_out.cpp#L839-L853),
[:875-897](../src/output/galvo_out.cpp#L875-L897)). The real question is
therefore whether that 2-tick hold margin is actually sufficient at the
device's current point rate, not whether an edge lines up with a frame.

At the point rate measured live during this session (~43.5-45kpps,
~23µs/point → 2 ticks ≈ 46µs), the margin over the documented ~40µs LEDC
latency is positive but thin — worth revisiting as point rates climb
toward the measured ~46.9kpps output-rate ceiling.

Captured real SPI+`laserR` data from an armed, running preset (Grid 3x3)
confirms the hold mechanism is doing *something* real: a 53-sample-long
solid-blank stretch was found (`X` held flat while `Y` swept, i.e. a
real blanked travel/approach), interrupted by exactly two single-frame
"laser on" blips in the middle of it. That is evidence-shaped for
exactly the streaking defect this item was chasing, but Grid 3x3 turned
out to have real per-point behavior richer than a simple
lit→blank→lit model — line junctions get a multi-tick dwell with what
looks like a deliberate brief re-light while the galvo is still parked
at the corner, not a single clean travel gap. That makes it impossible
to tell apart "this preset intentionally re-lights corners" from "this
is the streaking bug" using electrical data alone, without also knowing
the point buffer's intended per-point blank flags.

**Status: unresolved, needs a different test setup, not more captures of
this preset.** Two ways forward for a future session:

1. Pick a preset with two clearly disconnected shapes and one genuinely
   simple travel jump (no shared vertices/dwell), so a lit→blank→lit
   sandwich is unambiguous — `Cross + Lines` (preset idx 10) or
   `X Shape Lines` (idx 11) are more promising candidates than Grid 3x3
   (idx 12) was.
2. Add temporary firmware-side logging of the intended per-point blank
   flag (or expose it over the existing DAC debug-log snapshot
   mechanism) so captured electrical data can be checked against ground
   truth instead of inferred from geometry alone.

Also confirmed live: `calib-thresh-test`'s channel-select path
(`POST /api/calib-thresh-test {"active":true,"channel":1..3}`) bypasses
gain/gamma/dimmer entirely and gives a genuinely flat (non-PWM-chattering)
GPIO level for a "channel off" state — useful for future signal-integrity
tests, but it does **not** exercise `s_laser_on_hold`/`s_laser_off_hold`
(that logic only runs in the normal ring-buffer point path), so it can't
be used to validate this specific item either.
