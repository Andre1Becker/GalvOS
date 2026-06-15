# upload_all.py — Combined flash: firmware + LittleFS in one pio run
#
# Usage:
#   pio run --target upload_all      → build + flash both
#   pio run --target upload          → firmware only (unchanged)
#   pio run --target uploadfs        → LittleFS only  (unchanged)
#
# Sequence:
#   1. Build firmware (normal build)
#   2. Build LittleFS image (buildfs)
#   3. Flash firmware (upload)
#   4. Flash LittleFS (uploadfs)
#
# No esptool merge needed — PlatformIO flashes both images in sequence
# over the same serial connection.

Import("env")

def upload_all(source, target, env):
    print("\n=== upload_all: Firmware + LittleFS ===")

    # 1. Build LittleFS image
    print("[1/4] Building LittleFS image...")
    env.Execute("pio run --target buildfs --environment " + env["PIOENV"])

    # 2. Flash firmware
    print("[2/4] Flashing firmware...")
    env.Execute("pio run --target upload --environment " + env["PIOENV"])

    # 3. Flash LittleFS
    print("[3/4] Flashing LittleFS...")
    env.Execute("pio run --target uploadfs --environment " + env["PIOENV"])

    print("[4/4] Done! ESP32 will restart automatically.")

env.AddCustomTarget(
    name        = "upload_all",
    dependencies = None,
    actions     = upload_all,
    title       = "Flash Firmware + LittleFS",
    description = "Builds and flashes firmware and LittleFS filesystem in one step"
)
