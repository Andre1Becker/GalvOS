# upload_all.py — Kombiniertes Flash: Firmware + LittleFS in einem pio run
#
# Verwendung:
#   pio run --target upload_all      → baut + flasht beides
#   pio run --target upload          → nur Firmware (unverändert)
#   pio run --target uploadfs        → nur LittleFS  (unverändert)
#
# Ablauf:
#   1. Firmware bauen (normaler build)
#   2. LittleFS-Image bauen (buildfs)
#   3. Firmware flashen (upload)
#   4. LittleFS flashen (uploadfs)
#
# Kein esptool-merge nötig — PlatformIO flasht beide Images nacheinander
# über dieselbe serielle Verbindung.

Import("env")

def upload_all(source, target, env):
    print("\n=== upload_all: Firmware + LittleFS ===")

    # 1. LittleFS-Image bauen
    print("[1/4] Baue LittleFS-Image...")
    env.Execute("pio run --target buildfs --environment " + env["PIOENV"])

    # 2. Firmware flashen
    print("[2/4] Flashe Firmware...")
    env.Execute("pio run --target upload --environment " + env["PIOENV"])

    # 3. LittleFS flashen
    print("[3/4] Flashe LittleFS...")
    env.Execute("pio run --target uploadfs --environment " + env["PIOENV"])

    print("[4/4] Fertig! ESP32 wird automatisch neu gestartet.")

env.AddCustomTarget(
    name        = "upload_all",
    dependencies = None,
    actions     = upload_all,
    title       = "Flash Firmware + LittleFS",
    description = "Baut und flasht Firmware und LittleFS-Filesystem in einem Schritt"
)
