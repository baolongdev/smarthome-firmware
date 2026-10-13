// LVGL widgets for the 480x480 HMI.  Layout values follow the "Pixel specs"
// board of the design canvas; page content sits in y 64..392 between the
// persistent header and bottom navigation.

#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <math.h>
#include <stdio.h>

#include "hmi.h"
#include "hmi_model.h"
#include "hmi_port.h"
#include "hmi_theme.h"

namespace hmi {
namespace {

using namespace theme;

enum Page : uint8_t { P_HOME, P_CONTROL, P_SETTINGS, P_WIFI, P_COUNT };
constexpr uint8_t kNavCount = 3;  // P_WIFI is a sub-page of Settings

struct Ui {
    Callbacks cb;
    // header
    lv_obj_t* name;
    lv_obj_t* caption;
    lv_obj_t* clock;
    lv_obj_t* wifi;
    // navigation
    lv_obj_t* page[P_COUNT];
    lv_obj_t* navBtn[kNavCount];
    lv_obj_t* navIcon[kNavCount];
    lv_obj_t* navText[kNavCount];
    uint8_t current = P_HOME;
    // home
    lv_obj_t* arc;
    lv_obj_t* spinner;
    lv_obj_t* value;
    lv_obj_t* sub;
    lv_obj_t* pill;
    lv_obj_t* pillDot;
    lv_obj_t* pillIcon;
    lv_obj_t* pillText;
    lv_obj_t* metric[2];
    int32_t shown = 0;  // primary value ×10 currently drawn
    // control
    lv_obj_t* relayIcon[RELAY_COUNT];
    lv_obj_t* relayStatus[RELAY_COUNT];
    lv_obj_t* relaySwitch[RELAY_COUNT];
    lv_obj_t* devStatus[kMaxDevices];
    lv_obj_t* devRow[kMaxDevices][kMaxChannels];
    lv_obj_t* devIcon[kMaxDevices][kMaxChannels];
    lv_obj_t* devSub[kMaxDevices][kMaxChannels];
    lv_obj_t* devSwitch[kMaxDevices][kMaxChannels];
    lv_obj_t* slider;
    lv_obj_t* sliderValue;
    // settings
    lv_obj_t* dimSwitch;
    lv_obj_t* sysWifi;
    lv_obj_t* sysMqtt;
    lv_obj_t* sysIp;
    lv_obj_t* sysMem;
    lv_obj_t* sysUptime;
    lv_obj_t* sdCard;
    lv_obj_t* sdConfig;
    lv_obj_t* sdFw;
    bool fwArmed = false;
    // Wi-Fi page
    lv_obj_t* wifiList;
    lv_obj_t* wifiRefresh;
    lv_obj_t* wifiSpinner;
    char wifiRenderedSsid[33] = "";
    // join sheet (full screen, above everything)
    lv_obj_t* sheet;
    lv_obj_t* sheetSsid;
    lv_obj_t* pass;
    lv_obj_t* eye;
    lv_obj_t* kb;
    lv_obj_t* joinSpinner;
    lv_obj_t* joinStatus;
    lv_obj_t* joinBtn;
    lv_obj_t* joinBtnText;
    char joinSsid[33] = "";
    bool joinSecure = true;
    // firmware update overlay
    lv_obj_t* update;
    lv_obj_t* updateBar;
    lv_obj_t* updateText;
    bool dimmed = false;
};
Ui ui;
// Copy of the model taken under ModelLock at the start of every UI tick.  All
// widget code and event callbacks read this, never the shared model.
Model s_snap;

void applySystem(const Model& m);
void applyWifiList(const Model& m);

const char* const kRelayNames[RELAY_COUNT] = {RELAY_NAME_1, RELAY_NAME_2, RELAY_NAME_3};
const char* const kRelayIcons[RELAY_COUNT] = {ICON_BULB, ICON_SNOWFLAKE, ICON_PLUG_ZAP};

// ---------- small builders ----------

void makeClickable(lv_obj_t* o) {
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_style(o, &pressedBg, LV_STATE_PRESSED);
    lv_obj_add_style(o, &focusRing, LV_STATE_FOCUS_KEY);
    lv_group_add_obj(lv_group_get_default(), o);
}

lv_obj_t* cardObj(lv_obj_t* parent) {
    lv_obj_t* o = box(parent);
    lv_obj_add_style(o, &card, 0);
    return o;
}

lv_obj_t* iconChip(lv_obj_t* parent, const char* icon, lv_color_t color, lv_obj_t** iconOut = nullptr) {
    lv_obj_t* c = box(parent);
    lv_obj_add_style(c, &chip, 0);
    lv_obj_set_size(c, 40, 40);
    lv_obj_t* i = label(c, &hmi_icons_20, color, icon);
    lv_obj_center(i);
    if (iconOut) *iconOut = i;
    return c;
}

lv_obj_t* flexRow(lv_obj_t* parent, int32_t gap, lv_flex_align_t cross = LV_FLEX_ALIGN_CENTER) {
    lv_obj_t* o = box(parent);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, cross, cross);
    lv_obj_set_style_pad_column(o, gap, 0);
    lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    return o;
}

lv_obj_t* flexCol(lv_obj_t* parent, int32_t gap) {
    lv_obj_t* o = box(parent);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(o, gap, 0);
    lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    return o;
}

lv_obj_t* makeSwitch(lv_obj_t* parent) {
    lv_obj_t* sw = lv_switch_create(parent);
    lv_obj_remove_style_all(sw);
    lv_obj_set_size(sw, 52, 32);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(sw, track(), 0);
    lv_obj_set_style_radius(sw, 16, 0);
    lv_obj_set_style_bg_opa(sw, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, accent(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_radius(sw, 16, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, muted(), LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, bg(), LV_PART_KNOB | LV_STATE_CHECKED);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sw, -4, LV_PART_KNOB);
    lv_obj_set_style_anim_duration(sw, 200, 0);
    // The row owns the tap; the switch only mirrors state.
    lv_obj_remove_flag(sw, LV_OBJ_FLAG_CLICKABLE);
    lv_group_remove_obj(sw);
    return sw;
}

lv_obj_t* iconButton(lv_obj_t* parent, const char* icon, const char* a11y) {
    lv_obj_t* b = box(parent);
    lv_obj_set_size(b, 48, 48);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, bg(), 0);
    makeClickable(b);
    lv_obj_t* i = label(b, &hmi_icons_20, text(), icon);
    lv_obj_center(i);
    (void)a11y;  // LVGL has no accessibility tree; kept for readability at call sites
    return b;
}

void setChecked(lv_obj_t* o, bool on) {
    if (on) lv_obj_add_state(o, LV_STATE_CHECKED);
    else lv_obj_remove_state(o, LV_STATE_CHECKED);
}

// ---------- header + navigation ----------

void buildHeader(lv_obj_t* scr) {
    lv_obj_t* logo = lv_image_create(scr);  // BK mark before the room name
    lv_image_set_src(logo, &bk_logo_32);
    lv_obj_set_pos(logo, kMargin, 20);
    ui.name = label(scr, &hmi_font_16_sb, text(), s_snap.room);
    lv_obj_set_pos(ui.name, kMargin + 42, 16);
    ui.caption = label(scr, &hmi_font_13_r, muted(), "Đang kết nối…");
    lv_obj_set_pos(ui.caption, kMargin + 42, 38);

    lv_obj_t* right = flexRow(scr, 12);
    lv_obj_set_height(right, 40);
    lv_obj_align(right, LV_ALIGN_TOP_RIGHT, -kMargin, 16);
    ui.clock = label(right, &hmi_font_16_sb, text(), "--:--");
    ui.wifi = label(right, &hmi_icons_20, muted(), ICON_WIFI);
    lv_obj_t* power = flexRow(right, 4);
    label(power, &hmi_icons_20, muted(), ICON_PLUG);
    label(power, &hmi_font_12_sb, muted(), "AC");
}

void selectPage(uint8_t p) {
    if (p >= P_COUNT || p == ui.current) return;
    lv_obj_add_flag(ui.page[ui.current], LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(ui.page[p], LV_OBJ_FLAG_HIDDEN);
    lv_obj_fade_in(ui.page[p], 200, 0);
    const uint8_t navIndex = p == P_WIFI ? P_SETTINGS : p;
    for (uint8_t i = 0; i < kNavCount; i++) {
        const bool on = i == navIndex;
        setChecked(ui.navBtn[i], on);
        lv_obj_set_style_text_color(ui.navIcon[i], on ? accent() : muted(), 0);
        lv_obj_set_style_text_color(ui.navText[i], on ? text() : muted(), 0);
    }
    ui.current = p;
    // Keep dynamic pages quiet while hidden; refresh their cached model when
    // they become visible so scrolling does not compete with off-screen work.
    if (p == P_SETTINGS) applySystem(s_snap);
    if (p == P_WIFI) applyWifiList(s_snap);
}

void buildNav(lv_obj_t* scr) {
    lv_obj_t* nav = box(scr);
    lv_obj_set_pos(nav, kMargin, kNavY);
    lv_obj_set_size(nav, 448, 56);
    lv_obj_set_style_bg_color(nav, surface(), 0);
    lv_obj_set_style_bg_opa(nav, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(nav, 16, 0);
    lv_obj_set_style_pad_all(nav, 4, 0);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(nav, 4, 0);

    static const char* const icons[kNavCount] = {ICON_HOUSE, ICON_SLIDERS, ICON_SETTINGS};
    static const char* const names[kNavCount] = {"Tổng quan", "Điều khiển", "Cài đặt"};
    for (uint8_t i = 0; i < kNavCount; i++) {
        lv_obj_t* b = box(nav);
        lv_obj_set_height(b, 48);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, surface(), 0);
        lv_obj_set_style_bg_color(b, elevated(), LV_STATE_CHECKED);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(b, 8, 0);
        makeClickable(b);
        ui.navBtn[i] = b;
        ui.navIcon[i] = label(b, &hmi_icons_20, i == 0 ? accent() : muted(), icons[i]);
        ui.navText[i] = label(b, &hmi_font_14_m, i == 0 ? text() : muted(), names[i]);
        lv_obj_add_event_cb(b, [](lv_event_t* e) {
            selectPage(static_cast<uint8_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e))));
        }, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
    }
    setChecked(ui.navBtn[0], true);
}

