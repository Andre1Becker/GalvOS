#!/usr/bin/env python3
"""Capture + redact GalvOS WebUI screenshots for docs/04-ui-guide.md.

Read-only: never arms the laser, never sends a write API call. Uses Playwright
to drive headless Chromium, then Pillow to black out anything that looks like
an IP, a MAC, a credential field, or a WiFi/hostname field before saving.

Usage:
    pip install playwright pillow pytesseract
    playwright install chromium
    python scripts/capture_screenshots.py
"""
import io
import re
import sys
import time
from pathlib import Path

try:
    from playwright.sync_api import sync_playwright, TimeoutError as PWTimeoutError
except ImportError:
    print("Missing dependency: pip install playwright && playwright install chromium", file=sys.stderr)
    sys.exit(1)

try:
    from PIL import Image, ImageDraw
except ImportError:
    print("Missing dependency: pip install pillow", file=sys.stderr)
    sys.exit(1)

# pytesseract is optional -- the DOM-locator redaction path below doesn't need
# it, but importing it here surfaces a clear error if a future OCR fallback
# is added and the dependency was never installed.
try:
    import pytesseract  # noqa: F401
except ImportError:
    pytesseract = None

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
BASE_URL = "http://172.18.30.30"  # device is DHCP-reachable at this IP; also try http://galvOS.local or http://192.168.4.1
API_TOKEN = ""                     # X-Auth token -- left empty, this script only ever performs GETs
OUT_DIR = "docs/assets/screenshots"

REDACT_COLOR = (0x1A, 0x1A, 0x1A)
DESKTOP_VIEWPORT = {"width": 1280, "height": 800}
MOBILE_VIEWPORT = {"width": 390, "height": 844}
NAV_WAIT_MS = 1000
DASHBOARD_SETTLE_MS = 45000  # Dashboard telemetry/charts populate asynchronously; wait 30-60s after it becomes visible

IP_RE = re.compile(r"\b\d{1,3}(\.\d{1,3}){3}\b")
MAC_RE = re.compile(r"([0-9A-Fa-f]{2}[:\-]){5}[0-9A-Fa-f]{2}")

# Elements redacted unconditionally regardless of their current text/value.
SENSITIVE_SELECTORS = [
    'input[type="password"]',
    '[id*="ssid" i]', '[name*="ssid" i]', '[placeholder*="ssid" i]',
    '[id*="password" i]', '[name*="password" i]', '[placeholder*="password" i]',
    '[id*="pass" i]', '[name*="pass" i]',
    '[id*="token" i]', '[class*="token" i]',
    '[id*="auth" i]', '[class*="auth" i]',
    '[id*="key" i]', '[class*="key" i]',
    '[id*="secret" i]', '[class*="secret" i]',
    '[id*="hostname" i]', '[class*="hostname" i]',
]

# Card root is `.card:has(h2:text-is("<heading>"))`. Tab id must match
# data/index.html's NAV_TABS ids (note: Calibration's real tab id is "calib",
# not "calibration").
TAB_SHOTS = [
    ("Dashboard", "dashboard", "tab_dashboard.png"),
    ("Presets", "presets", "tab_presets.png"),
    ("Preset Manager", "preset-manager", "tab_preset_manager.png"),
    ("DMX Live", "dmx", "tab_dmx.png"),
    ("Text", "text", "tab_text.png"),
    ("Paint", "paint", "tab_paint.png"),
    ("ILDA / SD", "ilda", "tab_ilda.png"),
    ("Calibration", "calib", "tab_calibration.png"),
    ("Optimizer", "optimizer", "tab_optimizer.png"),
    ("Projection", "projection", "tab_projection.png"),
    ("Thermal", "thermal", "tab_thermal.png"),
    ("Log", "log", "tab_log.png"),
    ("Configuration", "config", "tab_config.png"),
]

CARD_SHOTS = [
    # (tab id, card <h2> heading text, filename)
    ("dashboard", "Safety & Arm", "card_safety.png"),
    ("dashboard", "Telemetry", "card_telemetry.png"),
    ("dashboard", "CPU Load", "card_cpu.png"),
    ("dashboard", "Temperature History", "card_temp.png"),
    ("dashboard", "Galvo Output Rate", "card_kpps.png"),
    ("dashboard", "Frame Composition", "card_frame.png"),
    ("dashboard", "System", "card_system.png"),
    ("presets", "Global Controls", "card_global_controls.png"),
    ("presets", "Presets", "card_preset_grid.png"),
    ("presets", "Sequencer", "card_sequencer.png"),
    ("calib", "Projection Zone", "card_zone.png"),
    ("log", "Memory Viewer", "card_memory.png"),
    ("config", "Debug", "card_debug.png"),
]

