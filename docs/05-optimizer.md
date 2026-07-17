# Chapter 5 — The Optimizer

> **Status:** Skeleton — content to be filled in Session 5

## Contents
- [Why an Optimizer?](#why-an-optimizer)
- [Pipeline Overview](#pipeline-overview)
- [Phase 1 — Primitive Generation](#phase-1--primitive-generation)
- [Phase 2 — Transform](#phase-2--transform)
- [Phase 3 — Resample](#phase-3--resample)
- [Phase 4 — Corner Dwell](#phase-4--corner-dwell)
- [Phase 4 — Blanking (S-Curve)](#phase-4--blanking-s-curve)
- [Phase 4 — Velocity Clamp](#phase-4--velocity-clamp)
- [Phase 4 — Acceleration Clamp (Ringing Compensation)](#phase-4--acceleration-clamp-ringing-compensation)
- [Phase 4 — DAC Output](#phase-4--dac-output)
- [PPS Scaling](#pps-scaling)
- [Optimizer Profiles](#optimizer-profiles)
- [The Three Pillars](#the-three-pillars)
- [Effective Values (opt_eff_*)](#effective-values-opt_eff_)
- [Pattern Cache](#pattern-cache)
- [Parameter Reference](#parameter-reference)

---

## Why an Optimizer?

<!-- TODO: galvos are inertial mechanical systems; naive point streaming causes ringing, 
     visible corners, blur on blanked jumps. Explain the physical constraints. -->

## Pipeline Overview

<!-- TODO: diagram: Primitive → Transform → Resample → Corner Dwell → Blanking 
     → Velocity Clamp → Acceleration Clamp → DAC -->
<!-- TODO: note on pipeline order being non-negotiable (Phase 1–4 complete) -->

## Phase 1 — Primitive Generation

<!-- TODO: how patterns generate raw LaserPoints (int16_t x/y, uint8_t r/g/b) -->
<!-- TODO: coordinate system: x↑=right, y↑=down, DAC(0x8000,0x8000)=center -->

## Phase 2 — Transform

<!-- TODO: invert_x, invert_y, swap_xy, scale, offset -->

## Phase 3 — Resample

<!-- TODO: adaptive density: pts_per_1000_units, min/max segment pts -->
<!-- TODO: resample_enabled flag, resample_spacing_units -->

## Phase 4 — Corner Dwell

<!-- TODO: corner angle detection, min/max corner pts, buildWfChains() requirement -->

## Phase 4 — Blanking (S-Curve)

<!-- TODO: blank_samples, min_blank_samples, blank_pts_per_1000_units -->
<!-- TODO: distance-proportional blank sample count, settle ticks carved from budget -->
<!-- TODO: emitBlankJump() -->

## Phase 4 — Velocity Clamp

<!-- TODO: vel_clamp_enabled, max_step_units -->

## Phase 4 — Acceleration Clamp (Ringing Compensation)

<!-- TODO: accel_clamp_enabled, max_accel_units, ringing_comp_enabled -->
<!-- TODO: ring_freq_hz, ring_damping_ratio -->

## Phase 4 — DAC Output

<!-- TODO: updateSnapshot() once per frame boundary (not per-point) -->
<!-- TODO: 22µs/point budget at 30kpps -->

## PPS Scaling

<!-- TODO: applyPpsScaling(cfg, rated_kpps, output_kpps) -->
<!-- TODO: pts_per_1000_units ∝ 1/r, max_step_units ∝ r, max_accel_units ∝ r² -->
<!-- TODO: r = rated_kpps / output_kpps -->

## Optimizer Profiles

<!-- TODO: PresetClass enum (Simple/Curves/ThreeD/Scenes) -->
<!-- TODO: gOptimizerProfiles[4], automatic switching in setPreset() via presetClassOf() -->
<!-- TODO: Smart Defaults button: auto-computes from opt_max_pts_per_frame + galvo_kpps -->

## The Three Pillars

<!-- TODO: Pillar 1 — Adaptive Density -->
<!-- TODO: Pillar 2 — S-Curve Blanking -->
<!-- TODO: Pillar 3 — Ringing Compensation -->
<!-- NOTE: all three hardware-verified -->

## Effective Values (opt_eff_*)

<!-- TODO: what WebUI shows as effective values after PPS scaling, why this matters -->

## Pattern Cache

<!-- TODO: isStaticPreset allowlist, single-slot PSRAM cache -->
<!-- TODO: gPatternCacheGen++ required on any optimizer/kpps change -->
<!-- TODO: known limit: opt_max_pts_per_frame: 1300 — no optical improvement above this -->

## Parameter Reference

<!-- TODO: full table of all optimizer parameters, defaults, units, valid range, effect -->
