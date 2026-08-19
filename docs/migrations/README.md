# Config Migrations

Full-device config snapshots (`/api/backup` format) taken around changes that
alter how stored values are interpreted. Restore one with the Configuration
tab's **Restore** button, or:

```bash
curl -X POST -H "Content-Type: application/json" \
     --data-binary @<file>.json http://<device>/api/restore
```

The device validates every value before applying anything, then reboots. A
single rejected key aborts the whole restore untouched.

---

## 2026-08-19 — `galvo_rated_kpps` 45 → 15 (fw v6.79.0)

| File | Contents |
| --- | --- |
| `2026-08-19-rated-kpps-45-rollback.json` | State **before** the migration (`rated_kpps: 45`). Restore this to undo. |
| `2026-08-19-rated-kpps-15-applied.json` | State **after** (`rated_kpps: 15`, profiles rescaled). |

### Why

`galvo_rated_kpps` held 45 while the JY-15K-BL's datasheet figure — and the
firmware default in `config.h` — is 15. The field is the anchor for
`applyPpsScaling()`, so the wrong anchor meant every **community preset**
(which ships raw optimizer values with no anchor of its own, see
`community_presets.cpp`) was interpreted with `r` 3× too large: density 3× too
low, velocity ceiling 3× too permissive, acceleration ceiling 9× too permissive.

### What was done

The rescale factor is `k = rated_old / rated_new = 3`, and the output rate
**cancels out** — so this is an exact change of units, not a re-tune. With
`effective = raw × r^e`, holding `effective` constant requires
`raw_new = raw_old × k^e`:

| Field | `e` | Factor |
| --- | --- | --- |
| `pts_per_1000_units` | −1 | ÷3 |
| `blank_pts_per_1000_units` | −1 | ÷3 |
| `resample_spacing_units` | +1 | ×3 |
| `min_spacing_units` | +1 | ×3 |
| `max_spacing_units` | +1 | ×3 |
| `max_step_units` | +1 | ×3 |
| `max_accel_units` | +2 | ×9 |

Verified across all 8 profiles: zero values saturate their firmware
`constrain()` bounds, and effective values are preserved to 1.25e-06 relative
(float rounding only).

### Firmware changes shipped alongside

Both were needed because they depended on the old anchor and would have broken
on their own:

- `text_renderer.cpp` — the per-glyph density constant `K` was scaled by
  `1/ppsRatio(rated, output)`, silently tying an absolute empirical calibration
  to the anchor. At the new anchor that tripled text density, straight back into
  the documented "only ~8 chars render" bug. Now scaled against
  `TEXT_DENSITY_REF_KPPS`, making it anchor-independent.
- `data/index.html` — the exit-angle warning compared the DAC sample clock
  against the mechanical ILDA derating (`15 × 8/20 = 6` kpps) and would have
  been pinned on permanently. Replaced with an informational line; the same
  category error the sample-rate autotune used to make.
