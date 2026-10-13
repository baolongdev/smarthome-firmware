#include "hmi_theme.h"

namespace hmi::theme {

lv_style_t card;
lv_style_t pressedBg;
lv_style_t focusRing;
lv_style_t chip;

namespace {
lv_style_transition_dsc_t s_pressTrans;
const lv_style_prop_t kPressProps[] = {LV_STYLE_BG_COLOR, LV_STYLE_PROP_INV};
}  // namespace

const Palette kPalettes[kPaletteCount] = {
    // name     accent    accent2   info      lamp      success   warning   error
    {"Lime",   0xC5F82A, 0x9B87FF, 0x4DA3FF, 0xFFC94D, 0x3DDC97, 0xFF9F1C, 0xFF4D5E},
    {"Ember",  0xFF6B2C, 0xB794FF, 0x4DA3FF, 0xFFD166, 0x3DDC97, 0xFFD23F, 0xFF3B4E},
    {"Cobalt", 0x4C7DFF, 0xC084FC, 0x38D9F5, 0xFFC94D, 0x3DDC97, 0xFFB020, 0xFF4D5E},
};

namespace {
uint8_t s_palette = 0;
bool s_stylesReady = false;
}  // namespace

void setPalette(uint8_t index) { s_palette = index < kPaletteCount ? index : 0; }
const Palette& palette() { return kPalettes[s_palette]; }

void init() {
    if (s_stylesReady) {  // palette switch: drop the old property lists first
        lv_style_reset(&card);
        lv_style_reset(&pressedBg);
        lv_style_reset(&focusRing);
        lv_style_reset(&chip);
    }
    s_stylesReady = true;
    lv_style_init(&card);
    lv_style_set_bg_color(&card, surface());
    lv_style_set_bg_opa(&card, LV_OPA_COVER);
    lv_style_set_radius(&card, 16);
    lv_style_set_pad_all(&card, 16);
    lv_style_set_border_width(&card, 0);
    lv_style_set_shadow_width(&card, 0);

    lv_style_transition_dsc_init(&s_pressTrans, kPressProps, lv_anim_path_ease_out, 150, 0, nullptr);
    lv_style_init(&pressedBg);
    lv_style_set_bg_color(&pressedBg, pressed());
    lv_style_set_transition(&pressedBg, &s_pressTrans);

    lv_style_init(&focusRing);
    lv_style_set_outline_width(&focusRing, 2);
    lv_style_set_outline_color(&focusRing, accent());
    lv_style_set_outline_pad(&focusRing, 2);

    lv_style_init(&chip);
    lv_style_set_bg_color(&chip, elevated());
    lv_style_set_bg_opa(&chip, LV_OPA_COVER);
    lv_style_set_radius(&chip, 12);
    lv_style_set_border_width(&chip, 0);
    lv_style_set_pad_all(&chip, 0);
}

lv_obj_t* box(lv_obj_t* parent) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t* label(lv_obj_t* parent, const lv_font_t* font, lv_color_t color, const char* text) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text);
    return l;
}

}  // namespace hmi::theme
