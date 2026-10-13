#!/usr/bin/env python3
"""Make 0-9 equal-width (tabular) in a TrueType font.

Be Vietnam Pro has no `tnum` feature, so changing values (26.4 -> 27.1) would
shift the layout.  This widens every digit to the widest one and centres the
outline in the new advance.

    pip install fonttools
    python tools/tabular_digits.py in.ttf out.ttf
"""
import sys

from fontTools.ttLib import TTFont


def main(src, dst):
    font = TTFont(src)
    cmap = font.getBestCmap()
    hmtx = font["hmtx"]
    glyf = font["glyf"]
    names = [cmap[ord(c)] for c in "0123456789"]
    width = max(hmtx[n][0] for n in names)
    for n in names:
        adv, lsb = hmtx[n]
        dx = (width - adv) // 2
        g = glyf[n]
        if g.isComposite():
            for comp in g.components:
                comp.x += dx
        elif g.numberOfContours > 0:
            g.coordinates.translate((dx, 0))
        g.recalcBounds(glyf)
        hmtx[n] = (width, lsb + dx)
    font.save(dst)
    print(f"{dst}: digits -> {width} units")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
