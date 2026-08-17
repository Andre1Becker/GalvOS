#!/usr/bin/env python3
"""Generate docs/index.html (the GalvOS browser simulator) from data/index.html.

The simulator IS the WebUI: same markup, same CSS, same JS bundle, with a thin
layer on top -- a title marker, the SIMULATOR banner, the preview canvas, the
preset-engine/mock-backend script (sim_engine.js) and the launch overlay
(overlay.html). Keeping docs/index.html a generated transform of
data/index.html, instead of a hand-maintained fork, is what makes re-syncing
the demo to a new UI version a rerun of this script rather than a diff review.

Usage (from the repo root):

    .venv/Scripts/python.exe scripts/simulator/build_simulator.py

After a WebUI change, bump SIM_UI_VERSION (and FW_VERSION/FW_COMMIT when the
preset generators were re-synced with src/patterns/preset_patterns.cpp) at the
top of sim_engine.js, rerun this, then run check_simulator.py.
"""
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent

src = (ROOT / "data/index.html").read_text(encoding="utf-8")
engine = (HERE / "sim_engine.js").read_text(encoding="utf-8")
overlay = (HERE / "overlay.html").read_text(encoding="utf-8")


def replace_once(text: str, old: str, new: str, what: str) -> str:
    """Substitute an anchor that must appear exactly once.

    Every edit below keys off real markup in data/index.html; if the WebUI
    moves or reworks one of those anchors, this fails loudly instead of
    silently emitting a simulator missing that piece.
    """
    hits = text.count(old)
    if hits != 1:
        sys.exit(f"build_simulator: anchor for {what!r} matched {hits} times, expected 1")
    return text.replace(old, new)


def grab(text: str, pattern: str, what: str) -> str:
    m = re.search(pattern, text)
    if not m:
        sys.exit(f"build_simulator: could not read {what}")
    return m.group(1)


out = src

# ── 1. Title ────────────────────────────────────────────────────────────────
out = replace_once(
    out,
    '<title id="page-title">GalvOS &mdash; Laser Controller</title>',
    '<title id="page-title">GalvOS &mdash; Simulator</title>',
    "title",
)

# ── 2. CSS: banner visibility + drawer offset ───────────────────────────────
# The banner adds height to the sticky top bar, so the tablet drawer offsets by
# the measured bar height (--topbar-h, published by sim_engine.js) instead of
# the WebUI's fixed 56px.
out = replace_once(
    out,
    "#sidebar.drawer-open { width: 200px; position: fixed; top: 56px;",
    "#sidebar.drawer-open { width: 200px; position: fixed; top: var(--topbar-h, 56px);",
    "drawer offset",
)
out = replace_once(
    out,
    "  .field-grid { grid-template-columns: repeat(2, minmax(0, 1fr)); }\n}\n</style>",
    "  .field-grid { grid-template-columns: repeat(2, minmax(0, 1fr)); }\n}\n"
    "/* Simulator banner -- hidden below 768px, where the top bar collapses to\n"
    "   the connection/ARM pills and has no room for it. */\n"
    "@media (max-width: 767px) { .tb-sim-banner { display: none; } }\n</style>",
    "sim banner css",
)

# ── 3. SIMULATOR pill in the top bar ────────────────────────────────────────
out = replace_once(
    out,
    '    <h1 class="brand">GalvOS</h1>\n  </div>\n  <div class="tb-center">',
    '    <h1 class="brand">GalvOS</h1>\n  </div>\n'
    '  <div class="tb-sim-banner"><span class="pill warn" '
    'style="font-size:0.72rem;letter-spacing:0.08em">&#127916; SIMULATOR</span></div>\n'
    '  <div class="tb-center">',
    "sim pill",
)

