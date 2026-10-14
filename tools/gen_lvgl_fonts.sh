#!/usr/bin/env bash
# Regenerate the LVGL C fonts in src/hmi/fonts/ from assets/fonts/.
#   needs: node (npx), python + fonttools
#   run from firmware/:  bash tools/gen_lvgl_fonts.sh
set -euo pipefail
cd "$(dirname "$0")/.."

SRC=assets/fonts
OUT=src/hmi/fonts
TMP=$(mktemp -d)
mkdir -p "$OUT"
CONV="npx --yes lv_font_conv@1.5.3"

# Tabular digits so changing values never shift the layout.
for w in Regular Medium SemiBold; do
  python tools/tabular_digits.py "$SRC/BeVietnamPro-$w.ttf" "$TMP/$w.ttf"
done

# ASCII + Vietnamese + ° · – • … × ± ≤ ≥  (Be Vietnam Pro has no arrows: write "5–60", not "5 → 60")
TEXT="0x20-0x7E,0xB0,0xB7,0xC0-0xC3,0xC8-0xCA,0xCC-0xCD,0xD2-0xD5,0xD9-0xDA,0xDD,0xE0-0xE3,0xE8-0xEA,0xEC-0xED,0xF2-0xF5,0xF9-0xFA,0xFD,0x102-0x103,0x110-0x111,0x128-0x129,0x168-0x169,0x1A0-0x1A1,0x1AF-0x1B0,0x1EA0-0x1EF9,0x2013,0x2022,0x2026,0xD7,0xB1,0x2264,0x2265"
NUM="0x20,0x25,0x2D,0x2E,0x30-0x39,0x2013"   # space % - . 0-9 –

text_font() {  # name weight size range
  $CONV --bpp 4 --no-compress --format lvgl --size "$3" --font "$TMP/$2.ttf" -r "$4" \
        --lv-font-name "$1" -o "$OUT/$1.c"
}
text_font hmi_font_12_sb SemiBold 12 "$TEXT"
text_font hmi_font_13_r  Regular  13 "$TEXT"
text_font hmi_font_14_m  Medium   14 "$TEXT"
text_font hmi_font_16_sb SemiBold 16 "$TEXT"
text_font hmi_font_20_m  Medium   20 "0x20-0x7E,0xB0"
text_font hmi_font_28_sb SemiBold 28 "$NUM"
text_font hmi_font_64_sb SemiBold 64 "$NUM"

# Lucide stroke icons (ISC).  Codepoints: node_modules/lucide-static/font/info.json
ICONS20="0xE1AE,0xE1AF,0xE383,0xE0B8,0xE1B4,0xE0F8,0xE29A,0xE158,0xE1C2,0xE169,0xE461,0xE17C,0xE08B,0xE44A,0xE157,0xE0EB,0xE0FE,0xE193,0xE088,0xE186,0xE1DD,0xE073,0xE10F,0xE149,0xE04C,0xE0BE,0xE0BF,0xE5FD,0xE5FC,0xE0B6,0xE0F0,0xE0D0,0xE226,0xE0DC,0xE0C4,0xE31D,0xE0C7,0xE327,0xE18E,0xE340"
ICONS14="0xE193,0xE088,0xE1AF"
$CONV --bpp 4 --no-compress --format lvgl --size 20 --font "$SRC/lucide.ttf" -r "$ICONS20" --lv-font-name hmi_icons_20 -o "$OUT/hmi_icons_20.c"
$CONV --bpp 4 --no-compress --format lvgl --size 14 --font "$SRC/lucide.ttf" -r "$ICONS14" --lv-font-name hmi_icons_14 -o "$OUT/hmi_icons_14.c"

rm -rf "$TMP"
echo "fonts written to $OUT"
