#!/usr/bin/env python3
"""Build helper: gzip main/index.html and emit a C array suitable for
embedding into firmware. Invoked from main/CMakeLists.txt.

Why a script and not CMake natively?
- CMake's file(READ HEX) gives us raw bytes only, no compression.
- file(ARCHIVE_CREATE) exists from 3.18 but lays out a tar wrapper,
  not a single-member gzip stream the way Content-Encoding: gzip
  needs it.
- Python ships with PlatformIO's toolchain, so we lose no cross-
  platform support over the previous in-CMake approach.

Output is byte-identical to what `gzip -9 -n -c index.html` would
produce, sans timestamp (mtime forced to 0 so the bytes are
reproducible across builds and don't poke ccache).
"""
import gzip
import sys
import io
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: gen_index_html_gz.py <input.html> <output.c>",
              file=sys.stderr)
        return 2

    src = Path(sys.argv[1])
    dst = Path(sys.argv[2])

    raw = src.read_bytes()
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb",
                        compresslevel=9, mtime=0) as gz:
        gz.write(raw)
    gz_bytes = buf.getvalue()

    out = io.StringIO()
    out.write("/* GENERATED — see main/gen_index_html_gz.py. "
              "Do not edit by hand. */\n")
    out.write("#include <stddef.h>\n")
    out.write("const char index_html_gz_start[] = {")
    for i, b in enumerate(gz_bytes):
        if i % 16 == 0:
            out.write("\n    ")
        out.write("0x%02x," % b)
    out.write("\n};\n")
    out.write("const size_t index_html_gz_len = sizeof(index_html_gz_start);\n")

    dst.write_text(out.getvalue(), encoding="utf-8")
    print("gen_index_html_gz: raw=%d  gz=%d  ratio=%.1fx" %
          (len(raw), len(gz_bytes), len(raw) / max(1, len(gz_bytes))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
