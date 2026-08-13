# GalvOS Code Review — State / Fix Backlog

Generated from full 6-category code review (Safety-Critical, Real-Time, Correctness,
Memory, Architecture, Style) across 7 finder passes. One item per checkbox — each is
sized to be picked up standalone in its own session without needing this file's
surrounding context re-derived.

Process top to bottom (already ordered by blast radius). Check off `[x]` and add a
one-line note (commit hash / reason skipped) when done.

---

- [x] **3. ILDA loader Pass1/Pass2 mismatch → PSRAM heap overflow** — fixed:
  Pass 2 now has the identical `default:` skip branch as Pass 1 (unknown
  format → seek `npts*8` and `continue`, no `s_frames`/`pool_offset`/`fi`
  advance). `pio run` (esp32-s3-devkitc-1) succeeds, only this file
  recompiled, no new warnings. Not verified against a real crafted `.ild`
  with an unsupported format section (no hardware/SD attached this session)
  — logic re-derived by hand against Pass 1's existing skip branch.
  File: [src/ilda/ilda_player.cpp:153-166](src/ilda/ilda_player.cpp#L153-L166) vs
  [202-260](src/ilda/ilda_player.cpp#L202-L260), pool alloc at 177-178
  Problem: Pass 1 (sizes `s_point_pool`) skips unknown ILDA format codes; Pass 2
  (fills pool) has no matching `default:` skip and processes them anyway,
  advancing `pool_offset` past the allocated size.
  Risk: `.ild` file with an unsupported/reserved format section between normal
  frames → heap buffer overflow in PSRAM, corrupting adjacent structures.
  Fix: give Pass 2 the identical `default: seek-and-continue` branch Pass 1 has.

- [ ] **4. OTA has no in-progress arm guard**
  File: [src/net/ota_update.cpp:40-56](src/net/ota_update.cpp#L40-L56),
  [src/net/web_ui.cpp:1164-1172](src/net/web_ui.cpp#L1164-L1172)
  Problem: OTA blocks arm only once at upload start (`index==0`); `/api/arm` has
  no OTA-in-progress check.
  Risk: arm request mid-flash re-enables `PIN_LASER_ENABLE` while `Update.write()`
  streams to flash.
  Fix: add `s_fw_state.active || s_fs_state.active` check in `/api/arm` handler.

- [ ] **5. Laser TTL fail-safe doc says pull-down, hardware needs pull-up**
  File: [include/pinmap.h:67-70](include/pinmap.h#L67-L70),
  [src/output/galvo_out.cpp:863-865](src/output/galvo_out.cpp#L863-L865)
  Problem: comments claim 10kΩ pull-**down** as fail-safe; actual inverted
  polarity (HIGH=off) requires pull-**up**, matching CLAUDE.md and
  `docs/01-introduction.md:220`.
  Risk: doc-only today, but a landmine if board is ever re-populated from these
  comments — floating GPIO would default to laser-ON.
  Fix: correct both comments to pull-up; cross-check against actual board.

- [ ] **6. Parked fallback keeps sending fake heartbeat**
  File: [src/patterns/pattern_engine.cpp:1386-1395](src/patterns/pattern_engine.cpp#L1386-L1395)
  Problem: after frame-buffer alloc failure, parked loop still calls
  `safety::subsystemHeartbeat(0)` forever → watchdog thinks pattern engine is alive.
  Risk: defeats "pattern engine missing → laser off" guarantee on this path.
  Fix: stop sending heartbeat once parked, or call `safety::emergencyStop()`
  before parking.

---

## Correctness

- [ ] **7. `resolveMasterDimmer()` UI-wins instead of `max()`**
  File: [src/patterns/pattern_engine.cpp:262-270](src/patterns/pattern_engine.cpp#L262-L270)
  Problem: `if (ui_dim>0) return ui_dim;` short-circuits instead of
  `max(dmxResolved, ui_dim)` — violates the documented master-dimmer rule.
  Risk: stale nonzero `ui_master_dimmer` silently caps brightness below what a
  DMX console commands (e.g. blackout ignored).
  Fix: `return max(dmxResolved, ui_dim);`

- [ ] **8. Paint-by-Finger double-dimming**
  File: [src/patterns/pattern_engine.cpp:1675-1689](src/patterns/pattern_engine.cpp#L1675-L1689)
  Problem: pre-scales RGB by `master_dimmer`, then `galvoTask` scales again by
  `dimEff` → brightness².
  Risk: at 50% dimmer, actual output ≈25%.
  Fix: push raw 255/0 values; let `galvoTask` dim once.

- [ ] **9. Curve Mode double-dimming**
  File: [src/patterns/pattern_engine.cpp:1797-1810](src/patterns/pattern_engine.cpp#L1797-L1810)
  Problem/Risk/Fix: same bug and same fix as #8.

- [ ] **10. Gray `(200,200,200)` DMX color fallback violates 255/0 rule**
  File: [src/patterns/pattern_engine.cpp:272-282,298](src/patterns/pattern_engine.cpp#L272-L282)
  Problem: `resolveColor()`/`genPattern()` default to gray instead of 255/0.
  Risk: large chunks of the DMX color-channel range render dim gray instead of a
  defined color; also compounds with master_dimmer.
  Fix: default to `255,255,255`; use `col_override` for tinting instead.

- [ ] **11. Duplicate `/api/calib-pattern/stop` route registration**
  File: [src/net/web_ui.cpp:2763-2772](src/net/web_ui.cpp#L2763-L2772) vs
  [2835-2841](src/net/web_ui.cpp#L2835-L2841)
  Problem: registered twice; second copy is dead (ESPAsyncWebServer: first
  registration wins) and missing the `ui_master_dimmer` release the first has.
  Risk: any future edit at the dead 2835 spot is silently inert — already caused
  divergence once.
  Fix: delete the duplicate at 2835-2841.

- [ ] **12. Encoder dimmer writes get stomped every frame**
  File: [src/control/encoder.cpp:70-73,122](src/control/encoder.cpp#L70-L73) +
  [src/patterns/pattern_engine.cpp:1441](src/patterns/pattern_engine.cpp#L1441)
  Problem: encoder writes to `gState.master_dimmer`; `pattern_engine`
  unconditionally overwrites `master_dimmer` from `resolveMasterDimmer()` every
  tick.
  Risk: standalone (no-DMX) front-panel dimmer knob is non-functional.
  Fix: encoder should target `ui_master_dimmer` like the WebUI does, not
  `master_dimmer` directly.

---

## Real-Time Violations

- [ ] **13. Blocking log call inside Core-1 hot loop**
  File: [src/output/galvo_out.cpp:557-561,1275-1278](src/output/galvo_out.cpp#L557-L561)
  Problem: `sendRawCommandImpl()` (serviced inline in `galvoTask`) calls
  `ESP_LOGI`/`LOG_I`, violating the file's own no-logging-in-hot-path rule
  (see comment at 153-156).
  Risk: raw-DAC-command from Config tab during playback stalls `galvoTask` for a
  UART write + up to 5ms mutex wait — ≈225 missed ticks at 45kpps, audible/visible
  stutter.
  Fix: defer raw-command logging to Core 0, or drop it for this debug path.

- [ ] **14. Unbounded SPI2 busy-wait, no timeout, no heartbeat GPIO**
  File: [src/output/galvo_out.cpp:216-228](src/output/galvo_out.cpp#L216-L228),
  `PIN_HEARTBEAT` commented out at [include/pinmap.h:197](include/pinmap.h#L197)
  Problem: SPI2 `UPDATE`/`USR` status-bit polling has no timeout; `galvoTask` is
  also intentionally excluded from TWDT (main.cpp:494-511) because of its
  busy-wait design, and the intended hang detector pin is disabled.
  Risk: an SPI2 glitch/brown-out hangs Core 1 indefinitely; only recovery is
  `safety::task()`'s 500ms subsystem-heartbeat timeout.
  Fix: add a bounded retry/timeout on the polling loop, or re-wire
  `PIN_HEARTBEAT` if hardware allows.

---

## Architecture

- [ ] **15. Two independently-drifting JSON status builders**
  File: [src/net/web_ui.cpp:3826-3871](src/net/web_ui.cpp#L3826-L3871) (`/api/status`,
  hand-rolled snprintf) vs [593-707](src/net/web_ui.cpp#L593-L707)
  (`buildStateJson()`/`/api/state`, the WebUI's real source)
  Problem: different field names, smaller field set on `/api/status`; only
  consumer is the external camera-autotuning script (`docs/06-camera-autotuning.md:113`).
  Risk: future state-field changes only touch `buildStateJson()`, leaving the
  autotuning script silently reading stale/incomplete data.
  Fix: make `/api/status` a thin subset-projection of `buildStateJson()`, or
  point the script at `/api/state`.

- [ ] **16. Legacy DMX shape generator bypasses the optimizer**
  File: [src/patterns/pattern_engine.cpp:76-142,284-298](src/patterns/pattern_engine.cpp#L76-L142),
  call site 2024
  Problem: `genCircle/genSquare/genStar` use fixed point counts, no easing/
  ZV-shaping/density scaling — the exact defect class Pillars 2/3 were built to fix.
  Risk: DMX-only sessions (no WebUI preset) get lower-quality jumps that don't
  track `galvo_kpps`/size; future optimizer improvements silently skip this path.
  Fix: migrate to `PathSegment` + `optimizer::optimize()` like
  `preset_patterns.cpp` already does.

- [ ] **17. Duplicator/RadialCopy hand-roll a third blank-jump implementation**
  File: [src/patterns/pattern_engine.cpp:793-867,884-956](src/patterns/pattern_engine.cpp#L793-L867)
  Problem: own linear, un-eased blank jump, alongside
  `optimizer::emitBlankJump()`/`reshapeBlankRun()`.
  Risk: Duplicator/Kaleidoscope effects get jerkier, more ringing-prone jumps
  than identical presets without them; jump-quality bugs now need fixing in
  three places.
  Fix: route both through `optimizer::emitBlankJump()`/`buildBlankTrajectory()`.

---

## Style

- [ ] **18. Magic GPIO literal `13` instead of `PIN_DAC_CLR_N`**
  File: [src/output/galvo_out.cpp:245-246](src/output/galvo_out.cpp#L245-L246)
  Fix: replace `13` with `PIN_DAC_CLR_N` (used correctly at 891-892).

- [ ] **19. Dead code disguised as comment**
  File: [src/output/galvo_out.cpp:304](src/output/galvo_out.cpp#L304)
  Fix: delete `// Removed for debugging static volatile bool s_ledc_active = false;`.

- [ ] **20. German text in JSON API response**
  File: [src/net/web_ui.cpp:3670](src/net/web_ui.cpp#L3670)
  Fix: translate `"verwende /api/preset"` to English.

- [ ] **21. "KRIT" (WebUI log) vs "CRITICAL" (serial log) mismatch**
  File: [src/sensors/temp_monitor.cpp:318](src/sensors/temp_monitor.cpp#L318)
  Fix: use "CRITICAL" in both.

- [ ] **22. Garbled bilingual comment in `galvoTask()`**
  File: [src/output/galvo_out.cpp:524](src/output/galvo_out.cpp#L524)
  Fix: rewrite in plain English.

- [ ] **23. snake_case locals in `applyTransform()`**
  File: [src/patterns/pattern_engine.cpp:312-313](src/patterns/pattern_engine.cpp#L312-L313)
  Fix: rename `wave_amp`/`wave_phase` → `waveAmp`/`wavePhase`.

---

## Suggested Session Grouping

Each bracket is a reasonable single-session batch (touches the same file/area,
bounded diff):

2. **Session B (ILDA):** #3 — isolated file, needs a crafted test `.ild`.
3. **Session C (dimmer pipeline):** #7, #8, #9, #10, #12 — all touch
   `pattern_engine.cpp` dimmer/color resolution, worth doing together since
   they interact.
4. **Session D (OTA/arm guard + doc fix):** #4, #5 — small, independent, low risk.
5. **Session E (hot-path real-time):** #13, #14 — `galvo_out.cpp`, needs careful
   build+bench validation per CLAUDE.md real-time rules.
6. **Session F (route/watchdog cleanup):** #6, #11 — small independent deletions.
7. **Session G (architecture):** #15, #16, #17 — larger refactors, budget more time,
   do after correctness/safety items land to avoid rebasing.
8. **Session H (style sweep):** #18–#23 — batch together, trivial diff, good
   "cooldown" session.
