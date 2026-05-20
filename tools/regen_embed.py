"""PIO pre-build hook: regenerate main/index_html_data.c when index.html
changed.

The CMakeLists.txt does `file(READ HEX) ...` at configure time and emits
`index_html_data.c`. CMake's CMAKE_CONFIGURE_DEPENDS is supposed to
re-run configure when listed files change, but PIO's SCons wrapper
caches the configure step and the trigger doesn't fire on plain HTML
edits — so the device ends up serving a stale SPA blob.

This pre-build script compares mtimes: if index.html is newer than the
generated .c, delete the .c and touch CMakeLists.txt so CMake
reconfigures cleanly on the next build.

SPDX-License-Identifier: MIT
"""
import os
Import("env")  # type: ignore  # noqa: F821

PROJECT_DIR = env.subst("$PROJECT_DIR")  # noqa: F821
HTML        = os.path.join(PROJECT_DIR, "main", "index.html")
GENERATED   = os.path.join(PROJECT_DIR, "main", "index_html_data.c")
CMAKELISTS  = os.path.join(PROJECT_DIR, "main", "CMakeLists.txt")

if os.path.exists(HTML) and os.path.exists(GENERATED):
    if os.path.getmtime(HTML) > os.path.getmtime(GENERATED):
        print("regen_embed: index.html is newer than the embedded C array — regenerating.")
        try:
            os.remove(GENERATED)
        except OSError:
            pass
        # Touch CMakeLists.txt so CMake notices and reconfigures.
        if os.path.exists(CMAKELISTS):
            now = None  # uses current time
            os.utime(CMAKELISTS, now)
