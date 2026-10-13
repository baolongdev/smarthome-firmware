#pragma once
// Design tokens from the HMI 480 design system (canvas "ESP32 480 HMI").

#include <lvgl.h>

LV_FONT_DECLARE(hmi_font_12_sb)
LV_FONT_DECLARE(hmi_font_13_r)
LV_FONT_DECLARE(hmi_font_14_m)
LV_FONT_DECLARE(hmi_font_16_sb)
LV_FONT_DECLARE(hmi_font_20_m)
LV_FONT_DECLARE(hmi_font_28_sb)
LV_FONT_DECLARE(hmi_font_64_sb)
LV_FONT_DECLARE(hmi_icons_20)
LV_FONT_DECLARE(hmi_icons_14)

// Lucide codepoints (UTF-8) available in hmi_icons_20; * also in hmi_icons_14.
#define ICON_WIFI ""
#define ICON_WIFI_OFF ""  // *
#define ICON_PLUG ""
#define ICON_DROPLET ""
#define ICON_ZAP ""
#define ICON_HOUSE ""
#define ICON_SLIDERS ""
#define ICON_SETTINGS ""
#define ICON_BULB ""
#define ICON_SNOWFLAKE ""
#define ICON_PLUG_ZAP ""
#define ICON_SUN ""
#define ICON_CLOCK ""
#define ICON_MEMORY ""
#define ICON_SERVER ""
#define ICON_GLOBE ""
#define ICON_INFO ""
#define ICON_ALERT ""     // *
#define ICON_ERROR ""     // *
#define ICON_THERMO ""
#define ICON_PALETTE ""
#define ICON_CHEVRON ""
#define ICON_LOCK ""
#define ICON_REFRESH ""
#define ICON_BACK ""
#define ICON_EYE ""
#define ICON_EYE_OFF ""
#define ICON_WIFI_LOW ""
#define ICON_WIFI_HIGH ""
#define ICON_DOWNLOAD ""
#define ICON_DRIVE ""
#define ICON_FILE ""
#define ICON_CHECK ""

namespace hmi::theme {

// Neutral base: black ground and greys carry the structure in every palette.
inline lv_color_t bg() { return lv_color_hex(0x000000); }
inline lv_color_t surface() { return lv_color_hex(0x121212); }
inline lv_color_t elevated() { return lv_color_hex(0x1F1F1F); }
inline lv_color_t pressed() { return lv_color_hex(0x292929); }
inline lv_color_t track() { return lv_color_hex(0x2E2E2E); }
inline lv_color_t disabled() { return lv_color_hex(0x555555); }
inline lv_color_t text() { return lv_color_hex(0xF5F5F5); }
inline lv_color_t muted() { return lv_color_hex(0x8A8A8A); }

// Signature palettes: colour only on what should stand out (gauge, "on"
// states, selection, status).  Chosen in Settings, stored in NVS.
struct Palette {
    const char* name;
    uint32_t accent;   // gauge, switches, slider, selected tab
    uint32_t accent2;  // power metric icon
    uint32_t info;     // humidity, air conditioner "on"
    uint32_t lamp;     // light "on"
    uint32_t success;  // ACTIVE, connected, socket "on"
    uint32_t warning;  // over threshold, link lost (never the accent hue)
    uint32_t error;
};
extern const Palette kPalettes[];
constexpr uint8_t kPaletteCount = 3;
void setPalette(uint8_t index);
const Palette& palette();

inline lv_color_t accent() { return lv_color_hex(palette().accent); }
inline lv_color_t accent2() { return lv_color_hex(palette().accent2); }
inline lv_color_t info() { return lv_color_hex(palette().info); }
inline lv_color_t lamp() { return lv_color_hex(palette().lamp); }
inline lv_color_t success() { return lv_color_hex(palette().success); }
inline lv_color_t warning() { return lv_color_hex(palette().warning); }
inline lv_color_t error() { return lv_color_hex(palette().error); }

// Solid tint of `c` over the background (12-14 %), so pills need no alpha
// blending at render time.
inline lv_color_t tint(lv_color_t c) { return lv_color_mix(c, bg(), 33); }

// Layout
constexpr int32_t kMargin = 16;
constexpr int32_t kContentY = 64;   // pages live in y 64..392
constexpr int32_t kContentH = 328;
constexpr int32_t kNavY = 408;

// Shared styles (initialised by init()).
extern lv_style_t card;      // surface, r16, pad 16, no border
extern lv_style_t pressedBg; // LV_STATE_PRESSED feedback, 150 ms
extern lv_style_t focusRing; // LV_STATE_FOCUS_KEY: 2 px accent outline
extern lv_style_t chip;      // 40x40 icon chip, r12

// (Re)initialise shared styles for the current palette.
void init();

// Plain container without theme styles (transparent, no padding, no scroll).
lv_obj_t* box(lv_obj_t* parent);
lv_obj_t* label(lv_obj_t* parent, const lv_font_t* font, lv_color_t color, const char* text = "");

}  // namespace hmi::theme