results = []   # (filename, label) in capture order, for the final MD block
ok_count = 0
skip_count = 0


def warn(msg):
    print(f"WARNING: {msg}", file=sys.stderr)


def find_sensitive_boxes(page):
    """DOM-locator based redaction targets: explicit sensitive fields + any
    leaf element whose text/value looks like an IP or a MAC address."""
    boxes = []
    for sel in SENSITIVE_SELECTORS:
        try:
            loc = page.locator(sel)
            for i in range(loc.count()):
                el = loc.nth(i)
                if el.is_visible():
                    box = el.bounding_box()
                    if box:
                        boxes.append(box)
        except Exception as e:
            warn(f"selector '{sel}' failed: {e}")

    try:
        js_boxes = page.evaluate(
            """
            () => {
              const ipRe = /\\b\\d{1,3}(\\.\\d{1,3}){3}\\b/;
              const macRe = /([0-9A-Fa-f]{2}[:\\-]){5}[0-9A-Fa-f]{2}/;
              const out = [];
              document.querySelectorAll('body *').forEach(el => {
                if (el.children.length > 0) return;
                let text = '';
                if (el.tagName === 'INPUT' || el.tagName === 'TEXTAREA') {
                  text = el.value || '';
                } else {
                  text = el.textContent || '';
                }
                if (!text) return;
                if (ipRe.test(text) || macRe.test(text)) {
                  const r = el.getBoundingClientRect();
                  if (r.width > 0 && r.height > 0) {
                    out.push({x: r.x, y: r.y, width: r.width, height: r.height});
                  }
                }
              });
              return out;
            }
            """
        )
        boxes.extend(js_boxes)
    except Exception as e:
        warn(f"IP/MAC regex DOM scan failed: {e}")
    return boxes


def redact(png_bytes, boxes, offset=(0, 0)):
    img = Image.open(io.BytesIO(png_bytes)).convert("RGB")
    draw = ImageDraw.Draw(img)
    ox, oy = offset
    for b in boxes:
        x0 = b["x"] - ox - 2
        y0 = b["y"] - oy - 2
        x1 = x0 + b["width"] + 4
        y1 = y0 + b["height"] + 4
        x0 = max(0, x0); y0 = max(0, y0)
        x1 = min(img.width, x1); y1 = min(img.height, y1)
        if x1 > x0 and y1 > y0:
            draw.rectangle([x0, y0, x1, y1], fill=REDACT_COLOR)
    return img


def save_full_page(page, out_path, label):
    global ok_count, skip_count
    try:
        page.evaluate("window.scrollTo(0, 0)")
        page.wait_for_timeout(100)
        boxes = find_sensitive_boxes(page)
        png = page.screenshot(full_page=True)
        img = redact(png, boxes, offset=(0, 0))
        img.save(out_path)
        results.append((out_path.name, label))
        ok_count += 1
        print(f"OK   {label:28s} -> {out_path}")
    except Exception as e:
        warn(f"failed to capture '{label}': {e}")
        skip_count += 1


def save_card(page, tab_id, heading, out_path, label):
    global ok_count, skip_count
    try:
        card = page.locator(f'.card:has(h2:text-is("{heading}"))').first
        card.wait_for(state="visible", timeout=5000)
        card.scroll_into_view_if_needed()
        page.wait_for_timeout(200)
        card_box = card.bounding_box()
        if not card_box:
            warn(f"card '{heading}' has no bounding box, skipping '{label}'")
            skip_count += 1
            return
        boxes = find_sensitive_boxes(page)
        png = card.screenshot()
        img = redact(png, boxes, offset=(card_box["x"], card_box["y"]))
        img.save(out_path)
        results.append((out_path.name, label))
        ok_count += 1
        print(f"OK   {label:28s} -> {out_path}")
    except PWTimeoutError:
        warn(f"card '{heading}' not found on tab '{tab_id}', skipping '{label}'")
        skip_count += 1
    except Exception as e:
        warn(f"failed to capture card '{heading}': {e}")
        skip_count += 1


def switch_tab(page, tab_id):
    try:
        btn = page.locator(f'[data-tab="{tab_id}"]').first
        btn.wait_for(state="visible", timeout=5000)
        btn.click()
        page.wait_for_timeout(NAV_WAIT_MS)
        return True
    except PWTimeoutError:
        return False
    except Exception as e:
        warn(f"switch_tab('{tab_id}') failed: {e}")
        return False


