# Force-rebuild the ESP-IDF esp_app_desc.c object on every build so the
# __DATE__/__TIME__ macros embedded into esp_app_desc_t (the same fields
# the /ota Device Management page and the index footer render) stay
# accurate. Without this PIO/ESP-IDF skip-rebuilds the file across
# successive incremental builds, freezing the "Built: <date> <time>"
# field at whatever moment the cold rebuild happened to run.
#
# Hooked in via platformio.ini -> extra_scripts = pre:scripts/...
#
# SPDX-License-Identifier: MIT
import glob
import os

Import("env")  # noqa: F821 — provided by PlatformIO at script load time

build_dir = env.subst("$BUILD_DIR")  # noqa: F821

# The object's exact path depends on ESP-IDF version / component layout —
# walk the build tree for anything matching `esp_app_desc.c.obj`.
patterns = [
    "**/esp_app_desc.c.obj",
    "**/esp_app_desc.c.o",
]

removed = []
for p in patterns:
    for match in glob.glob(os.path.join(build_dir, p), recursive=True):
        try:
            os.remove(match)
            removed.append(match)
        except OSError:
            pass

if removed:
    print(
        "[pre:force_rebuild_app_desc] removed {} stale app_desc obj(s) "
        "to refresh build timestamp".format(len(removed))
    )
