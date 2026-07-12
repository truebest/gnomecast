#!/usr/bin/env python3
import pathlib
import re
import sys


def supported(codepoint: int) -> bool:
    return (
        0xA0 <= codepoint <= 0xFF
        or 0x400 <= codepoint <= 0x45F
        or 0x462 <= codepoint <= 0x463
        or 0x46A <= codepoint <= 0x46B
        or 0x472 <= codepoint <= 0x475
        or 0x490 <= codepoint <= 0x4C2
        or 0x4CF <= codepoint <= 0x4D9
        or 0x4DC <= codepoint <= 0x4E9
        or 0x4EE <= codepoint <= 0x4F9
    )


expected = {codepoint for codepoint in range(0xA0, 0x500) if supported(codepoint)}
for argument in sys.argv[1:]:
    path = pathlib.Path(argument)
    source = path.read_text(encoding="utf-8")
    if ".bitmap_format = 0" not in source:
        raise SystemExit(f"{path}: profile-name font must remain uncompressed")
    codepoints = {
        int(match, 16)
        for match in re.findall(r"/\* U\+([0-9A-F]{4,6}) ", source)
    }
    missing = sorted(expected - codepoints)
    if missing:
        formatted = ", ".join(f"U+{codepoint:04X}" for codepoint in missing)
        raise SystemExit(f"{path}: missing profile-name glyphs: {formatted}")
