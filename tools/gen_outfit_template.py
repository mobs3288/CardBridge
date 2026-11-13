#!/usr/bin/env python3
"""Regenerates src/export/outfit_template.inc from image/outfit_template.jpg.

The outfit-file icon background (white rounded surround, pink tiled wallpaper,
filigree frame with hearts and gems, stage floor) is an embedded template, so the
CLI and the viewer both composite onto it without any file on disk. The source of
truth is image/outfit_template.jpg (504x704 JPEG, aspect matching the stock
Koikatsu outfit thumbnails); this script embeds those exact bytes as a C array.

    python tools/gen_outfit_template.py
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "image", "outfit_template.jpg")
DST = os.path.join(ROOT, "src", "export", "outfit_template.inc")

def main() -> int:
    with open(SRC, "rb") as f:
        data = f.read()
    lines = []
    lines.append("// Generated from image/outfit_template.jpg by tools/gen_outfit_template.py - do not edit by hand.")
    lines.append("// The outfit-file icon background (a Stage-style pink tiled backdrop with the filigree frame).")
    lines.append("// Embedded as-is (JPEG), decoded at icon time via stb_image. 504x704, the stock outfit-thumbnail aspect.")
    lines.append("namespace {")
    lines.append("const unsigned char kOutfitTemplateJpeg[] = {")
    for i in range(0, len(data), 16):
        chunk = ", ".join(f"0x{b:02x}" for b in data[i : i + 16])
        lines.append(f"    {chunk},")
    lines.append("};")
    lines.append(f"const size_t kOutfitTemplateJpegSize = {len(data)};")
    lines.append("} // namespace")
    text = "\n".join(lines) + "\n"
    with open(DST, "w", newline="\n") as f:
        f.write(text)
    print(f"wrote {DST} ({len(data)} bytes of template data)")
    return 0

if __name__ == "__main__":
    sys.exit(main())
