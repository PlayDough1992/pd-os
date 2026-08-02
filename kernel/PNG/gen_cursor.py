#!/usr/bin/env python3
"""Convert PD-CURSOR.png to a C header with sprite and mask arrays."""
import sys
import os

def main():
    src = os.path.join(os.path.dirname(__file__), "PD-CURSOR.png")
    dst = os.path.join(os.path.dirname(__file__), "..", "include", "cursor_data.h")

    try:
        from PIL import Image
    except ImportError:
        print("Pillow not installed; skipping cursor_data.h generation")
        return

    img = Image.open(src).convert("RGBA")
    w, h = img.size
    pixels = list(img.getdata())

    sprites = []
    masks   = []
    for r, g, b, a in pixels:
        if a < 32:
            sprites.append("0x00000000")
            masks.append("0")
        else:
            sprites.append("0x00%02X%02X%02X" % (r, g, b))
            masks.append("1")

    with open(dst, "w") as f:
        f.write("#ifndef CURSOR_DATA_H\n#define CURSOR_DATA_H\n")
        f.write("#define PD_CURSOR_W %d\n" % w)
        f.write("#define PD_CURSOR_H %d\n" % h)
        f.write("static const uint32_t pd_cursor_sprite[%d] = {\n" % (w * h))
        for i, s in enumerate(sprites):
            comma = "," if i < len(sprites) - 1 else ""
            f.write("  %s%s" % (s, comma))
            if (i + 1) % w == 0:
                f.write("\n")
        f.write("};\n")
        f.write("static const uint8_t pd_cursor_mask[%d] = {\n" % (w * h))
        for i, m in enumerate(masks):
            comma = "," if i < len(masks) - 1 else ""
            f.write("  %s%s" % (m, comma))
            if (i + 1) % w == 0:
                f.write("\n")
        f.write("};\n")
        f.write("#endif /* CURSOR_DATA_H */\n")

    print("Generated cursor_data.h: %dx%d (%d opaque pixels)" % (
        w, h, masks.count("1")))

if __name__ == "__main__":
    main()
