# gzip_assets.py — Pre-build hook: gzip large text assets before the LittleFS
# image is built.
#
# ESPAsyncWebServer's serveStatic automatically serves "<file>.gz" (with a
# Content-Encoding: gzip header) when the client requests "<file>" and the .gz
# variant exists on the filesystem. Shipping index.html pre-compressed cuts the
# on-flash size from ~440 KB to ~105 KB and — more importantly — removes the
# largest single internal-DRAM load spike: serving the uncompressed page held
# lwIP buffers long enough to starve the shared internal heap (WiFiUdp ENOMEM /
# low-heap spiral).
#
# The uncompressed original is kept in data/ (source of truth); only the .gz is
# added alongside it and picked up by buildfs. serveStatic prefers the .gz, so
# the plain file is never sent — it just stays as the editable source.
#
# Runs automatically on every `pio run` / `buildfs`. Regenerates the .gz only
# when the source is newer, so repeat builds stay fast.

Import("env")

import os
import gzip
import shutil

PROJECT_DIR = env["PROJECT_DIR"]
DATA_DIR = os.path.join(PROJECT_DIR, "data")

# Extensions worth compressing (already-compressed formats like png are skipped).
COMPRESS_EXT = (".html", ".css", ".js", ".svg", ".json", ".ico")


def gzip_assets(*args, **kwargs):
    if not os.path.isdir(DATA_DIR):
        return

    for name in os.listdir(DATA_DIR):
        if not name.lower().endswith(COMPRESS_EXT):
            continue
        if name.lower().endswith(".gz"):
            continue

        src = os.path.join(DATA_DIR, name)
        dst = src + ".gz"

        if os.path.isfile(dst) and os.path.getmtime(dst) >= os.path.getmtime(src):
            continue  # up to date

        with open(src, "rb") as f_in, gzip.open(dst, "wb", compresslevel=9) as f_out:
            shutil.copyfileobj(f_in, f_out)

        orig = os.path.getsize(src)
        comp = os.path.getsize(dst)
        print(
            "[gzip_assets] {} : {} B -> {} B ({:.0f}% smaller)".format(
                name, orig, comp, 100.0 * (1.0 - comp / orig)
            )
        )


# Run before the LittleFS image is assembled.
env.AddPreAction("buildfs", gzip_assets)
env.AddPreAction("uploadfs", gzip_assets)
