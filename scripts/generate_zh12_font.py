#!/usr/bin/env python3
"""Generate full GB2312 12x12 Chinese font bitmap for MimiClaw e-paper display.

Renders all 6763 GB2312 simplified Chinese characters directly to 12x12
monochrome bitmaps using the macOS system Heiti font (10pt, hinted for
small sizes). Produces a drop-in replacement for display_font_zh12.c.

Output format per glyph: 24 bytes (12 rows × 2 bytes/row), MSB-first.
"""

import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

WIDTH = 12
HEIGHT = 12
BYTES_PER_GLYPH = 2 * HEIGHT  # 12 rows × 2 bytes/row

FONT_PATH = "/System/Library/Fonts/STHeiti Medium.ttc"
FONT_SIZE = 10  # pt — direct 12×12 rendering with hinting
THRESHOLD = 128

# ── GB2312 code space ───────────────────────────────────────────────
GB2312_HANZI_START = 0xB0
GB2312_HANZI_END = 0xF7
GB2312_ROW_START = 0xA1
GB2312_ROW_END = 0xFE
CJK_BLOCK_START = 0x4E00
CJK_BLOCK_END = 0x9FFF


def decode_gb2312_codepoints():
    """Yield Unicode codepoint for every CJK hanzi in GB2312."""
    for hi in range(GB2312_HANZI_START, GB2312_HANZI_END + 1):
        for lo in range(GB2312_ROW_START, GB2312_ROW_END + 1):
            raw = bytes([hi, lo])
            try:
                char = raw.decode("gb2312")
            except (UnicodeDecodeError, LookupError):
                continue
            if len(char) != 1:
                continue
            cp = ord(char)
            if CJK_BLOCK_START <= cp <= CJK_BLOCK_END:
                yield cp


def render_glyph(char, font):
    """Render single char directly to 12×12 monochrome bitmap (24 bytes, MSB-first)."""
    img = Image.new("L", (WIDTH, HEIGHT), 255)
    draw = ImageDraw.Draw(img)

    bbox = draw.textbbox((0, 0), char, font=font)
    tw = bbox[2] - bbox[0]
    th = bbox[3] - bbox[1]
    x = (WIDTH - tw) // 2 - bbox[0]
    y = (HEIGHT - th) // 2 - bbox[1]
    draw.text((x, y), char, fill=0, font=font)

    bitmap = bytearray(BYTES_PER_GLYPH)
    for row in range(HEIGHT):
        row_val = 0
        for col in range(WIDTH):
            if img.getpixel((col, row)) < THRESHOLD:
                row_val |= 0x8000 >> col
        bitmap[row * 2] = (row_val >> 8) & 0xFF
        bitmap[row * 2 + 1] = row_val & 0xFF

    return bytes(bitmap)


def ascii_art(bitmap):
    """Print a glyph bitmap as ASCII art for visual inspection."""
    lines = []
    for row in range(HEIGHT):
        row_val = (bitmap[row * 2] << 8) | bitmap[row * 2 + 1]
        line = "".join(
            "##" if (row_val & (0x8000 >> col)) else "  "
            for col in range(WIDTH)
        )
        lines.append(line)
    return "\n".join(lines)


def generate_c_file(glyphs, output_path):
    """Write display_font_zh12.c with the sorted glyph array."""
    header = """#include "display/display_font_zh12.h"

static const display_zh12_glyph_t ZH12_GLYPHS[] = {
"""

    with open(output_path, "w", encoding="utf-8") as f:
        f.write(header)

        for i, (cp, bitmap) in enumerate(glyphs):
            hex_bytes = ", ".join(f"0x{b:02X}" for b in bitmap)
            sep = ",\n" if i < len(glyphs) - 1 else "\n"
            f.write(f"    {{0x{cp:04X}, {{{hex_bytes}}}}}{sep}")

        f.write("""};

const display_zh12_glyph_t *display_font_zh12_find(uint32_t codepoint)
{
    size_t low = 0;
    size_t high = sizeof(ZH12_GLYPHS) / sizeof(ZH12_GLYPHS[0]);

    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (ZH12_GLYPHS[mid].codepoint == codepoint) {
            return &ZH12_GLYPHS[mid];
        }
        if (ZH12_GLYPHS[mid].codepoint < codepoint) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }

    return NULL;
}
""")


def main():
    print("Loading font...")
    try:
        font = ImageFont.truetype(FONT_PATH, FONT_SIZE)
    except OSError:
        print(f"ERROR: Could not load font at {FONT_PATH}", file=sys.stderr)
        print("Trying fallback: PingFang.ttc...", file=sys.stderr)
        try:
            font = ImageFont.truetype(
                "/System/Library/Fonts/PingFang.ttc", FONT_SIZE
            )
        except OSError:
            print("FATAL: No Chinese font found.", file=sys.stderr)
            sys.exit(1)

    print("Collecting GB2312 codepoints...")
    codepoints = sorted(decode_gb2312_codepoints())
    print(f"  Found {len(codepoints)} CJK characters in GB2312")

    print("Rendering glyphs...")
    glyphs = []
    zero_glyphs = []
    for idx, cp in enumerate(codepoints):
        char = chr(cp)
        bitmap = render_glyph(char, font)
        glyphs.append((cp, bitmap))

        if all(b == 0x00 for b in bitmap):
            zero_glyphs.append((cp, char))

        if (idx + 1) % 500 == 0:
            print(f"  {idx + 1}/{len(codepoints)}...")

    print(f"  Done. {len(glyphs)} glyphs rendered.")

    if zero_glyphs:
        print(f"  WARNING: {len(zero_glyphs)} all-zero glyphs:")
        for cp, ch in zero_glyphs[:10]:
            print(f"    U+{cp:04X} {ch}")
        if len(zero_glyphs) > 10:
            print(f"    ... and {len(zero_glyphs) - 10} more")
    else:
        print("  All glyphs non-zero.")

    # Print sample glyphs
    print("\nSample glyphs:")
    samples = [0x4E00, 0x4E2D, 0x56FD, 0x5929, 0x591A, 0x4E91, 0x5317, 0x4EAC,
               0x96E8, 0x660E, 0x9F99]
    for cp in samples:
        for g_cp, g_bm in glyphs:
            if g_cp == cp:
                print(f"\n  U+{g_cp:04X} {chr(g_cp)}:")
                print(ascii_art(g_bm))
                break
        else:
            print(f"\n  U+{cp:04X} {chr(cp)}: NOT IN FONT")

    output_path = (
        Path(__file__).parent.parent / "main" / "display" / "display_font_zh12.c"
    )
    print(f"\nWriting {output_path}...")
    generate_c_file(glyphs, str(output_path))

    size = output_path.stat().st_size
    print(f"  Written: {size:,} bytes ({size/1024:.0f} KB)")


if __name__ == "__main__":
    main()
