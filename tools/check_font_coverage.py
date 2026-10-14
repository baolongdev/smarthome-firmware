#!/usr/bin/env python3
"""Report characters used in UI strings that the compiled LVGL fonts lack.

A missing glyph renders as an empty box on the panel.  Run after adding text:

    python tools/check_font_coverage.py

Checks every string literal in the UI/business sources against the TEXT range
in tools/gen_lvgl_fonts.sh (Lucide icon codepoints U+E000..U+F8FF are skipped).
Exit code 1 if anything is missing.
"""
import glob
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCES = ["src/hmi/*.cpp", "src/*.cpp", "include/config.h"]
STRING = re.compile(r'"((?:[^"\\]|\\.)*)"')


def parse_ranges(spec):
    out = set()
    for part in spec.split(","):
        if "-" in part:
            a, b = part.split("-")
            out.update(range(int(a, 16), int(b, 16) + 1))
        else:
            out.add(int(part, 16))
    return out


def compiled_glyphs(c_file):
    """Code points actually present in a generated LVGL font (/* U+XXXX */ markers)."""
    src = (ROOT / c_file).read_text(encoding="utf-8")
    return {int(h, 16) for h in re.findall(r"/\* U\+([0-9A-Fa-f]{4,5}) ", src)}


def main():
    script = (ROOT / "tools/gen_lvgl_fonts.sh").read_text(encoding="utf-8")
    wanted = parse_ranges(re.search(r'^TEXT="([^"]+)"', script, re.M).group(1))
    # What the panel can really draw: glyphs present in every text font that
    # was generated (a range entry the TTF lacks never makes it into the .c).
    text = None
    for name in ("hmi_font_12_sb", "hmi_font_13_r", "hmi_font_14_m", "hmi_font_16_sb"):
        glyphs = compiled_glyphs(f"src/hmi/fonts/{name}.c")
        text = glyphs if text is None else text & glyphs
    lost = sorted(wanted - text)
    if lost:
        print("In TEXT range but not in the compiled fonts (TTF lacks them, or fonts are stale):")
        print("  " + " ".join(f"U+{cp:04X}" for cp in lost))
    missing = {}
    for pattern in SOURCES:
        for path in sorted(glob.glob(str(ROOT / pattern))):
            rel = Path(path).relative_to(ROOT).as_posix()
            for n, line in enumerate(Path(path).read_text(encoding="utf-8").splitlines(), 1):
                code = line.split("//", 1)[0] if '"' not in line.split("//", 1)[0] else line
                for lit in STRING.findall(code):
                    for ch in lit:
                        cp = ord(ch)
                        if 0xE000 <= cp <= 0xF8FF or cp in text:
                            continue
                        missing.setdefault(ch, []).append(f"{rel}:{n}")
    if not missing:
        print("OK: every UI string is covered by the text fonts")
        return 0
    for ch, where in sorted(missing.items()):
        print(f"U+{ord(ch):04X} {ch!r:>5}  x{len(where):<3} {', '.join(where[:5])}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
