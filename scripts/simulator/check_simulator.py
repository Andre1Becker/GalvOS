#!/usr/bin/env python3
"""Smoke-test docs/index.html (the generated GalvOS simulator) in headless Chrome.

Catches what a diff review cannot: a name collision between the simulator layer
and the WebUI bundle (both share one global scope), a mock response whose field
names drifted from the firmware's JSON, or a tab whose loader throws on the
simulated data. Any page error at all fails the run.

Usage (from the repo root):

    .venv/Scripts/python.exe scripts/simulator/check_simulator.py
"""
import asyncio
import sys
from pathlib import Path

from playwright.async_api import async_playwright

ROOT = Path(__file__).resolve().parent.parent.parent
PAGE = ROOT / "docs/index.html"

TABS = ["dashboard", "presets", "preset-manager", "dmx", "text", "paint", "ilda",
        "calib", "optimizer", "projection", "thermal", "log", "config"]

# Live-control paths worth exercising: each one is a stage of the sim's render
# pipeline that only runs when the WebUI asks for it.
INTERACTIONS = [
    ("Star 8 + per-side colors",
     "activatePreset(9); document.getElementById('segColEnabled').checked = true;"
     "segColToggle(true); segColRandomize()"),
    ("H Line + bounce",
     "activatePreset(13); document.getElementById('lv-hline-bounce').checked = true;"
     "sendLiveControls()"),
    ("Heart + kaleidoscope (mirror fold)",
     "activatePreset(24); document.getElementById('lv-kaleido-on').checked = true;"
     "document.getElementById('lv-kaleido-mode').value = '1'; sendLiveControls()"),
    ("Heart + kaleidoscope (radial repeat)",
     "document.getElementById('lv-kaleido-mode').value = '0'; onKaleidoModeChange()"),
    ("Heart + points-only mode",
     "document.getElementById('lv-kaleido-on').checked = false;"
     "document.getElementById('lv-points-only').checked = true; sendLiveControls()"),
    ("Concentric Rings (MultiObject profile)",
     "document.getElementById('lv-points-only').checked = false; sendLiveControls();"
     "activatePreset(54)"),
    ("Sine Wave + autoscale + radial4 mirror",
     "activatePreset(33); document.getElementById('lv-autoscale-speed').value = '60';"
     "liveSlider('lv-autoscale-speed'); setMirrorMode(3)"),
    ("Milky Way (Particles profile)", "setMirrorMode(0); activatePreset(80)"),
]


async def main() -> int:
    failures = []
    async with async_playwright() as pw:
        browser = await pw.chromium.launch()
        page = await browser.new_page(viewport={"width": 1600, "height": 1000})
        errors = []
        page.on("pageerror", lambda e: errors.append(f"pageerror: {e}"))
        page.on("console", lambda m: errors.append(f"console {m.type}: {m.text}")
                if m.type == "error" else None)

        await page.goto(PAGE.as_uri())
        await page.wait_for_timeout(2500)

        if not await page.is_visible("#disclaimer-overlay"):
            failures.append("launch overlay did not appear")
        await page.evaluate("closeDisclaimer()")
        await page.wait_for_timeout(400)

        sim = await page.evaluate("window.GALVOS_SIM || null")
        if not sim:
            failures.append("window.GALVOS_SIM missing -- simulator layer did not run")
            print("FAIL: simulator layer did not initialise")
            for e in errors:
                print("  ", e)
            await browser.close()
            return 1
        print(f"simulator {sim['SIM_VERSION']} / FW {sim['FW_VERSION']} / UI {sim['UI_VERSION']}, "
              f"{len(sim['presets'])} presets, {len(sim['profiles'])} optimizer profiles")

        for tab in TABS:
            await page.evaluate(f"switchTab('{tab}')")
            await page.wait_for_timeout(700)
            if not await page.is_visible(f"#tab-{tab}"):
                failures.append(f"tab {tab} did not become visible")
        print(f"tabs: {len(TABS)} opened")

        await page.evaluate("switchTab('presets')")
        await page.wait_for_timeout(800)
        for label, js in INTERACTIONS:
            await page.evaluate(js)
            await page.wait_for_timeout(1100)
            stats = await page.evaluate("""() => ({
                pts: GALVOS_SIM.stats.emitted_lit + GALVOS_SIM.stats.emitted_blank,
                lit: GALVOS_SIM.stats.emitted_lit,
                jumps: GALVOS_SIM.stats.jump_count,
                profile: GALVOS_SIM.profiles[GALVOS_SIM.presetProfile[GALVOS_SIM.ui.preset]].name
            })""")
            print(f"  {label:42s} {stats['pts']:5d} pts  {stats['lit']:5d} lit  "
                  f"{stats['jumps']:3d} jumps  [{stats['profile']}]")
            if stats["lit"] == 0:
                failures.append(f"{label}: frame contains no lit points")

        # Calibration-tab grids come from the /api/warp + /api/brightness mocks.
        await page.evaluate("switchTab('calib')")
        await page.wait_for_timeout(1200)
        cells = await page.evaluate("document.querySelectorAll('.brightness-cell').length")
        warp_ink = await page.evaluate("""() => {
            const c = document.getElementById('warp-canvas');
            const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
            let n = 0;
            for (let i = 0; i < d.length; i += 4) if (d[i] || d[i+1] || d[i+2]) n++;
            return n;
        }""")
        print(f"  calibration: warp grid {warp_ink} lit px, brightness grid {cells} cells")
        if not cells:
            failures.append("brightness grid rendered no cells")
        if not warp_ink:
            failures.append("warp grid drew nothing")

        # Mobile: the SIMULATOR banner must not push the top bar or nav around.
        # Checked on the Dashboard, not the Presets tab: the Presets tab's
        # right-hand sidebar column overflows 390px in data/index.html itself
        # (its 2.2fr/1fr grid is not in the mobile collapse rule), and the sim
        # inherits that verbatim -- failing on it here would report a WebUI
        # layout quirk as a simulator regression.
        await page.set_viewport_size({"width": 390, "height": 844})
        await page.evaluate("switchTab('dashboard')")
        await page.wait_for_timeout(700)
        if await page.evaluate("document.documentElement.scrollWidth > window.innerWidth + 1"):
            failures.append("horizontal overflow at 390x844 (dashboard)")
        nav_ok = await page.evaluate("""() => Array.from(document.querySelectorAll('#bottom-tabs button'))
            .every(b => { const r = b.getBoundingClientRect();
                          return r.width > 40 && r.left >= -1 && r.right <= window.innerWidth + 1; })""")
        if not nav_ok:
            failures.append("bottom nav buttons off-screen or too narrow at 390x844")
        print("  mobile 390x844: nav ok" if nav_ok else "  mobile 390x844: nav BROKEN")

        await browser.close()

    for e in errors:
        failures.append(e)
    if failures:
        print(f"\nFAIL ({len(failures)}):")
        for f in failures:
            print("  ", f)
        return 1
    print("\nOK -- no page errors, every tab and render stage exercised")
    return 0


sys.exit(asyncio.run(main()))
