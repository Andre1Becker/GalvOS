# Chapter 8 — Contributing

> **Status:** Skeleton — content to be filled in Session 8

## Contents
- [Getting Started as a Contributor](#getting-started-as-a-contributor)
- [Code Style](#code-style)
- [Patch Workflow](#patch-workflow)
- [Commit Messages](#commit-messages)
- [Testing](#testing)
- [Areas That Need Help](#areas-that-need-help)

---

## Getting Started as a Contributor

<!-- TODO: fork, clone, branch, PR -->

---

## Code Style

- All code, comments, logs, and commit messages in **English**
- camelCase for variables and functions
- Clean, readable code over clever code
- Large buffers (>16 KB) go in PSRAM via `ps_malloc` / `heap_caps_malloc(MALLOC_CAP_SPIRAM)`
- All `JsonDocument` instances use `SpiRamAllocator`

---

## Patch Workflow

<!-- TODO: git clone --depth 1 fresh, Python str.replace with assert, git diff, 
     git apply --check on second fresh clone, node --check JS, HTML div balance -->

---

## Commit Messages

English, imperative mood, structured body:

```
Add ringing compensation to optimizer pipeline

- Implements accel clamp as Phase 4 stage
- New params: ring_freq_hz, ring_damping_ratio
- Wired into all 4 liveOptimizerConfig() callers
- Verified on JY-15K-BL at 30kpps
```

---

## Testing

<!-- TODO: host-compile test harness (g++ -std=gnu++11, cfg_stub.h shim) -->
<!-- TODO: JS syntax: node --check on all script blocks -->
<!-- TODO: HTML: div balance check -->

---

## Areas That Need Help

From the current roadmap:

- SD card / galvo interaction bug
- Text animation fixes (Bounce, Typewriter, Star Wars Scroll)
- Calibration pattern fixes
- Auto-tuning via global-shutter camera input
- kpps history graph on dashboard
- Pattern fixes: Chaos Bouncer, Hypotrochoid, Phyllotaxis
- Point-only mode optimizer profile selection