# ── 4. Preview canvas at the top of the Presets tab ─────────────────────────
canvas_block = """          <div style="display:flex;justify-content:center;margin-bottom:16px">
            <div style="width:100%;max-width:480px;border:1px solid var(--border);overflow:hidden;border-radius:var(--radius-lg)">
              <canvas id="sim-scan" width="700" height="700" style="width:100%;aspect-ratio:1;background:#000;display:block"></canvas>
              <div style="padding:5px 12px 6px;font-size:0.72rem;display:flex;gap:16px;color:var(--text-dim);font-family:var(--font-mono)">
                <span>&#127916; Simulator</span>
                <span id="sim-pts" style="color:var(--accent)">-- pts</span>
                <span id="sim-fps" style="color:var(--accent)">-- fps</span>
              </div>
            </div>
          </div>
"""
out = replace_once(
    out,
    '        <div>\n          <div class="card">\n            <h2>Global Controls</h2>',
    '        <div>\n' + canvas_block + '          <div class="card">\n            <h2>Global Controls</h2>',
    "sim canvas",
)

# ── 5. Simulator script, immediately before the WebUI bundle ────────────────
# The bundle is data/index.html's second (and last) <script>. The sim layer has
# to be parsed first so window.fetch is already patched when the bundle's
# DOMContentLoaded handlers start polling /api/*.
if out.count("<script>") != 2:
    sys.exit("build_simulator: expected exactly 2 <script> tags in data/index.html")
bundle_open = out.rindex("<script>")
out = out[:bundle_open] + "<script>\n" + engine + "</script>\n" + out[bundle_open:]

# ── 6. Launch overlay: hooks at the end of the bundle + its markup ──────────
disclaimer_js = """  showDisclaimerIfNeeded();
});

function showDisclaimerIfNeeded() {
  if (sessionStorage.getItem('galvos_disclaimer_seen')) return;
  sessionStorage.setItem('galvos_disclaimer_seen', '1');
  var overlay = document.getElementById('disclaimer-overlay');
  if (overlay) overlay.classList.remove('hidden');
}

function closeDisclaimer() {
  var overlay = document.getElementById('disclaimer-overlay');
  if (overlay) {
    overlay.style.opacity = '0';
    overlay.style.transform = 'translateY(-12px)';
    setTimeout(function() { overlay.classList.add('hidden'); }, 280);
  }
}
</script>"""
out = replace_once(
    out,
    "  enhanceSliders(document);\n});\n</script>",
    "  enhanceSliders(document);\n" + disclaimer_js,
    "disclaimer hooks",
)

# Version line on the launch card -- read straight from the sources, so the card
# can never advertise a version the page does not actually contain.
fw_version = grab((ROOT / "platformio.ini").read_text(encoding="utf-8"),
                  r'LASER_FW_VERSION=\\"([0-9.]+)\\"', "firmware version")
ui_version = grab(src, r"var UI_VERSION = '([0-9.]+)'", "UI version")
sim_version = grab(engine, r"const SIM_VERSION\s*=\s*'([0-9.]+)'", "simulator version")
for placeholder, value in (("__FW_VERSION__", fw_version),
                           ("__UI_VERSION__", ui_version),
                           ("__SIM_VERSION__", sim_version)):
    overlay = replace_once(overlay, placeholder, value, placeholder)

for name, wanted, found in (
        ("FW_VERSION", fw_version,
         grab(engine, r"const FW_VERSION\s*=\s*'([0-9.]+)'", "engine FW_VERSION")),
        ("SIM_UI_VERSION", ui_version,
         grab(engine, r"const SIM_UI_VERSION\s*=\s*'([0-9.]+)'", "engine SIM_UI_VERSION"))):
    if wanted != found:
        sys.exit(f"build_simulator: sim_engine.js {name} is {found}, sources say {wanted} "
                 "-- update sim_engine.js before rebuilding")

out = replace_once(out, "</body>\n</html>", overlay + "</body>\n</html>", "overlay")

dst = ROOT / "docs/index.html"
dst.write_text(out, encoding="utf-8", newline="\n")
print(f"{dst.relative_to(ROOT)}: FW {fw_version} / UI {ui_version} / SIM {sim_version}, "
      f"{len(out)} bytes, {out.count(chr(10)) + 1} lines")