def main():
    global ok_count, skip_count
    out_dir = Path(OUT_DIR)
    out_dir.mkdir(parents=True, exist_ok=True)

    with sync_playwright() as p:
        browser = p.chromium.launch(headless=True)
        context = browser.new_context(viewport=DESKTOP_VIEWPORT, device_scale_factor=1)
        page = context.new_page()

        try:
            page.goto(BASE_URL, wait_until="load", timeout=15000)
        except Exception as e:
            print(f"ERROR: device unreachable at {BASE_URL}: {e}", file=sys.stderr)
            browser.close()
            sys.exit(1)

        try:
            page.wait_for_selector("#app", timeout=15000)
        except PWTimeoutError:
            print(f"ERROR: WebUI did not load (#app not found) at {BASE_URL}", file=sys.stderr)
            browser.close()
            sys.exit(1)

        # Force Cyberpunk / Glitch theme (slot 0, the default) and reload so
        # it's actually applied before the first screenshot.
        page.evaluate("localStorage.setItem('galvos_theme', '0')")
        if API_TOKEN:
            page.evaluate(f"window._apiToken = {API_TOKEN!r}")
        page.reload(wait_until="load")
        page.wait_for_selector("#app", timeout=15000)
        page.wait_for_timeout(NAV_WAIT_MS)

        # ---- Full layout (desktop) ----
        # Dashboard is the default tab; its telemetry/charts populate async
        # over the first ~30-60s (state polling ramps up), so give it time
        # before the first screenshot.
        print(f"Waiting {DASHBOARD_SETTLE_MS // 1000}s for Dashboard data to settle...")
        page.wait_for_timeout(DASHBOARD_SETTLE_MS)
        save_full_page(page, out_dir / "layout_desktop.png", "Full layout (desktop)")

        # ---- Full layout (mobile) ----
        page.set_viewport_size(MOBILE_VIEWPORT)
        page.reload(wait_until="load")
        page.wait_for_selector("#app", timeout=15000)
        page.wait_for_timeout(NAV_WAIT_MS)
        print(f"Waiting {DASHBOARD_SETTLE_MS // 1000}s for Dashboard data to settle (mobile)...")
        page.wait_for_timeout(DASHBOARD_SETTLE_MS)
        save_full_page(page, out_dir / "layout_mobile.png", "Full layout (mobile)")

        # Back to desktop viewport for every remaining shot.
        page.set_viewport_size(DESKTOP_VIEWPORT)
        page.reload(wait_until="load")
        page.wait_for_selector("#app", timeout=15000)
        page.wait_for_timeout(NAV_WAIT_MS)
        # Lands back on Dashboard (default tab) -- settle again before the
        # tab_dashboard.png / card shots below.
        print(f"Waiting {DASHBOARD_SETTLE_MS // 1000}s for Dashboard data to settle (post-reload)...")
        page.wait_for_timeout(DASHBOARD_SETTLE_MS)

        card_lookup = {}
        for tab_id, heading, filename in CARD_SHOTS:
            card_lookup.setdefault(tab_id, []).append((heading, filename))

        for label, tab_id, filename in TAB_SHOTS:
            if not switch_tab(page, tab_id):
                warn(f"tab selector [data-tab=\"{tab_id}\"] not found, skipping '{label}' and its cards")
                skip_count += 1
                skip_count += len(card_lookup.get(tab_id, []))
                continue

            if tab_id == "log":
                try:
                    page.wait_for_function(
                        "document.querySelectorAll('#log-lines > *').length > 0",
                        timeout=15000,
                    )
                except PWTimeoutError:
                    warn("no log lines appeared within 15s, capturing Log tab as-is")

            save_full_page(page, out_dir / filename, label)

            for heading, card_filename in card_lookup.get(tab_id, []):
                card_label = f"{label} - {heading}"
                save_card(page, tab_id, heading, out_dir / card_filename, card_label)

        browser.close()

    print("\nMarkdown snippets:\n")
    for filename, label in results:
        print(f"![{label}](screenshots/{filename})")

    print(f"\nSummary: OK: {ok_count}  SKIPPED: {skip_count}")

    print("\nConsolidated block for docs/04-ui-guide.md:\n")
    print("<!-- BEGIN generated screenshots (scripts/capture_screenshots.py) -->")
    for filename, label in results:
        print(f"![{label}](screenshots/{filename})")
    print("<!-- END generated screenshots -->")


if __name__ == "__main__":
    main()
