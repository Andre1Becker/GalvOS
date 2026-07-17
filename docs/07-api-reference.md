# Chapter 7 — API Reference

> **Status:** Skeleton — content to be filled in Session 7

## Contents
- [Base URL & Format](#base-url--format)
- [Config Endpoints](#config-endpoints)
- [Preset Endpoints](#preset-endpoints)
- [Calibration Endpoints](#calibration-endpoints)
- [Text Endpoints](#text-endpoints)
- [Pattern Control Endpoints](#pattern-control-endpoints)
- [System Endpoints](#system-endpoints)
- [Notes on Route Registration Order](#notes-on-route-registration-order)

---

## Base URL & Format

<!-- TODO: http://<device-ip>/api/, JSON request/response, chunked PSRAM responses -->

---

## Config Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/config` | Read all RuntimeConfig values |
| POST | `/api/config` | Write RuntimeConfig values (partial update OK) |

<!-- TODO: full field list, types, valid ranges -->

---

## Preset Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/presets` | List all presets with index, name, class, SVG |
| POST | `/api/preset` | Activate a preset by index |

<!-- TODO: response format, PRESET_COUNT -->

---

## Calibration Endpoints

| Method | Path | Description |
|--------|------|-------------|
| POST | `/api/calib-pattern/start` | Start a calibration pattern |
| POST | `/api/calib-pattern/stop` | Stop calibration pattern (must be registered before prefix routes) |

<!-- TODO: pattern types, channel parameter -->

---

## Text Endpoints

| Method | Path | Description |
|--------|------|-------------|
| POST | `/api/text` | Set text content and animation |
| GET | `/api/text/vertices` | Get pre-computed text vertices (must be registered before prefix routes) |

---

## Pattern Control Endpoints

| Method | Path | Description |
|--------|------|-------------|
| POST | `/api/stop` | Stop current pattern output |
| POST | `/api/optimizer` | Update optimizer parameters live |

---

## System Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/status` | Telemetry: kpps, point count, temperatures, heap |
| POST | `/api/restart` | Restart ESP32 |
| POST | `/api/nvs-reset` | Reset all NVS config to defaults |

---

## Notes on Route Registration Order

<!-- TODO: ESPAsyncWebServer requires specific routes (/api/calib-pattern/stop, 
     /api/text/vertices) registered BEFORE prefix-matching routes, else 404 -->