lv_obj_t* makePage(lv_obj_t* scr) {
    lv_obj_t* p = box(scr);
    lv_obj_set_pos(p, 0, kContentY);
    lv_obj_set_size(p, 480, kContentH);
    return p;
}

// ---------- home ----------

void gaugeAnimCb(void*, int32_t v) {
    ui.shown = v;
    lv_arc_set_value(ui.arc, v);
    char buf[12];
    snprintf(buf, sizeof(buf), "%.1f", v / 10.0f);
    lv_label_set_text(ui.value, buf);
}

void buildHome(lv_obj_t* page) {
    // Gauge box 232x232 at (124, 0) page coords = (124, 64) on screen.
    lv_obj_t* g = box(page);
    lv_obj_set_pos(g, 124, 0);
    lv_obj_set_size(g, 232, 232);

    ui.arc = lv_arc_create(g);
    lv_obj_remove_style_all(ui.arc);
    lv_obj_set_size(ui.arc, 220, 220);  // stroke centre on r104 like the design
    lv_obj_center(ui.arc);
    lv_arc_set_rotation(ui.arc, 135);
    lv_arc_set_bg_angles(ui.arc, 0, 270);
    lv_arc_set_range(ui.arc, lroundf(HMI_PRIMARY_MIN * 10), lroundf(HMI_PRIMARY_MAX * 10));
    lv_arc_set_value(ui.arc, lroundf(HMI_PRIMARY_MIN * 10));
    lv_obj_set_style_arc_width(ui.arc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ui.arc, elevated(), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(ui.arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui.arc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ui.arc, accent(), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(ui.arc, true, LV_PART_INDICATOR);
    lv_obj_remove_flag(ui.arc, LV_OBJ_FLAG_CLICKABLE);
    lv_group_remove_obj(ui.arc);
    ui.shown = lroundf(HMI_PRIMARY_MIN * 10);

    ui.spinner = lv_spinner_create(g);
    lv_obj_remove_style_all(ui.spinner);
    lv_obj_set_size(ui.spinner, 220, 220);
    lv_obj_center(ui.spinner);
    lv_spinner_set_anim_params(ui.spinner, 1000, 60);
    lv_obj_set_style_arc_opa(ui.spinner, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui.spinner, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ui.spinner, accent(), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(ui.spinner, true, LV_PART_INDICATOR);
    lv_obj_add_flag(ui.spinner, LV_OBJ_FLAG_HIDDEN);

    // Warning threshold notch: 3 px cut across the arc in background colour.
    static lv_point_precise_t notch[2];
    const float frac = (HMI_PRIMARY_WARN - HMI_PRIMARY_MIN) / (HMI_PRIMARY_MAX - HMI_PRIMARY_MIN);
    const float a = (135.0f + 270.0f * frac) * (float)M_PI / 180.0f;
    notch[0] = {116 + 96 * cosf(a), 116 + 96 * sinf(a)};
    notch[1] = {116 + 112 * cosf(a), 116 + 112 * sinf(a)};
    lv_obj_t* line = lv_line_create(g);
    lv_obj_remove_style_all(line);
    lv_line_set_points(line, notch, 2);
    lv_obj_set_style_line_width(line, 3, 0);
    lv_obj_set_style_line_color(line, bg(), 0);

    lv_obj_t* cap = label(g, &hmi_font_12_sb, muted(), HMI_PRIMARY_LABEL);
    lv_obj_set_style_text_letter_space(cap, 1, 0);
    lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 54);

    lv_obj_t* row = flexRow(g, 2, LV_FLEX_ALIGN_START);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 80);
    ui.value = label(row, &hmi_font_64_sb, text(), "--.-");
    lv_obj_t* unit = label(row, &hmi_font_20_m, muted(), HMI_PRIMARY_UNIT);
    lv_obj_set_style_pad_top(unit, 14, 0);

    ui.sub = label(g, &hmi_font_13_r, muted(), "");
    lv_obj_align(ui.sub, LV_ALIGN_TOP_MID, 0, 154);

    ui.pill = flexRow(g, 6);
    lv_obj_set_height(ui.pill, 28);
    lv_obj_set_style_pad_hor(ui.pill, 12, 0);
    lv_obj_set_style_radius(ui.pill, 14, 0);
    lv_obj_set_style_bg_opa(ui.pill, LV_OPA_COVER, 0);
    lv_obj_align(ui.pill, LV_ALIGN_TOP_MID, 0, 196);
    ui.pillDot = box(ui.pill);
    lv_obj_set_size(ui.pillDot, 8, 8);
    lv_obj_set_style_radius(ui.pillDot, 4, 0);
    lv_obj_set_style_bg_opa(ui.pillDot, LV_OPA_COVER, 0);
    ui.pillIcon = label(ui.pill, &hmi_icons_14, muted(), ICON_ALERT);
    ui.pillText = label(ui.pill, &hmi_font_12_sb, muted(), "LOADING");
    lv_obj_set_style_text_letter_space(ui.pillText, 1, 0);

    char buf[8];
    snprintf(buf, sizeof(buf), "%.0f", HMI_PRIMARY_MIN);
    lv_obj_t* lo = label(page, &hmi_font_12_sb, muted(), buf);
    snprintf(buf, sizeof(buf), "%.0f", HMI_PRIMARY_MAX);
    lv_obj_t* hi = label(page, &hmi_font_12_sb, muted(), buf);
    for (lv_obj_t* l : {lo, hi}) {
        lv_obj_set_width(l, 32);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    }
    // Below the arc's rounded end caps (arc ends at x 166 / 314, y 254 on screen).
    lv_obj_set_pos(lo, 150, 202);
    lv_obj_set_pos(hi, 298, 202);

    static const char* const mLabel[2] = {HMI_METRIC1_LABEL, HMI_METRIC2_LABEL};
    static const char* const mUnit[2] = {HMI_METRIC1_UNIT, HMI_METRIC2_UNIT};
    for (int i = 0; i < 2; i++) {
        lv_obj_t* c = cardObj(page);
        lv_obj_set_pos(c, i == 0 ? 16 : 244, 240);
        lv_obj_set_size(c, 220, 88);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(c, 12, 0);
        iconChip(c, i == 0 ? ICON_DROPLET : ICON_ZAP, i == 0 ? info() : accent2());
        lv_obj_t* col = flexCol(c, 0);
        label(col, &hmi_font_13_r, muted(), mLabel[i]);
        lv_obj_t* vr = flexRow(col, 4, LV_FLEX_ALIGN_END);
        ui.metric[i] = label(vr, &hmi_font_28_sb, text(), "--");
        lv_obj_t* u = label(vr, &hmi_font_14_m, muted(), mUnit[i]);
        // Bottom-aligned in the row: lift the unit so both baselines match.
        lv_obj_set_style_pad_bottom(u, hmi_font_28_sb.base_line - hmi_font_14_m.base_line, 0);
    }
}

void setPill(const char* txt, lv_color_t color, const char* icon, bool tinted) {
    lv_obj_set_style_bg_color(ui.pill, tinted ? tint(color) : elevated(), 0);
    lv_obj_set_style_text_color(ui.pillText, color, 0);
    lv_label_set_text(ui.pillText, txt);
    if (icon) {
        lv_obj_add_flag(ui.pillDot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui.pillIcon, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(ui.pillIcon, icon);
        lv_obj_set_style_text_color(ui.pillIcon, color, 0);
    } else {
        lv_obj_add_flag(ui.pillIcon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui.pillDot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(ui.pillDot, color, 0);
    }
}

void animateTo(float value) {
    const int32_t target = lroundf(value * 10);
    lv_anim_delete(&ui, gaugeAnimCb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &ui);
    lv_anim_set_exec_cb(&a, gaugeAnimCb);
    lv_anim_set_values(&a, ui.shown, target);
    lv_anim_set_duration(&a, 250);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

void applyPrimary(const Model& m) {
    const Metric& p = m.primary;
    const bool hasValue = p.state == SensorState::Ok || p.state == SensorState::Stale;
    char buf[48];

    if (p.state == SensorState::Loading) lv_obj_remove_flag(ui.spinner, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(ui.spinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_arc_opa(ui.arc, hasValue ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(ui.value, p.state == SensorState::Stale ? muted() : text(), 0);

    switch (p.state) {
        case SensorState::Loading:
            lv_anim_delete(&ui, gaugeAnimCb);
            lv_label_set_text(ui.value, "--.-");
            lv_label_set_text(ui.sub, "Đang đọc cảm biến…");
            setPill("LOADING", muted(), nullptr, false);
            return;
        case SensorState::Error:
            lv_anim_delete(&ui, gaugeAnimCb);
            lv_label_set_text(ui.value, "--.-");
            lv_label_set_text(ui.sub, "Lỗi đọc cảm biến");
            setPill("ERROR", error(), ICON_ERROR, true);
            return;
        case SensorState::Stale:
            lv_obj_set_style_arc_color(ui.arc, track(), LV_PART_INDICATOR);
            snprintf(buf, sizeof(buf), "Dữ liệu lúc %s", m.lastOkClock);
            lv_label_set_text(ui.sub, buf);
            setPill("OFFLINE", muted(), ICON_WIFI_OFF, false);
            break;
        case SensorState::Ok: {
            const bool warn = p.value >= HMI_PRIMARY_WARN;
            lv_obj_set_style_arc_color(ui.arc, warn ? warning() : accent(), LV_PART_INDICATOR);
            snprintf(buf, sizeof(buf), warn ? "Cao hơn ngưỡng %.0f %s" : "Ngưỡng cảnh báo %.0f %s",
                     HMI_PRIMARY_WARN, HMI_PRIMARY_UNIT);
            lv_label_set_text(ui.sub, buf);
            if (warn) setPill("WARNING", warning(), ICON_ALERT, true);
            else setPill("ACTIVE", success(), nullptr, true);
            break;
        }
    }
    animateTo(p.value);
}

void applyMetrics(const Model& m) {
    for (int i = 0; i < 2; i++) {
        const Metric& mt = m.metric[i];
        const bool has = mt.state == SensorState::Ok || mt.state == SensorState::Stale;
        char buf[12];
        if (has) snprintf(buf, sizeof(buf), "%.0f", mt.value);
        else strlcpy(buf, "--", sizeof(buf));
        lv_label_set_text(ui.metric[i], buf);
        lv_obj_set_style_text_color(ui.metric[i], mt.state == SensorState::Stale ? muted() : text(), 0);
    }
}

// ---------- control ----------

lv_obj_t* sectionTitle(lv_obj_t* parent, const char* t);
lv_obj_t* settingsCard(lv_obj_t* parent);
void rowPressable(lv_obj_t* row);

// Vertical scrolling page with flex column layout (Control, Settings).
void makeScrollPage(lv_obj_t* page) {
    // LVGL hit-tests only CLICKABLE objects.  box() clears that flag, so a drag
    // starting on a gap, card, title or label used to resolve to the (not
    // scrollable) screen and the page never scrolled.  A clickable page catches
    // those drags; clickable rows still get their taps first.
    lv_obj_add_flag(page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_width(page, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(page, muted(), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(page, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page, 8, 0);
    lv_obj_set_style_pad_hor(page, kMargin, 0);
    lv_obj_set_style_pad_bottom(page, 8, 0);
}

// 60 px row: icon, name + status line, switch.  The whole row is the tap target.
lv_obj_t* switchRow(lv_obj_t* card, const char* icon, const char* name, bool last,
                    lv_obj_t** iconOut, lv_obj_t** subOut, lv_obj_t** swOut) {
    lv_obj_t* r = box(card);
    lv_obj_set_size(r, LV_PCT(100), 60);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 12, 0);
    lv_obj_set_style_pad_hor(r, 16, 0);
    if (!last) {
        lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_color(r, elevated(), 0);
    }
    rowPressable(r);
    *iconOut = label(r, &hmi_icons_20, muted(), icon);
    lv_obj_t* col = flexCol(r, 0);
    lv_obj_set_flex_grow(col, 1);
    label(col, &hmi_font_14_m, text(), name);
    *subOut = label(col, &hmi_font_13_r, muted(), "");
    *swOut = makeSwitch(r);
    return r;
}

void buildControl(lv_obj_t* page) {
    makeScrollPage(page);

    sectionTitle(page, "RELAY TRÊN PANEL");
    lv_obj_t* local = settingsCard(page);
    for (int i = 0; i < RELAY_COUNT; i++) {
        lv_obj_t* r = switchRow(local, kRelayIcons[i], kRelayNames[i], i == RELAY_COUNT - 1,
                                &ui.relayIcon[i], &ui.relayStatus[i], &ui.relaySwitch[i]);
        lv_obj_add_event_cb(r, [](lv_event_t* e) {
            const auto idx = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
            if (ui.cb.relayToggle) ui.cb.relayToggle(idx);
        }, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
    }

    // One group per Tasmota device: title + online state, then its channels.
    for (uint8_t d = 0; d < s_snap.deviceCount; d++) {
        const DeviceInfo& dev = s_snap.devices[d];
        lv_obj_t* head = box(page);
        lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
        char title[32];
        snprintf(title, sizeof(title), "%s", dev.name);
        for (char* c = title; *c; c++) *c = toupper(static_cast<unsigned char>(*c));  // ASCII names
        lv_obj_t* t = sectionTitle(head, title);
        lv_obj_set_flex_grow(t, 1);
        ui.devStatus[d] = label(head, &hmi_font_12_sb, muted(), "Đang kết nối…");
        lv_obj_set_style_pad_right(ui.devStatus[d], 4, 0);

        lv_obj_t* card = settingsCard(page);
        for (uint8_t c = 0; c < dev.channels && c < kMaxChannels; c++) {
            char name[24];
            if (dev.labels[c][0]) strlcpy(name, dev.labels[c], sizeof(name));
            else snprintf(name, sizeof(name), "Kênh %u", c + 1);
            lv_obj_t* r = switchRow(card, ICON_PLUG_ZAP, name, c + 1 == dev.channels,
                                    &ui.devIcon[d][c], &ui.devSub[d][c], &ui.devSwitch[d][c]);
            ui.devRow[d][c] = r;
            lv_obj_add_event_cb(r, [](lv_event_t* e) {
                const auto v = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
                if (ui.cb.deviceToggle) ui.cb.deviceToggle(v >> 4, v & 0x0F);
            }, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<uintptr_t>((d << 4) | c)));
        }
    }
}

void applyRelays(const Model& m) {
    for (int i = 0; i < RELAY_COUNT; i++) {
        const bool on = m.relay[i];
        setChecked(ui.relaySwitch[i], on);
        // Each device lights up in its own colour when on, grey when off.
        const lv_color_t onColor[RELAY_COUNT] = {lamp(), info(), success()};  // per palette
        lv_obj_set_style_text_color(ui.relayIcon[i], on ? onColor[i] : muted(), 0);
        lv_label_set_text_fmt(ui.relayStatus[i], "%s · Kênh %d", on ? "Đang bật" : "Đã tắt", i + 1);
    }
}

void applyDevices(const Model& m) {
    for (uint8_t d = 0; d < m.deviceCount; d++) {
        const bool online = m.deviceOnline[d];
        if (!m.deviceKnown[d]) {
            lv_label_set_text(ui.devStatus[d], "Đang kết nối…");
            lv_obj_set_style_text_color(ui.devStatus[d], muted(), 0);
        } else if (online) {
            lv_label_set_text(ui.devStatus[d], m.devices[d].host);
            lv_obj_set_style_text_color(ui.devStatus[d], muted(), 0);
        } else {
            lv_label_set_text(ui.devStatus[d], "Ngoại tuyến");
            lv_obj_set_style_text_color(ui.devStatus[d], warning(), 0);
        }
        for (uint8_t c = 0; c < m.devices[d].channels && c < kMaxChannels; c++) {
            const bool on = online && (m.deviceMask[d] >> c) & 1;
            setChecked(ui.devSwitch[d][c], on);
            lv_obj_set_style_text_color(ui.devIcon[d][c], on ? lamp() : muted(), 0);
            lv_label_set_text(ui.devSub[d][c], !m.deviceKnown[d] ? "Đang kết nối…"
                                               : !online ? "Không phản hồi" : (on ? "Đang bật" : "Đã tắt"));
            // Offline channels are visibly disabled and ignore taps.
            lv_obj_set_style_opa(ui.devRow[d][c], online ? LV_OPA_COVER : LV_OPA_50, 0);
            if (online) lv_obj_add_flag(ui.devRow[d][c], LV_OBJ_FLAG_CLICKABLE);
            else lv_obj_remove_flag(ui.devRow[d][c], LV_OBJ_FLAG_CLICKABLE);
        }
    }
}

// Brightness: label row + slider, first entry of the Settings display card.
void buildBrightness(lv_obj_t* card) {
    lv_obj_t* r = box(card);
    lv_obj_set_size(r, LV_PCT(100), 84);
    lv_obj_set_style_pad_hor(r, 16, 0);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_border_color(r, elevated(), 0);
    lv_obj_t* icon = label(r, &hmi_icons_20, muted(), ICON_SUN);
    lv_obj_set_pos(icon, 0, 16);
    lv_obj_t* t = label(r, &hmi_font_14_m, text(), "Độ sáng màn hình");
    lv_obj_set_pos(t, 32, 16);
    ui.sliderValue = label(r, &hmi_font_14_m, muted(), "");
    lv_obj_align(ui.sliderValue, LV_ALIGN_TOP_RIGHT, 0, 16);

    ui.slider = lv_slider_create(r);
    lv_obj_remove_style_all(ui.slider);
    lv_obj_set_size(ui.slider, 400, 8);
    lv_obj_align(ui.slider, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_slider_set_range(ui.slider, 10, 100);
    lv_obj_set_ext_click_area(ui.slider, 20);
    lv_obj_set_style_bg_opa(ui.slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui.slider, elevated(), LV_PART_MAIN);
    lv_obj_set_style_radius(ui.slider, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui.slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ui.slider, accent(), LV_PART_INDICATOR);
    lv_obj_set_style_radius(ui.slider, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(ui.slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_bg_color(ui.slider, text(), LV_PART_KNOB);
    lv_obj_set_style_radius(ui.slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(ui.slider, 10, LV_PART_KNOB);
    lv_obj_add_style(ui.slider, &focusRing, LV_STATE_FOCUS_KEY);
    lv_obj_add_event_cb(ui.slider, [](lv_event_t*) {
        const auto v = static_cast<uint8_t>(lv_slider_get_value(ui.slider));
        lv_label_set_text_fmt(ui.sliderValue, "%u%%", v);
        if (ui.cb.backlight) ui.cb.backlight(v);  // live preview while dragging
    }, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(ui.slider, [](lv_event_t*) {
        const auto v = static_cast<uint8_t>(lv_slider_get_value(ui.slider));
        setBrightness(v);
        if (ui.cb.brightnessChanged) ui.cb.brightnessChanged(v);
    }, LV_EVENT_RELEASED, nullptr);
}

// ---------- settings ----------

lv_obj_t* sectionTitle(lv_obj_t* parent, const char* t) {
    lv_obj_t* l = label(parent, &hmi_font_12_sb, muted(), t);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    lv_obj_set_style_pad_left(l, 4, 0);
    lv_obj_set_style_pad_top(l, 8, 0);
    return l;
}

lv_obj_t* settingsCard(lv_obj_t* parent) {
    lv_obj_t* c = cardObj(parent);
    lv_obj_set_width(c, 448);
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, 0, 0);  // rows pad themselves so press feedback fills the card
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    return c;
}

// One 52 px settings row; returns its right-hand value label.
lv_obj_t* settingsRow(lv_obj_t* cardParent, const char* icon, const char* name, bool last, lv_obj_t** rowOut = nullptr) {
    lv_obj_t* r = box(cardParent);
    lv_obj_set_size(r, LV_PCT(100), 52);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 12, 0);
    lv_obj_set_style_pad_hor(r, 16, 0);
    if (!last) {
        lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_color(r, elevated(), 0);
    }
    label(r, &hmi_icons_20, muted(), icon);
    label(r, &hmi_font_14_m, text(), name);
    if (rowOut) *rowOut = r;
    // The value takes the remaining width, right-aligned, "…" if too long.
    lv_obj_t* v = label(r, &hmi_font_14_m, muted(), "–");
    lv_obj_set_flex_grow(v, 1);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    return v;
}

void addChevron(lv_obj_t* row) { label(row, &hmi_icons_20, muted(), ICON_CHEVRON); }

void rowPressable(lv_obj_t* row) {
    lv_obj_set_style_radius(row, 16, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, surface(), 0);
    makeClickable(row);
}

void openWifiPage();
void applyStorage(const Model& m);

void buildSettings(lv_obj_t* page) {
    makeScrollPage(page);

    // --- connection ---
    sectionTitle(page, "KẾT NỐI");
    lv_obj_t* net = settingsCard(page);
    lv_obj_t* wifiRow = nullptr;
    ui.sysWifi = settingsRow(net, ICON_WIFI, "Wi-Fi", true, &wifiRow);
    addChevron(wifiRow);
    rowPressable(wifiRow);
    lv_obj_add_event_cb(wifiRow, [](lv_event_t*) { openWifiPage(); }, LV_EVENT_CLICKED, nullptr);

    // --- display ---
    sectionTitle(page, "MÀN HÌNH");
    lv_obj_t* display = settingsCard(page);
    buildBrightness(display);
    lv_obj_t* dimRow = nullptr;
    char dimText[40];
    snprintf(dimText, sizeof(dimText), "Tự giảm sáng sau %d giây", HMI_DIM_AFTER_MS / 1000);
    lv_obj_t* dimValue = settingsRow(display, ICON_CLOCK, dimText, false, &dimRow);
    lv_label_set_text(dimValue, "");  // spacer pushing the switch to the right
    ui.dimSwitch = makeSwitch(dimRow);
    rowPressable(dimRow);
    lv_obj_add_event_cb(dimRow, [](lv_event_t*) {
        const bool on = !s_snap.autoDim;
        setAutoDim(on);
        if (ui.cb.autoDimChanged) ui.cb.autoDimChanged(on);
    }, LV_EVENT_CLICKED, nullptr);

    // Colour palette: tap cycles Lime -> Ember -> Cobalt; the name and a dot
    // are drawn in the palette's own accent so the choice previews itself.
    lv_obj_t* themeRow = nullptr;
    lv_obj_t* themeValue = settingsRow(display, ICON_PALETTE, "Màu chủ đạo", true, &themeRow);
    lv_label_set_text(themeValue, palette().name);
    lv_obj_set_style_text_color(themeValue, accent(), 0);
    lv_obj_t* dot = box(themeRow);
    lv_obj_set_size(dot, 12, 12);
    lv_obj_set_style_radius(dot, 6, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, accent(), 0);
    rowPressable(themeRow);
    lv_obj_add_event_cb(themeRow, [](lv_event_t*) {
        const uint8_t next = (s_snap.theme + 1) % kPaletteCount;
        setTheme(next);  // rebuild happens in hmi::loop(), not inside this event
        if (ui.cb.themeChanged) ui.cb.themeChanged(next);
    }, LV_EVENT_CLICKED, nullptr);

    // --- SD card ---
    sectionTitle(page, "THẺ NHỚ");
    lv_obj_t* sd = settingsCard(page);
    ui.sdCard = settingsRow(sd, ICON_DRIVE, "Thẻ nhớ", false);
    ui.sdConfig = settingsRow(sd, ICON_FILE, "config.json", false);
    lv_obj_t* fwRow = nullptr;
    ui.sdFw = settingsRow(sd, ICON_DOWNLOAD, "Cập nhật firmware", true, &fwRow);
    rowPressable(fwRow);
    lv_obj_add_event_cb(fwRow, [](lv_event_t*) {
        // Two taps: the first arms (4 s), the second starts flashing.
        if (!s_snap.storage.firmwareKB) return;
        if (!ui.fwArmed) {
            ui.fwArmed = true;
            lv_label_set_text(ui.sdFw, "Chạm lần nữa để cài đặt");
            lv_obj_set_style_text_color(ui.sdFw, warning(), 0);
            lv_timer_t* t = lv_timer_create([](lv_timer_t*) {
                ui.fwArmed = false;
                applyStorage(s_snap);
            }, 4000, nullptr);
            lv_timer_set_repeat_count(t, 1);
            return;
        }
        ui.fwArmed = false;
        if (ui.cb.firmwareUpdate) ui.cb.firmwareUpdate();
    }, LV_EVENT_CLICKED, nullptr);

    // --- system ---
    sectionTitle(page, "TRẠNG THÁI HỆ THỐNG");
    lv_obj_t* sys = settingsCard(page);
    ui.sysMqtt = settingsRow(sys, ICON_SERVER, "MQTT", false);
    ui.sysIp = settingsRow(sys, ICON_GLOBE, "Địa chỉ IP", false);
    ui.sysMem = settingsRow(sys, ICON_MEMORY, "Bộ nhớ trống", false);
    ui.sysUptime = settingsRow(sys, ICON_CLOCK, "Thời gian chạy", true);

    // --- about: HCMUT identity + firmware ---
    sectionTitle(page, "GIỚI THIỆU");
    lv_obj_t* about = settingsCard(page);
    lv_obj_set_style_pad_all(about, 16, 0);
    lv_obj_set_flex_flow(about, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(about, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(about, 14, 0);
    lv_obj_t* mark = lv_image_create(about);
    lv_image_set_src(mark, &bk_logo_48);
    lv_obj_t* col = flexCol(about, 2);
    label(col, &hmi_font_12_sb, muted(), "ĐẠI HỌC QUỐC GIA TP.HCM");
    label(col, &hmi_font_14_m, text(), "TRƯỜNG ĐẠI HỌC BÁCH KHOA");
    label(col, &hmi_font_13_r, muted(), "Smart Home Panel · v" FIRMWARE_VERSION);
}

void applyStorage(const Model& m) {
    const StorageInfo& st = m.storage;
    char buf[48];
    if (st.present) {
        snprintf(buf, sizeof(buf), "Trống %.1f / %.1f GB", (st.totalMB - st.usedMB) / 1024.0f, st.totalMB / 1024.0f);
        lv_label_set_text(ui.sdCard, buf);
        lv_obj_set_style_text_color(ui.sdCard, muted(), 0);
    } else {
        lv_label_set_text(ui.sdCard, "Không có thẻ");
        lv_obj_set_style_text_color(ui.sdCard, warning(), 0);
    }
    lv_label_set_text(ui.sdConfig, st.configLoaded ? "Đã nạp" : (st.present ? "Không có tệp" : "–"));
    lv_obj_set_style_text_color(ui.sdConfig, st.configLoaded ? success() : muted(), 0);
    if (ui.fwArmed) return;
    if (st.firmwareKB) {
        snprintf(buf, sizeof(buf), "firmware.bin · %.1f MB", st.firmwareKB / 1024.0f);
        lv_label_set_text(ui.sdFw, buf);
        lv_obj_set_style_text_color(ui.sdFw, accent(), 0);
    } else {
        lv_label_set_text(ui.sdFw, "Không có tệp");
        lv_obj_set_style_text_color(ui.sdFw, muted(), 0);
    }
}

// ---------- Wi-Fi page ----------

void openWifiPage() {
    selectPage(P_WIFI);
    if (ui.cb.wifiScan) ui.cb.wifiScan();
}

void openSheet(const WifiNetwork& n);

void buildWifi(lv_obj_t* page) {
    lv_obj_t* back = iconButton(page, ICON_BACK, "Quay lại");
    lv_obj_set_pos(back, kMargin, 0);
    lv_obj_add_event_cb(back, [](lv_event_t*) { selectPage(P_SETTINGS); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* title = label(page, &hmi_font_16_sb, text(), "Wi-Fi");
    lv_obj_set_pos(title, 72, 13);

    ui.wifiRefresh = iconButton(page, ICON_REFRESH, "Quét lại");
    lv_obj_set_pos(ui.wifiRefresh, 480 - kMargin - 48, 0);
    lv_obj_add_event_cb(ui.wifiRefresh, [](lv_event_t*) {
        if (ui.cb.wifiScan) ui.cb.wifiScan();
    }, LV_EVENT_CLICKED, nullptr);
    ui.wifiSpinner = lv_spinner_create(page);
    lv_obj_remove_style_all(ui.wifiSpinner);
    lv_obj_set_size(ui.wifiSpinner, 22, 22);
    lv_obj_set_pos(ui.wifiSpinner, 480 - kMargin - 35, 13);
    lv_spinner_set_anim_params(ui.wifiSpinner, 900, 90);
    lv_obj_set_style_arc_opa(ui.wifiSpinner, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui.wifiSpinner, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ui.wifiSpinner, accent(), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(ui.wifiSpinner, true, LV_PART_INDICATOR);
    lv_obj_add_flag(ui.wifiSpinner, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* scroll = box(page);
    lv_obj_set_pos(scroll, kMargin, 56);
    lv_obj_set_size(scroll, 448, kContentH - 56);
    lv_obj_add_flag(scroll, LV_OBJ_FLAG_CLICKABLE);  // see makeScrollPage()
    lv_obj_add_flag(scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_width(scroll, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(scroll, muted(), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_50, LV_PART_SCROLLBAR);
    ui.wifiList = settingsCard(scroll);
    lv_obj_set_style_pad_bottom(scroll, 8, 0);
}

const char* signalIcon(int8_t rssi) {
    if (rssi >= -60) return ICON_WIFI;
    if (rssi >= -72) return ICON_WIFI_HIGH;
    return ICON_WIFI_LOW;
}

void applyWifiList(const Model& m) {
    strlcpy(ui.wifiRenderedSsid, m.sys.ssid, sizeof(ui.wifiRenderedSsid));
    if (m.scanning) {
        lv_obj_add_flag(ui.wifiRefresh, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui.wifiSpinner, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(ui.wifiRefresh, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui.wifiSpinner, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clean(ui.wifiList);
    const bool linked = m.link == Link::Online || m.link == Link::BrokerDown;
    if (m.networkCount == 0) {
        lv_obj_t* r = box(ui.wifiList);
        lv_obj_set_size(r, LV_PCT(100), 56);
        lv_obj_t* l = label(r, &hmi_font_14_m, muted(), m.scanning ? "Đang tìm mạng…" : "Không tìm thấy mạng nào");
        lv_obj_center(l);
        return;
    }
    for (uint8_t i = 0; i < m.networkCount; i++) {
        const WifiNetwork& n = m.networks[i];
        const bool current = linked && strcmp(n.ssid, m.sys.ssid) == 0;
        lv_obj_t* r = box(ui.wifiList);
        lv_obj_set_size(r, LV_PCT(100), 56);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(r, 12, 0);
        lv_obj_set_style_pad_hor(r, 16, 0);
        if (i + 1 < m.networkCount) {
            lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_width(r, 1, 0);
            lv_obj_set_style_border_color(r, elevated(), 0);
        }
        rowPressable(r);
        label(r, &hmi_icons_20, current ? accent() : text(), signalIcon(n.rssi));
        lv_obj_t* name = label(r, &hmi_font_14_m, text(), n.ssid);
        lv_obj_set_flex_grow(name, 1);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        if (current) label(r, &hmi_font_13_r, success(), "Đã kết nối");
        if (n.secure) label(r, &hmi_icons_20, muted(), ICON_LOCK);
        lv_obj_add_event_cb(r, [](lv_event_t* e) {
            const auto idx = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
            if (idx < s_snap.networkCount) openSheet(s_snap.networks[idx]);
        }, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
    }
}

// ---------- join sheet: password + keyboard ----------

void closeSheet() {
    lv_obj_add_flag(ui.sheet, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_text(ui.pass, "");
}

void setJoinStatus(const char* txt, lv_color_t color, bool busy) {
    lv_label_set_text(ui.joinStatus, txt);
    lv_obj_set_style_text_color(ui.joinStatus, color, 0);
    if (busy) {
        lv_obj_remove_flag(ui.joinSpinner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_x(ui.joinStatus, kMargin + 30);
        lv_obj_add_state(ui.joinBtn, LV_STATE_DISABLED);
        lv_obj_set_style_text_color(ui.joinBtnText, muted(), 0);
    } else {
        lv_obj_add_flag(ui.joinSpinner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_x(ui.joinStatus, kMargin);
        lv_obj_remove_state(ui.joinBtn, LV_STATE_DISABLED);
        lv_obj_set_style_text_color(ui.joinBtnText, bg(), 0);
    }
}

void openSheet(const WifiNetwork& n) {
    strlcpy(ui.joinSsid, n.ssid, sizeof(ui.joinSsid));
    ui.joinSecure = n.secure;
    lv_label_set_text(ui.sheetSsid, n.ssid);
    lv_textarea_set_text(ui.pass, "");
    lv_textarea_set_password_mode(ui.pass, true);
    lv_label_set_text(lv_obj_get_child(ui.eye, 0), ICON_EYE);
    const lv_obj_flag_t h = LV_OBJ_FLAG_HIDDEN;
    if (n.secure) {
        lv_obj_remove_flag(ui.pass, h);
        lv_obj_remove_flag(ui.eye, h);
        lv_obj_remove_flag(ui.kb, h);
        setJoinStatus("", muted(), false);
    } else {
        lv_obj_add_flag(ui.pass, h);
        lv_obj_add_flag(ui.eye, h);
        lv_obj_add_flag(ui.kb, h);
        setJoinStatus("Mạng mở, không cần mật khẩu", muted(), false);
    }
    lv_obj_remove_flag(ui.sheet, h);
    lv_obj_fade_in(ui.sheet, 200, 0);
}

void submitJoin() {
    if (lv_obj_has_state(ui.joinBtn, LV_STATE_DISABLED)) return;
    const char* pw = ui.joinSecure ? lv_textarea_get_text(ui.pass) : "";
    if (ui.joinSecure && strlen(pw) < 8) {
        setJoinStatus("Mật khẩu cần ít nhất 8 ký tự", error(), false);
        return;
    }
    if (ui.cb.wifiJoin) ui.cb.wifiJoin(ui.joinSsid, pw);
}

void buildSheet(lv_obj_t* scr) {
    ui.sheet = box(scr);
    lv_obj_set_size(ui.sheet, 480, 480);
    lv_obj_set_style_bg_opa(ui.sheet, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(ui.sheet, bg(), 0);
    lv_obj_add_flag(ui.sheet, LV_OBJ_FLAG_CLICKABLE);  // swallow taps meant for the page below
    lv_obj_add_flag(ui.sheet, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* back = iconButton(ui.sheet, ICON_BACK, "Đóng");
    lv_obj_set_pos(back, kMargin, 12);
    lv_obj_add_event_cb(back, [](lv_event_t*) { closeSheet(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* t = label(ui.sheet, &hmi_font_16_sb, text(), "Kết nối Wi-Fi");
    lv_obj_set_pos(t, 72, 14);
    ui.sheetSsid = label(ui.sheet, &hmi_font_13_r, muted(), "");
    lv_obj_set_pos(ui.sheetSsid, 72, 36);
    lv_label_set_long_mode(ui.sheetSsid, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ui.sheetSsid, 380);

    ui.pass = lv_textarea_create(ui.sheet);
    lv_obj_remove_style_all(ui.pass);
    lv_obj_set_pos(ui.pass, kMargin, 72);
    lv_obj_set_size(ui.pass, 448, 52);
    lv_obj_set_style_bg_opa(ui.pass, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(ui.pass, surface(), 0);
    lv_obj_set_style_radius(ui.pass, 12, 0);
    lv_obj_set_style_pad_left(ui.pass, 16, 0);
    lv_obj_set_style_pad_right(ui.pass, 56, 0);
    lv_obj_set_style_pad_ver(ui.pass, 14, 0);
    lv_obj_set_style_border_width(ui.pass, 1, LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(ui.pass, accent(), LV_STATE_FOCUSED);
    lv_obj_set_style_text_font(ui.pass, &hmi_font_16_sb, 0);
    lv_obj_set_style_text_color(ui.pass, text(), 0);
    lv_obj_set_style_text_color(ui.pass, muted(), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_border_side(ui.pass, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR);
    lv_obj_set_style_border_width(ui.pass, 2, LV_PART_CURSOR);
    lv_obj_set_style_border_color(ui.pass, accent(), LV_PART_CURSOR);
    lv_obj_set_style_anim_duration(ui.pass, 500, LV_PART_CURSOR);
    lv_textarea_set_one_line(ui.pass, true);
    lv_textarea_set_password_mode(ui.pass, true);
    lv_textarea_set_password_bullet(ui.pass, "•");
    lv_textarea_set_max_length(ui.pass, 63);
    lv_textarea_set_placeholder_text(ui.pass, "Mật khẩu");
    lv_obj_add_state(ui.pass, LV_STATE_FOCUSED);

    ui.eye = iconButton(ui.sheet, ICON_EYE, "Hiện mật khẩu");
    lv_obj_set_style_bg_color(ui.eye, surface(), 0);
    lv_obj_set_pos(ui.eye, kMargin + 448 - 50, 74);
    lv_obj_add_event_cb(ui.eye, [](lv_event_t*) {
        const bool hidden = !lv_textarea_get_password_mode(ui.pass);
        lv_textarea_set_password_mode(ui.pass, hidden);
        lv_label_set_text(lv_obj_get_child(ui.eye, 0), hidden ? ICON_EYE : ICON_EYE_OFF);
    }, LV_EVENT_CLICKED, nullptr);

    ui.joinSpinner = lv_spinner_create(ui.sheet);
    lv_obj_remove_style_all(ui.joinSpinner);
    lv_obj_set_size(ui.joinSpinner, 20, 20);
    lv_obj_set_pos(ui.joinSpinner, kMargin, 144);
    lv_spinner_set_anim_params(ui.joinSpinner, 900, 90);
    lv_obj_set_style_arc_opa(ui.joinSpinner, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui.joinSpinner, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ui.joinSpinner, accent(), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(ui.joinSpinner, true, LV_PART_INDICATOR);
    lv_obj_add_flag(ui.joinSpinner, LV_OBJ_FLAG_HIDDEN);
    ui.joinStatus = label(ui.sheet, &hmi_font_13_r, muted(), "");
    lv_obj_set_pos(ui.joinStatus, kMargin, 145);
    lv_obj_set_width(ui.joinStatus, 280);
    lv_label_set_long_mode(ui.joinStatus, LV_LABEL_LONG_DOT);

    ui.joinBtn = box(ui.sheet);
    lv_obj_set_pos(ui.joinBtn, 480 - kMargin - 140, 132);
    lv_obj_set_size(ui.joinBtn, 140, 44);
    lv_obj_set_style_radius(ui.joinBtn, 12, 0);
    lv_obj_set_style_bg_opa(ui.joinBtn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(ui.joinBtn, accent(), 0);
    lv_obj_set_style_bg_color(ui.joinBtn, elevated(), LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(ui.joinBtn, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_add_flag(ui.joinBtn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_style(ui.joinBtn, &focusRing, LV_STATE_FOCUS_KEY);
    lv_group_add_obj(lv_group_get_default(), ui.joinBtn);
    ui.joinBtnText = label(ui.joinBtn, &hmi_font_14_m, bg(), "Kết nối");
    lv_obj_center(ui.joinBtnText);
    lv_obj_add_event_cb(ui.joinBtn, [](lv_event_t*) { submitJoin(); }, LV_EVENT_CLICKED, nullptr);

    // Keyboard keys use Montserrat for the LV_SYMBOL_* glyphs (passwords are ASCII).
    ui.kb = lv_keyboard_create(ui.sheet);
    lv_obj_set_pos(ui.kb, 0, 188);
    lv_obj_set_size(ui.kb, 480, 292);
    lv_obj_set_style_bg_color(ui.kb, bg(), 0);
    lv_obj_set_style_bg_opa(ui.kb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui.kb, 0, 0);
    lv_obj_set_style_radius(ui.kb, 0, 0);
    lv_obj_set_style_pad_all(ui.kb, 8, 0);
    lv_obj_set_style_pad_row(ui.kb, 6, 0);
    lv_obj_set_style_pad_column(ui.kb, 6, 0);
    lv_obj_set_style_bg_color(ui.kb, surface(), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(ui.kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(ui.kb, elevated(), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(ui.kb, track(), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(ui.kb, text(), LV_PART_ITEMS);
    lv_obj_set_style_text_color(ui.kb, text(), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_font(ui.kb, &lv_font_montserrat_16, LV_PART_ITEMS);
    lv_obj_set_style_radius(ui.kb, 10, LV_PART_ITEMS);
    lv_obj_set_style_border_width(ui.kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(ui.kb, 0, LV_PART_ITEMS);
    lv_keyboard_set_textarea(ui.kb, ui.pass);
    lv_obj_add_event_cb(ui.kb, [](lv_event_t*) { submitJoin(); }, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(ui.kb, [](lv_event_t*) { closeSheet(); }, LV_EVENT_CANCEL, nullptr);
}

void applyJoin(const Model& m) {
    if (lv_obj_has_flag(ui.sheet, LV_OBJ_FLAG_HIDDEN)) return;
    char buf[64];
    switch (m.join) {
        case WifiJoin::Idle:
            break;
        case WifiJoin::Connecting:
            setJoinStatus("Đang kết nối…", muted(), true);
            break;
        case WifiJoin::Failed:
            setJoinStatus(m.joinMessage, error(), false);
            break;
        case WifiJoin::Connected: {
            snprintf(buf, sizeof(buf), "Đã kết nối · %s", m.joinMessage);
            setJoinStatus(buf, success(), false);
            lv_obj_add_state(ui.joinBtn, LV_STATE_DISABLED);
            lv_timer_t* t = lv_timer_create([](lv_timer_t*) {
                closeSheet();
                selectPage(P_SETTINGS);
            }, 1500, nullptr);
            lv_timer_set_repeat_count(t, 1);
            break;
        }
    }
}

// ---------- firmware update overlay ----------

void buildUpdateOverlay(lv_obj_t* scr) {
    ui.update = box(scr);
    lv_obj_set_size(ui.update, 480, 480);
    lv_obj_set_style_bg_opa(ui.update, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(ui.update, bg(), 0);
    lv_obj_add_flag(ui.update, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ui.update, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_flex_flow(ui.update, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui.update, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(ui.update, 16, 0);
    iconChip(ui.update, ICON_DOWNLOAD, accent());
    label(ui.update, &hmi_font_16_sb, text(), "Đang cập nhật firmware");
    ui.updateBar = lv_bar_create(ui.update);
    lv_obj_remove_style_all(ui.updateBar);
    lv_obj_set_size(ui.updateBar, 320, 8);
    lv_obj_set_style_bg_opa(ui.updateBar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui.updateBar, elevated(), LV_PART_MAIN);
    lv_obj_set_style_radius(ui.updateBar, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui.updateBar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ui.updateBar, accent(), LV_PART_INDICATOR);
    lv_obj_set_style_radius(ui.updateBar, 4, LV_PART_INDICATOR);
    ui.updateText = label(ui.update, &hmi_font_13_r, muted(), "");
}

void applyUpdate(const Model& m) {
    if (m.updatePct < 0) {
        lv_obj_add_flag(ui.update, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(ui.update, LV_OBJ_FLAG_HIDDEN);
    lv_bar_set_value(ui.updateBar, m.updatePct, LV_ANIM_OFF);
    lv_label_set_text_fmt(ui.updateText, "%d%% · không tắt nguồn", m.updatePct);
}

void applySystem(const Model& m) {
    char buf[64];
    const SystemInfo& s = m.sys;
    switch (m.link) {
        case Link::Online:
        case Link::BrokerDown:
            snprintf(buf, sizeof(buf), "%s · %d dBm", s.ssid, s.rssi);
            lv_label_set_text(ui.sysWifi, buf);
            lv_obj_set_style_text_color(ui.sysWifi, muted(), 0);
            break;
        case Link::Offline:
            lv_label_set_text(ui.sysWifi, "Mất kết nối");
            lv_obj_set_style_text_color(ui.sysWifi, warning(), 0);
            break;
        case Link::Connecting:
            lv_label_set_text(ui.sysWifi, "Đang kết nối…");
            lv_obj_set_style_text_color(ui.sysWifi, muted(), 0);
            break;
    }
    const bool mqttOk = m.link == Link::Online;
    lv_label_set_text(ui.sysMqtt, mqttOk ? "Đã kết nối" : (m.link == Link::BrokerDown ? "Không kết nối được" : "Chờ Wi-Fi"));
    lv_obj_set_style_text_color(ui.sysMqtt, mqttOk ? success() : (m.link == Link::BrokerDown ? warning() : muted()), 0);
    lv_label_set_text(ui.sysIp, s.ip[0] ? s.ip : "–");
    snprintf(buf, sizeof(buf), "%lu KB · PSRAM %.1f MB", (unsigned long)s.heapKB, s.psramKB / 1024.0f);
    lv_label_set_text(ui.sysMem, buf);
    const uint32_t min = s.uptimeS / 60;
    if (min >= 60) snprintf(buf, sizeof(buf), "%lu giờ %lu phút", (unsigned long)(min / 60), (unsigned long)(min % 60));
    else snprintf(buf, sizeof(buf), "%lu phút", (unsigned long)min);
    lv_label_set_text(ui.sysUptime, buf);
}

// ---------- header state ----------

void applyLink(const Model& m) {
    static const char* const captions[] = {
        "Đang kết nối…", "Đã kết nối · MQTT", "Wi-Fi OK · MQTT mất kết nối", "Mất kết nối · đang thử lại"};
    const auto idx = static_cast<uint8_t>(m.link);
    const bool problem = m.link == Link::BrokerDown || m.link == Link::Offline;
    lv_label_set_text(ui.caption, captions[idx]);
    lv_obj_set_style_text_color(ui.caption, problem ? warning() : muted(), 0);
    lv_label_set_text(ui.wifi, m.link == Link::Offline ? ICON_WIFI_OFF : ICON_WIFI);
    lv_obj_set_style_text_color(ui.wifi,
        m.link == Link::Offline ? warning() : (m.link == Link::Connecting ? muted() : text()), 0);
}

// ---------- backlight / auto-dim ----------

void onWake() {
    ui.dimmed = false;
    if (ui.cb.backlight) ui.cb.backlight(s_snap.brightness);
    lv_display_trigger_activity(nullptr);
}

void updateDim() {
    const Model& m = s_snap;
    if (ui.dimmed || !m.autoDim) return;
    if (lv_display_get_inactive_time(nullptr) < HMI_DIM_AFTER_MS) return;
    ui.dimmed = true;
    if (ui.cb.backlight) ui.cb.backlight(HMI_DIM_BRIGHTNESS_PCT);
    port::armWakeGuard(onWake);
}

}  // namespace

// ---------- public ----------

// Boot splash with the HCMUT identity (brand guide lockup: logo, ĐHQG-HCM line,
// school name).  Shown once at power-up, fades out, then deletes itself; both
// the fade and the delete are bound to the object, so a rebuild during the
// splash simply removes it.
void buildSplash(lv_obj_t* scr) {
    lv_obj_t* s = box(scr);
    lv_obj_set_size(s, 480, 480);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s, bg(), 0);
    lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);  // swallow taps while it is up
    lv_obj_set_flex_flow(s, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s, 6, 0);

    lv_obj_t* logo = lv_image_create(s);
    lv_image_set_src(logo, &bk_logo_132);
    lv_obj_set_style_margin_bottom(logo, 22, 0);
    lv_obj_t* top = label(s, &hmi_font_12_sb, muted(), "ĐẠI HỌC QUỐC GIA TP.HCM");
    lv_obj_set_style_text_letter_space(top, 1, 0);
    label(s, &hmi_font_16_sb, text(), "TRƯỜNG ĐẠI HỌC BÁCH KHOA");
    lv_obj_t* bar = box(s);
    lv_obj_set_size(bar, 32, 3);
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x1488DB), 0);  // BK blue, whatever the palette
    lv_obj_set_style_margin_ver(bar, 10, 0);
    label(s, &hmi_font_13_r, muted(), "Smart Home Panel");

    lv_obj_fade_out(s, 350, 1800);
    lv_obj_delete_delayed(s, 2200);
}

// Build (or rebuild after a palette change) every widget for the current palette.
void buildAll() {
    const Callbacks cb = ui.cb;
    const uint8_t page = ui.current;
    const bool dimmed = ui.dimmed;
    lv_anim_delete(&ui, gaugeAnimCb);
    lv_obj_t* scr = lv_screen_active();
    lv_obj_clean(scr);
    ui = Ui{};
    ui.cb = cb;
    ui.dimmed = dimmed;

    theme::setPalette(s_snap.theme);
    theme::init();
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, bg(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, text(), 0);
    lv_obj_set_style_text_font(scr, &hmi_font_14_m, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    for (uint8_t p = 0; p < P_COUNT; p++) ui.page[p] = makePage(scr);
    buildHome(ui.page[P_HOME]);
    buildControl(ui.page[P_CONTROL]);
    buildSettings(ui.page[P_SETTINGS]);
    buildWifi(ui.page[P_WIFI]);
    for (uint8_t p = P_CONTROL; p < P_COUNT; p++) lv_obj_add_flag(ui.page[p], LV_OBJ_FLAG_HIDDEN);
    buildHeader(scr);
    buildNav(scr);
    buildSheet(scr);
    buildUpdateOverlay(scr);
    selectPage(page);
    static bool splashShown = false;
    if (!splashShown) {
        splashShown = true;
        buildSplash(scr);
    }
}

// ---------- UI task ----------

namespace {

enum ReqType : uint8_t { REQ_PAGE, REQ_SNAPSHOT, REQ_SCROLL };

struct Request {
    ReqType type;
    uint8_t page;
    lv_draw_buf_t** result;   // REQ_SNAPSHOT: filled by the UI task
    SemaphoreHandle_t done;   // REQ_SNAPSHOT: given when result is ready
};

QueueHandle_t s_requests = nullptr;
LGFX* s_lcd = nullptr;

void serveRequests() {
    Request r;
    while (xQueueReceive(s_requests, &r, 0) == pdTRUE) {
        if (r.type == REQ_PAGE) {
            selectPage(r.page);
        } else if (r.type == REQ_SCROLL) {
            lv_obj_scroll_by(ui.page[ui.current], 0, -static_cast<int8_t>(r.page) * 4, LV_ANIM_ON);
        } else {
            *r.result = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
            xSemaphoreGive(r.done);
        }
    }
}

void tick() {
    uint32_t d;
    {
        ModelLock lock;
        d = model().dirty;
        model().dirty = 0;
        s_snap = model();
    }
    if (d & D_THEME) {
        buildAll();
        d = D_ALL & ~D_THEME;  // fresh widgets need every value
    }
    const Model& m = s_snap;
    if (d & D_PRIMARY) applyPrimary(m);
    if (d & D_METRICS) applyMetrics(m);
    if (d & D_LINK) applyLink(m);
    if (d & D_CLOCK) lv_label_set_text(ui.clock, m.clock);
    if (d & D_RELAYS) applyRelays(m);
    if (d & D_BRIGHTNESS) {
        lv_slider_set_value(ui.slider, m.brightness, LV_ANIM_OFF);
        lv_label_set_text_fmt(ui.sliderValue, "%u%%", m.brightness);
        if (!ui.dimmed && ui.cb.backlight) ui.cb.backlight(m.brightness);
    }
    if (d & D_AUTODIM) setChecked(ui.dimSwitch, m.autoDim);
    if ((d & D_SYSTEM) && ui.current == P_SETTINGS) applySystem(m);
    const bool wifiSsidChanged = (d & D_SYSTEM) && strcmp(ui.wifiRenderedSsid, m.sys.ssid) != 0;
    if ((d & (D_WIFI_LIST | D_LINK) || wifiSsidChanged) && ui.current == P_WIFI) applyWifiList(m);
    if (d & D_WIFI_JOIN) applyJoin(m);
    if (d & D_STORAGE) applyStorage(m);
    if (d & D_UPDATE) applyUpdate(m);
    if (d & D_ROOM) lv_label_set_text(ui.name, m.room);
    if (d & D_DEVICES) applyDevices(m);

    serveRequests();
    updateDim();
    lv_timer_handler();
}

// The only task that ever calls LVGL.  Subscribed to the task watchdog: a UI
// stall longer than the WDT timeout reboots the panel (reset reason 6/7, logged).
void uiTask(void*) {
    port::begin(*s_lcd);
    {
        ModelLock lock;
        s_snap = model();
        model().dirty &= ~D_THEME;  // the first build below already uses the palette
    }
    buildAll();
    esp_task_wdt_add(nullptr);
    for (;;) {
        tick();
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

lv_draw_buf_t* takeSnapshot() {
    if (!s_requests) return nullptr;
    lv_draw_buf_t* buf = nullptr;
    StaticSemaphore_t storage;
    SemaphoreHandle_t done = xSemaphoreCreateBinaryStatic(&storage);
    Request r{REQ_SNAPSHOT, 0, &buf, done};
    if (xQueueSend(s_requests, &r, pdMS_TO_TICKS(500)) != pdTRUE) return nullptr;
    xSemaphoreTake(done, pdMS_TO_TICKS(5000));
    return buf;
}

}  // namespace

void begin(LGFX& lcd, const Callbacks& cb) {
    ui.cb = cb;
    s_lcd = &lcd;
    s_requests = xQueueCreate(8, sizeof(Request));
    xTaskCreatePinnedToCore(uiTask, "ui", 10 * 1024, nullptr, 3, nullptr, 1);
}

void showPage(uint8_t page) {
    Request r{REQ_PAGE, page, nullptr, nullptr};
    if (s_requests) xQueueSend(s_requests, &r, 0);
}

// Snapshot is taken by the UI task; encoding/streaming runs in the caller's
// task so a slow serial link or SD card never stalls the UI.
void injectDrag(int16_t x, int16_t y0, int16_t y1, uint16_t ms) { port::injectDrag(x, y0, y1, ms); }

void scrollPage(int16_t dy) {
    Request r{REQ_SCROLL, static_cast<uint8_t>(static_cast<int8_t>(constrain(dy / 4, -127, 127))), nullptr, nullptr};
    if (s_requests) xQueueSend(s_requests, &r, 0);
}

bool screenshotBmp(Print& out) {
    lv_draw_buf_t* snap = takeSnapshot();
    if (!snap) return false;
    const uint32_t w = snap->header.w, h = snap->header.h, stride = snap->header.stride;
    const uint32_t rowBytes = (w * 3 + 3) & ~3u;
    const uint32_t dataSize = rowBytes * h;
    auto le32 = [](uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; };
    uint8_t hdr[54] = {'B', 'M'};
    le32(hdr + 2, 54 + dataSize);
    le32(hdr + 10, 54);
    le32(hdr + 14, 40);
    le32(hdr + 18, w);
    le32(hdr + 22, h);
    hdr[26] = 1;   // planes
    hdr[28] = 24;  // bpp
    le32(hdr + 34, dataSize);
    out.write(hdr, sizeof(hdr));
    static uint8_t row[480 * 3 + 4];
    for (int32_t y = h - 1; y >= 0; y--) {  // BMP rows are bottom-up, BGR
        const auto* px = reinterpret_cast<const uint16_t*>(snap->data + y * stride);
        for (uint32_t x = 0; x < w; x++) {
            const uint16_t c = px[x];
            row[x * 3 + 0] = (c & 0x1F) << 3;
            row[x * 3 + 1] = ((c >> 5) & 0x3F) << 2;
            row[x * 3 + 2] = (c >> 11) << 3;
        }
        out.write(row, rowBytes);
    }
    lv_draw_buf_destroy(snap);
    return true;
}

void screenshot(Print& out) {
    lv_draw_buf_t* snap = takeSnapshot();
    if (!snap) {
        out.println("SHOT failed: no memory");
        return;
    }
    const uint32_t w = snap->header.w, h = snap->header.h, stride = snap->header.stride;
    out.printf("SHOT %lu %lu\n", (unsigned long)w, (unsigned long)h);
    // RLE records (u16 count, u16 byte-swapped RGB565) sent as "#<base64>" lines.
    // Each line is one write(), so log lines from other tasks can only land
    // between lines; tools/screenshot.py ignores anything not starting with '#'.
    static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint8_t raw[192];  // 48 records -> 256 base64 chars
    size_t rawLen = 0;
    char line[2 + 256 + 2];
    auto flush = [&]() {
        if (!rawLen) return;
        size_t o = 0;
        line[o++] = '#';
        for (size_t i = 0; i < rawLen; i += 3) {  // rawLen is a multiple of 4, pad as needed
            const uint32_t n = (raw[i] << 16) | ((i + 1 < rawLen ? raw[i + 1] : 0) << 8) | (i + 2 < rawLen ? raw[i + 2] : 0);
            line[o++] = kB64[(n >> 18) & 63];
            line[o++] = kB64[(n >> 12) & 63];
            line[o++] = i + 1 < rawLen ? kB64[(n >> 6) & 63] : '=';
            line[o++] = i + 2 < rawLen ? kB64[n & 63] : '=';
        }
        line[o++] = '\n';
        out.write(reinterpret_cast<const uint8_t*>(line), o);
        rawLen = 0;
    };
    auto emit = [&](uint16_t run, uint16_t color) {
        raw[rawLen++] = uint8_t(run);
        raw[rawLen++] = uint8_t(run >> 8);
        raw[rawLen++] = uint8_t(color);
        raw[rawLen++] = uint8_t(color >> 8);
        if (rawLen == sizeof(raw)) flush();
    };
    uint16_t runColor = 0;
    uint32_t run = 0;
    for (uint32_t y = 0; y < h; y++) {
        const auto* row = reinterpret_cast<const uint16_t*>(snap->data + y * stride);
        for (uint32_t x = 0; x < w; x++) {
            const uint16_t c = static_cast<uint16_t>((row[x] >> 8) | (row[x] << 8));
            if (run && (c != runColor || run == 0xFFFF)) {
                emit(run, runColor);
                run = 0;
            }
            runColor = c;
            run++;
        }
    }
    if (run) emit(run, runColor);
    flush();
    out.print("END\n");
    lv_draw_buf_destroy(snap);
}

}  // namespace hmi
