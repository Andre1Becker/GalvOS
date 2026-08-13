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

- [x] **4. OTA has no in-progress arm guard** — fixed: new
  `ota_update::uploadInProgress()` (returns `s_fw_state.active ||
  s_fs_state.active`) checked in `/api/arm`'s handler before `arm==true` is
  allowed through to `safety::requestArm()`; rejects with 409 while a flash
  write is active. Disarm (`arm==false`) is left unguarded — always safe
  direction. `pio run` (esp32-s3-devkitc-1) succeeds, no new warnings.
  File: [src/net/ota_update.cpp:349-351](src/net/ota_update.cpp#L349-L351),
  [src/net/web_ui.cpp:1170-1173](src/net/web_ui.cpp#L1170-L1173)

- [x] **5. Laser TTL fail-safe doc says pull-down, hardware needs pull-up**
  — fixed: both comments corrected to pull-**up**, with the inverted-6N137
  rationale (HIGH = laser OFF) spelled out inline so it can't drift back.
  File: [include/pinmap.h:67-70](include/pinmap.h#L67-L70),
  [src/output/galvo_out.cpp:863-866](src/output/galvo_out.cpp#L863-L866)

- [x] **6. Parked fallback keeps sending fake heartbeat** — fixed: parked
  loop no longer calls `safety::subsystemHeartbeat(0)`; calls
  `safety::emergencyStop()` once before parking instead (forces
  `PIN_LASER_ENABLE` LOW, clears `laser_armed`/arm request). `pio run`
  (esp32-s3-devkitc-1) succeeds, no new warnings. Not reproducible without
  forcing both the PSRAM and internal-DRAM `s_frame` allocations to fail —
  logic re-derived by hand.
  File: [src/patterns/pattern_engine.cpp:1398-1404](src/patterns/pattern_engine.cpp#L1398-L1404)

---

## Correctness

- [x] **7. `resolveMasterDimmer()` UI-wins instead of `max()`** — fixed:
  now computes `dmx_dim` from DMX CH1 unconditionally and returns
  `max(dmx_dim, ui_dim)` instead of short-circuiting on `ui_dim>0`.
  File: [src/patterns/pattern_engine.cpp:262-275](src/patterns/pattern_engine.cpp#L262-L275)

- [x] **8. Paint-by-Finger double-dimming** — fixed: removed the RGB
  pre-scale loop before `pushFrame()`; `dim` is now only used to decide
  push-vs-blank (matches Text Mode's existing gating pattern).
  Dimming happens exactly once, in `galvoTask`.
  File: [src/patterns/pattern_engine.cpp:1675-1682](src/patterns/pattern_engine.cpp#L1675-L1682)

- [x] **9. Curve Mode double-dimming** — fixed, same change as #8.
  File: [src/patterns/pattern_engine.cpp:1797-1802](src/patterns/pattern_engine.cpp#L1797-L1802)

- [x] **10. Gray `(200,200,200)` DMX color fallback violates 255/0 rule**
  — fixed: `resolveColor()`'s fallback branch and `genPattern()`'s >99
  branch both now default to `255,255,255`.
  File: [src/patterns/pattern_engine.cpp:272-282,298](src/patterns/pattern_engine.cpp#L272-L282)

- [x] **11. Duplicate `/api/calib-pattern/stop` route registration** — fixed:
  deleted the dead second registration (missing the `ui_master_dimmer`
  release the first has). `pio run` (esp32-s3-devkitc-1) succeeds, no new
  warnings.
  File: [src/net/web_ui.cpp:2766-2776](src/net/web_ui.cpp#L2766-L2776)
  (surviving copy)

- [x] **12. Encoder dimmer writes get stomped every frame** — fixed: both
  `MODE_DIMMER`'s rotate handler and the long-press toggle now target
  `gState.ui_master_dimmer` (same knob `/api/ui-control` drives) instead of
  `gState.master_dimmer`, which `pattern_engine` overwrites every tick from
  `resolveMasterDimmer()`.
  File: [src/control/encoder.cpp:70-79,121-124](src/control/encoder.cpp#L70-L79) +
  [src/patterns/pattern_engine.cpp:1447](src/patterns/pattern_engine.cpp#L1447)

---

## Real-Time Violations

- [x] **13. Blocking log call inside Core-1 hot loop** — fixed: moved the
  `ESP_LOGI`/`LOG_I` calls out of `sendRawCommandImpl()` (runs inline in
  `galvoTask`) into `sendRawCommand()` (the Core-0 caller, which already
  blocks/polls up to 200ms for the result) — recomputes the tx bytes there
  from the already-available `cmd3`/`addr3`/`data` instead of threading
  `esp_err_to_name(err)` back through shared state, so the message is now
  "-> OK"/"-> FAIL" instead of the IDF error string. `sendRawCommandImpl()`
  itself no longer touches logging at all.
  File: [src/output/galvo_out.cpp:1308-1358](src/output/galvo_out.cpp#L1308-L1358)

- [x] **14. Unbounded SPI2 busy-wait, no timeout, no heartbeat GPIO** —
  fixed (software side; `PIN_HEARTBEAT` re-wire is a hardware change, out of
  scope here and not attempted). New `spi2WaitClear()` bounds each of the
  4 `UPDATE`/`USR` CMD-bit polls in `writeDAC8562XY()` to 200us
  (`esp_timer_get_time()`, already used elsewhere in this IRAM task) instead
  of spinning forever. On timeout: CS released HIGH (bus fail-safe) and
  `rgbOff()` called immediately (cheap ledcWrite, already used inline
  elsewhere in this hot loop) — position can't be trusted after a wedged
  transfer, so the beam is blanked rather than left lit at a stale point.
  The fault only increments a `volatile` counter in the IRAM path (calling
  ESP_LOGx there was rejected — same flash-cache-disabled hazard as #13);
  a new deferred logger `logSpi2FaultIfPending()` (Core 0, polled from
  `temp_monitor.cpp`'s existing loop next to `logDacDebugIfPending()`)
  does the actual rate-limited warning log. `spi2TimeoutCount()` getter
  added to `galvo_out.h` for future diagnostics/UI surfacing (not wired
  into `/api/state` here — out of scope, `overflowCount()` next to it
  isn't wired in either).
  `pio run` (esp32-s3-devkitc-1) succeeds, no new warnings. Not verified
  against a real SPI2 glitch/brown-out (no hardware attached this session,
  and that fault is not practically reproducible without one) — logic
  re-derived by hand against the existing register-poll sequence and the
  file's own IRAM/no-logging conventions.
  File: [src/output/galvo_out.cpp:183-275](src/output/galvo_out.cpp#L183-L275),
  deferred logger at [1391-1409](src/output/galvo_out.cpp#L1391-L1409)

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
