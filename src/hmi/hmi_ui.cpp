// LVGL widgets for the 480x480 HMI.  Layout values follow the "Pixel specs"
// board of the design canvas; page content sits in y 64..392 between the
// persistent header and bottom navigation.

#include <esp_heap_caps.h>
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
#include "lv_mem_psram.h"
#include "datalog.h"

namespace hmi {
namespace {

using namespace theme;

enum Page : uint8_t { P_HOME, P_CONTROL, P_SETTINGS, P_WIFI, P_DETAIL, P_FILES, P_FILEVIEW, P_COUNT };

// Detail pages opened from Settings rows.
enum Topic : uint8_t { T_NET, T_MQTT, T_MEMORY, T_UPTIME, T_SD, T_CONFIG, T_ABOUT, T_HISTORY, T_EVENTS, T_COUNT };
constexpr uint8_t kNavCount = 3;  // P_WIFI is a sub-page of Settings

struct Ui {
    Callbacks cb;
    // header
    lv_obj_t* header;  // logo, room, status, clock: Tổng quan only
    lv_obj_t* name;
    lv_obj_t* caption;
    lv_obj_t* clock;
    lv_obj_t* wifi;
    // navigation
    lv_obj_t* page[P_COUNT];
    lv_obj_t* nav;  // hidden on Settings sub-pages
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
    // data (PSRAM history + event log)
    lv_obj_t* dataHistory;
    lv_obj_t* dataEvents;
    uint8_t detailBack = P_SETTINGS;  // page the detail back arrow returns to
    uint8_t detailParent = T_COUNT;   // detail opened from another detail (T_COUNT: none)
    uint8_t histRange = datalog::R_24H;
    uint32_t evShown = 0;     // datalog::eventSerial() of the rendered event list
    uint32_t histShownAt = 0; // lv_tick of the last history render
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
    // SD card file browser
    lv_obj_t* filesTitle;
    lv_obj_t* filesList;
    char browsePath[96] = "/";
    char renderedPath[96] = "";
    lv_obj_t* viewTitle;
    lv_obj_t* viewList;
    char viewPath[96] = "";
    bool delArmed = false;
    // detail page
    lv_obj_t* detailTitle;
    lv_obj_t* detailList;
    uint8_t detailTopic = T_NET;
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
Model& s_snap = newModel();

void applySystem(const Model& m);
void applyWifiList(const Model& m);
void openDetail(uint8_t topic, uint8_t back = P_SETTINGS);  // back: page the arrow returns to

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

// Sub-page back bar (kBarH tall): back arrow left, title centred on the
// screen, optional action right, all on the bar's middle line.  Glyph insets
// inside a 44 px button were measured on screenshots: the arrow's visible edge
// is 15 px from the button's left, the refresh glyph 14 px from its right.
// Be Vietnam Pro keeps room for Vietnamese marks above the caps, so the title
// label sits 13 px above the middle line for its cap height to centre on it.
constexpr int32_t kBarBtn = 44;
constexpr int32_t kBarInset = kSubPad;  // border to the visible arrow / action glyph
constexpr int32_t kBarMidY = kBarH / 2;
constexpr int32_t kBarLeftX = kBarInset - 15;
constexpr int32_t kBarRightX = 480 - kBarInset + 14 - kBarBtn;
constexpr int32_t kBarBtnY = kBarMidY - kBarBtn / 2;
constexpr int32_t kBarTitleX = kBarLeftX + kBarBtn + 4;  // symmetric, clear of both buttons
constexpr int32_t kBarTitleW = 480 - 2 * kBarTitleX;
constexpr int32_t kBarTitleY = kBarMidY - 13;

// Centred one-line bar title; long names end in "…".
void barTitle(lv_obj_t* l) {
    lv_obj_set_pos(l, kBarTitleX, kBarTitleY);
    lv_obj_set_width(l, kBarTitleW);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
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
    lv_obj_t* hdr = box(scr);
    lv_obj_set_size(hdr, 480, kContentY);
    ui.header = hdr;
    lv_obj_t* logo = lv_image_create(hdr);  // BK mark before the room name
    lv_image_set_src(logo, &bk_logo_32);
    lv_obj_set_pos(logo, kMargin, kMargin);  // logo edge 16 px from the border
    ui.name = label(hdr, &hmi_font_16_sb, text(), s_snap.room);
    lv_obj_set_pos(ui.name, kMargin + 42, 12);
    ui.caption = label(hdr, &hmi_font_13_r, muted(), "Đang kết nối…");
    lv_obj_set_pos(ui.caption, kMargin + 42, 34);

    lv_obj_t* right = flexRow(hdr, 12);
    lv_obj_set_height(right, 40);
    lv_obj_align(right, LV_ALIGN_TOP_RIGHT, -kMargin, 12);
    ui.clock = label(right, &hmi_font_16_sb, text(), "--:--");
    lv_obj_add_flag(ui.clock, LV_OBJ_FLAG_HIDDEN);  // until the time is synced
    ui.wifi = label(right, &hmi_icons_20, muted(), ICON_WIFI);
    lv_obj_t* power = flexRow(right, 4);
    label(power, &hmi_icons_20, muted(), ICON_PLUG);
    label(power, &hmi_font_12_sb, muted(), "AC");
}

void selectPage(uint8_t p) {
    if (p >= P_COUNT || p == ui.current) return;
    lv_obj_add_flag(ui.page[ui.current], LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(ui.page[p], LV_OBJ_FLAG_HIDDEN);
    // No fade (here, on the Wi-Fi sheet or the splash).  LVGL 9.2 treats a
    // full-screen object with an opaque background as covering the area even
    // while its `opa` animates, so it skips drawing what is underneath and
    // blends the half-transparent object over whatever the partial draw buffer
    // still held: the band rendered just before.  Seen on the real
    // framebuffer ("shot fb") as the keyboard repeated every 64 px.
    if (p != P_HOME) lv_obj_add_flag(ui.header, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(ui.header, LV_OBJ_FLAG_HIDDEN);
    if (p >= P_WIFI) lv_obj_add_flag(ui.nav, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(ui.nav, LV_OBJ_FLAG_HIDDEN);
    const uint8_t navIndex = p >= P_WIFI ? P_SETTINGS : p;  // sub-pages of Settings
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
    ui.nav = nav;
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

lv_obj_t* makePage(lv_obj_t* scr, uint8_t page) {
    lv_obj_t* p = box(scr);
    lv_obj_set_pos(p, 0, page == P_HOME ? kContentY : page >= P_WIFI ? kSubY : kTabY);
    lv_obj_set_size(p, 480, page == P_HOME ? kContentH : page >= P_WIFI ? kSubH : kTabH);
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
    lv_obj_add_flag(g, LV_OBJ_FLAG_CLICKABLE);  // tap: sensor history (no press tint on the gauge)
    lv_obj_add_event_cb(g, [](lv_event_t*) { openDetail(T_HISTORY, P_HOME); }, LV_EVENT_CLICKED, nullptr);

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
        makeClickable(c);
        lv_obj_add_event_cb(c, [](lv_event_t*) { openDetail(T_HISTORY, P_HOME); }, LV_EVENT_CLICKED, nullptr);
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
    lv_obj_set_width(c, LV_PCT(100));
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
void openDetail(uint8_t topic, uint8_t back);
void openFolder(const char* path);

void openSubDetail(uint8_t topic);

// Row inside a detail page that leads somewhere else; returns its value label.
lv_obj_t* linkRow(lv_obj_t* card, const char* icon, const char* name, bool last, lv_event_cb_t cb) {
    lv_obj_t* row = nullptr;
    lv_obj_t* value = settingsRow(card, icon, name, last, &row);
    addChevron(row);
    rowPressable(row);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, nullptr);
    return value;
}

// Settings row that opens a detail page; returns its value label.
lv_obj_t* detailRow(lv_obj_t* card, const char* icon, const char* name, bool last, uint8_t topic) {
    lv_obj_t* row = nullptr;
    lv_obj_t* value = settingsRow(card, icon, name, last, &row);
    addChevron(row);
    rowPressable(row);
    lv_obj_add_event_cb(row, [](lv_event_t* e) {
        openDetail(static_cast<uint8_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e))));
    }, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<uintptr_t>(topic)));
    return value;
}

void buildSettings(lv_obj_t* page) {
    makeScrollPage(page);

    // Only what is used often is out here; the details sit in the sub-pages
    // (Mạng & MQTT, Thẻ nhớ, Hệ thống).
    // --- about: HCMUT identity + firmware ---
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
    lv_obj_set_flex_grow(col, 1);
    addChevron(about);
    lv_obj_set_style_bg_color(about, pressed(), LV_STATE_PRESSED);
    lv_obj_add_flag(about, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_style(about, &focusRing, LV_STATE_FOCUS_KEY);
    lv_group_add_obj(lv_group_get_default(), about);
    lv_obj_add_event_cb(about, [](lv_event_t*) { openDetail(T_ABOUT); }, LV_EVENT_CLICKED, nullptr);

    // --- connection ---
    sectionTitle(page, "KẾT NỐI");
    lv_obj_t* net = settingsCard(page);
    lv_obj_t* wifiRow = nullptr;
    ui.sysWifi = settingsRow(net, ICON_WIFI, "Wi-Fi", false, &wifiRow);
    addChevron(wifiRow);
    rowPressable(wifiRow);
    lv_obj_add_event_cb(wifiRow, [](lv_event_t*) { openWifiPage(); }, LV_EVENT_CLICKED, nullptr);
    ui.sysIp = detailRow(net, ICON_GLOBE, "Mạng & MQTT", true, T_NET);

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

    // --- data and system ---
    sectionTitle(page, "DỮ LIỆU & HỆ THỐNG");
    lv_obj_t* sys = settingsCard(page);
    ui.sdCard = detailRow(sys, ICON_DRIVE, "Thẻ nhớ", false, T_SD);
    ui.dataHistory = detailRow(sys, ICON_THERMO, "Lịch sử cảm biến", false, T_HISTORY);
    ui.sysMem = detailRow(sys, ICON_MEMORY, "Hệ thống", true, T_MEMORY);
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
    if (!ui.sdFw || ui.fwArmed) return;  // the firmware row only exists on the Thẻ nhớ page
    if (st.firmwareKB) {
        snprintf(buf, sizeof(buf), "firmware.bin · %.1f MB", st.firmwareKB / 1024.0f);
        lv_label_set_text(ui.sdFw, buf);
        lv_obj_set_style_text_color(ui.sdFw, accent(), 0);
    } else {
        lv_label_set_text(ui.sdFw, "Không có tệp");
        lv_obj_set_style_text_color(ui.sdFw, muted(), 0);
    }
}

// ---------- SD card file browser ----------

void parentPath(const char* path, char* out, size_t len) {
    strlcpy(out, path, len);
    char* slash = strrchr(out, '/');
    if (!slash || slash == out) strlcpy(out, "/", len);
    else *slash = 0;
}

const char* baseName(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash && slash[1] ? slash + 1 : path;
}

const char* fileIcon(const char* name) {
    const char* dot = strrchr(name, '.');
    if (!dot) return ICON_FILE_PLAIN;
    if (!strcasecmp(dot, ".bmp") || !strcasecmp(dot, ".png") || !strcasecmp(dot, ".jpg")) return ICON_FILE_IMAGE;
    if (!strcasecmp(dot, ".csv")) return ICON_FILE_SHEET;
    if (!strcasecmp(dot, ".json") || !strcasecmp(dot, ".yaml") || !strcasecmp(dot, ".yml") || !strcasecmp(dot, ".bin"))
        return ICON_FILE_CODE;
    if (!strcasecmp(dot, ".log") || !strcasecmp(dot, ".txt") || !strcasecmp(dot, ".md")) return ICON_FILE;
    return ICON_FILE_PLAIN;
}

void fmtBytes(char* buf, size_t len, uint32_t b) {
    if (b >= 1024u * 1024u) snprintf(buf, len, "%.1f MB", b / (1024.0f * 1024.0f));
    else if (b >= 1024u) snprintf(buf, len, "%.1f KB", b / 1024.0f);
    else snprintf(buf, len, "%lu B", (unsigned long)b);
}

void listMessage(lv_obj_t* list, const char* msg, lv_color_t color) {
    lv_obj_t* r = box(list);
    lv_obj_set_size(r, LV_PCT(100), 56);
    lv_obj_t* l = label(r, &hmi_font_14_m, color, msg);
    lv_obj_center(l);
}

void openFile(const char* path);
void detailNoteIn(lv_obj_t* list, const char* note);
void kv(lv_obj_t* card, const char* key, const char* value, lv_color_t color);
void kv(lv_obj_t* card, const char* key, const char* value);
void closeCard(lv_obj_t* card);

// One title line: "Tệp trên thẻ" at the root, else the path as a breadcrumb
// without the leading slash ("logs", "logs / 2026").
void setFilesTitle(const char* path) {
    if (strcmp(path, "/") == 0) {
        lv_label_set_text(ui.filesTitle, "Tệp trên thẻ");
        return;
    }
    char crumb[128];
    size_t n = 0;
    for (const char* c = path[0] == '/' ? path + 1 : path; *c && n + 4 < sizeof(crumb); c++) {
        if (*c == '/') {
            crumb[n++] = ' ';
            crumb[n++] = '/';
            crumb[n++] = ' ';
        } else {
            crumb[n++] = *c;
        }
    }
    crumb[n] = 0;
    lv_label_set_text(ui.filesTitle, crumb);
}

void openFolder(const char* path) {
    strlcpy(ui.browsePath, path, sizeof(ui.browsePath));
    setFilesTitle(path);
    lv_obj_clean(ui.filesList);
    listMessage(ui.filesList, "Đang đọc thẻ nhớ…", muted());
    if (ui.cb.fileList) ui.cb.fileList(path);
    selectPage(P_FILES);
}

void buildFiles(lv_obj_t* page) {
    lv_obj_t* back = iconButton(page, ICON_BACK, "Lên thư mục cha");
    lv_obj_set_size(back, kBarBtn, kBarBtn);
    lv_obj_set_pos(back, kBarLeftX, kBarBtnY);
    lv_obj_add_event_cb(back, [](lv_event_t*) {
        if (strcmp(ui.browsePath, "/") == 0) {
            openDetail(T_SD);  // the browser is opened from the Thẻ nhớ page
            return;
        }
        char parent[96];
        parentPath(ui.browsePath, parent, sizeof(parent));
        openFolder(parent);
    }, LV_EVENT_CLICKED, nullptr);
    ui.filesTitle = label(page, &hmi_font_16_sb, text(), "Tệp trên thẻ");
    barTitle(ui.filesTitle);

    lv_obj_t* refresh = iconButton(page, ICON_REFRESH, "Tải lại");
    lv_obj_set_size(refresh, kBarBtn, kBarBtn);
    lv_obj_set_pos(refresh, kBarRightX, kBarBtnY);
    lv_obj_add_event_cb(refresh, [](lv_event_t*) {
        if (ui.cb.fileList) ui.cb.fileList(ui.browsePath);
    }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* scroll = box(page);
    lv_obj_set_pos(scroll, kSubPad, kBarH);
    lv_obj_set_size(scroll, kSubW, kSubH - kBarH - kSubPad);
    lv_obj_add_flag(scroll, LV_OBJ_FLAG_CLICKABLE);  // see makeScrollPage()
    lv_obj_add_flag(scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_width(scroll, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(scroll, muted(), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_bottom(scroll, 8, 0);
    ui.filesList = settingsCard(scroll);
}

void applyFiles(const Model& m) {
    const FileList& fl = m.files;
    // A delete re-lists the parent: follow the storage task's path.
    strlcpy(ui.browsePath, fl.path, sizeof(ui.browsePath));
    setFilesTitle(fl.path);

    lv_obj_t* scroll = lv_obj_get_parent(ui.filesList);
    const bool samePath = strcmp(ui.renderedPath, fl.path) == 0;
    const int32_t keepY = samePath ? lv_obj_get_scroll_y(scroll) : 0;
    strlcpy(ui.renderedPath, fl.path, sizeof(ui.renderedPath));
    lv_obj_clean(ui.filesList);

    if (fl.error[0]) {
        listMessage(ui.filesList, fl.error, warning());
    } else if (fl.count == 0) {
        listMessage(ui.filesList, "Thư mục trống", muted());
    }
    for (uint8_t i = 0; i < fl.count; i++) {
        const FileEntry& e = fl.items[i];
        lv_obj_t* r = box(ui.filesList);
        lv_obj_set_size(r, LV_PCT(100), 56);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(r, 12, 0);
        lv_obj_set_style_pad_hor(r, 16, 0);
        if (i + 1 < fl.count || fl.truncated) {
            lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_width(r, 1, 0);
            lv_obj_set_style_border_color(r, elevated(), 0);
        }
        rowPressable(r);
        label(r, &hmi_icons_20, e.dir ? accent() : muted(), e.dir ? ICON_FOLDER : fileIcon(e.name));
        lv_obj_t* name = label(r, &hmi_font_14_m, text(), e.name);
        lv_obj_set_flex_grow(name, 1);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        if (e.dir) {
            addChevron(r);
        } else {
            char size[16];
            fmtBytes(size, sizeof(size), e.size);
            label(r, &hmi_font_13_r, muted(), size);
        }
        lv_obj_add_event_cb(r, [](lv_event_t* ev) {
            const auto idx = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(ev)));
            if (idx >= s_snap.files.count) return;
            const FileEntry& fe = s_snap.files.items[idx];
            char path[96];
            if (strcmp(s_snap.files.path, "/") == 0) snprintf(path, sizeof(path), "/%s", fe.name);
            else snprintf(path, sizeof(path), "%s/%s", s_snap.files.path, fe.name);
            if (fe.dir) openFolder(path);
            else openFile(path);
        }, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
    }
    if (fl.truncated) {
        char msg[48];
        snprintf(msg, sizeof(msg), "Chỉ hiện %u mục đầu tiên", kMaxFiles);
        listMessage(ui.filesList, msg, muted());
    }
    lv_obj_update_layout(scroll);
    lv_obj_scroll_to_y(scroll, keepY, LV_ANIM_OFF);
}

// ----- file view: info, text preview, delete -----

void buildFileView(lv_obj_t* page) {
    lv_obj_t* back = iconButton(page, ICON_BACK, "Quay lại");
    lv_obj_set_size(back, kBarBtn, kBarBtn);
    lv_obj_set_pos(back, kBarLeftX, kBarBtnY);
    lv_obj_add_event_cb(back, [](lv_event_t*) { selectPage(P_FILES); }, LV_EVENT_CLICKED, nullptr);
    ui.viewTitle = label(page, &hmi_font_16_sb, text(), "");
    barTitle(ui.viewTitle);
    ui.viewList = box(page);
    lv_obj_set_pos(ui.viewList, 0, kBarH);
    lv_obj_set_size(ui.viewList, 480, kSubH - kBarH - kSubPad);
    makeScrollPage(ui.viewList);
    lv_obj_set_style_pad_hor(ui.viewList, kSubPad, 0);
}

void openFile(const char* path) {
    strlcpy(ui.viewPath, path, sizeof(ui.viewPath));
    ui.delArmed = false;
    lv_label_set_text(ui.viewTitle, baseName(path));
    lv_obj_clean(ui.viewList);
    lv_obj_scroll_to_y(ui.viewList, 0, LV_ANIM_OFF);  // a new file starts at the top
    detailNoteIn(ui.viewList, "Đang đọc tệp…");
    if (ui.cb.fileRead) ui.cb.fileRead(path);
    selectPage(P_FILEVIEW);
}

bool isCsvPath(const char* path) {
    const char* dot = strrchr(path, '.');
    return dot && strcasecmp(dot, ".csv") == 0;
}

// ----- image preview: decoded by the storage task into fileImage() -----

lv_image_dsc_t s_imgDsc;  // points at fileImage(), rewritten for every image

void imagePreview(lv_obj_t* list, const FileView& v) {
    char a[48];
    snprintf(a, sizeof(a), "ẢNH · %u × %u", v.srcW, v.srcH);
    sectionTitle(list, a);
    lv_image_cache_drop(&s_imgDsc);  // same address, new pixels
    s_imgDsc = lv_image_dsc_t{};
    s_imgDsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_imgDsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_imgDsc.header.w = v.imgW;
    s_imgDsc.header.h = v.imgH;
    s_imgDsc.header.stride = v.imgW * 2;
    s_imgDsc.data = reinterpret_cast<const uint8_t*>(fileImage());
    s_imgDsc.data_size = (uint32_t)v.imgW * v.imgH * 2;
    lv_obj_t* frame = box(list);  // centred on the column, rounded clip
    lv_obj_set_size(frame, LV_PCT(100), v.imgH);
    lv_obj_t* img = lv_image_create(frame);
    lv_image_set_src(img, &s_imgDsc);
    lv_obj_center(img);
    lv_obj_set_style_outline_width(img, 1, 0);  // dark images otherwise melt into the page
    lv_obj_set_style_outline_color(img, elevated(), 0);
}

// ----- CSV as a table -----

constexpr uint16_t kCsvRows = 200;  // newest rows shown
constexpr uint8_t kCsvCols = 8;

// Split one CSV line in place into at most kCsvCols cells ("quoted, values" kept whole).
uint8_t csvSplit(char* line, char** cells) {
    uint8_t n = 0;
    char* p = line;
    while (n < kCsvCols) {
        bool quoted = *p == '"';
        if (quoted) p++;
        cells[n++] = p;
        char* out = p;
        while (*p && (quoted || *p != ',')) {
            if (quoted && *p == '"') {
                if (p[1] == '"') {
                    *out++ = '"';
                    p += 2;
                    continue;
                }
                quoted = false;
                p++;
                continue;
            }
            *out++ = *p++;
        }
        const bool more = *p == ',';
        *out = 0;
        if (!more) break;
        p++;
    }
    return n;
}

// Header row: muted small caps on a raised band; numbers right-aligned.
void csvDrawTask(lv_event_t* e) {
    lv_draw_task_t* t = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t* base = static_cast<lv_draw_dsc_base_t*>(lv_draw_task_get_draw_dsc(t));
    if (base->part != LV_PART_ITEMS) return;
    const uint32_t row = base->id1, col = base->id2;
    if (lv_draw_label_dsc_t* l = lv_draw_task_get_label_dsc(t)) {
        if (row == 0) {
            l->color = muted();
            l->font = &hmi_font_12_sb;
        }
        if (col > 0) l->align = LV_TEXT_ALIGN_RIGHT;
    }
    if (lv_draw_fill_dsc_t* f = lv_draw_task_get_fill_dsc(t)) {
        if (row == 0) {
            f->color = elevated();
            f->opa = LV_OPA_COVER;
        }
    }
}

void csvTable(lv_obj_t* list, bool tail) {
    // Copy what is needed while holding the lock: the header and the last rows.
    constexpr size_t kBuf = 24 * 1024;  // PSRAM, kept for the next table
    static char* buf = static_cast<char*>(heap_caps_malloc(kBuf, MALLOC_CAP_SPIRAM));
    if (!buf) return;
    size_t used = 0;
    const char* lines[kCsvRows + 1];
    uint16_t count = 0;
    uint32_t total = 0;
    {
        ModelLock lock;
        const char* text = fileText();
        const char* starts[kCsvRows + 1];  // ring of the newest kCsvRows line starts
        const char* header = nullptr;
        for (const char* p = text; *p;) {
            const char* e = strchr(p, '\n');
            const size_t len = e ? (size_t)(e - p) : strlen(p);
            if (len) {
                if (!header) header = p;
                else starts[total++ % kCsvRows] = p;
            }
            if (!e) break;
            p = e + 1;
        }
        const uint32_t shown = std::min<uint32_t>(total, kCsvRows);
        auto take = [&](const char* p) -> const char* {
            const char* e = strchr(p, '\n');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            if (len > 160) len = 160;  // one long cell must not eat the buffer
            if (used + len + 1 > kBuf) return nullptr;
            char* dst = buf + used;
            memcpy(dst, p, len);
            dst[len] = 0;
            used += len + 1;
            return dst;
        };
        if (header && (lines[count] = take(header))) count++;
        for (uint32_t i = total - shown; i < total && count <= kCsvRows; i++) {
            if (!(lines[count] = take(starts[i % kCsvRows]))) break;
            count++;
        }
    }
    if (!count) {
        detailNoteIn(list, "(tệp trống)");
        return;
    }

    // Parse: column count from the header, widths from the longest cell.
    char* cells[kCsvCols];
    const uint8_t cols = std::max<uint8_t>(1, csvSplit(const_cast<char*>(lines[0]), cells));
    char a[96];
    snprintf(a, sizeof(a), "BẢNG · %lu DÒNG%s", (unsigned long)total, total > kCsvRows || tail ? " · MỚI NHẤT" : "");
    sectionTitle(list, a);

    lv_obj_t* tbl = lv_table_create(list);
    lv_obj_remove_style_all(tbl);
    lv_table_set_column_count(tbl, cols);
    lv_table_set_row_count(tbl, count);
    uint16_t widthCh[kCsvCols] = {};
    for (uint8_t c = 0; c < cols; c++) {
        lv_table_set_cell_value(tbl, 0, c, cells[c]);
        widthCh[c] = strlen(cells[c]);
    }
    for (uint16_t r = 1; r < count; r++) {
        // newest first: row 1 is the last line of the file
        char* row = const_cast<char*>(lines[count - r]);
        const uint8_t n = csvSplit(row, cells);
        for (uint8_t c = 0; c < cols; c++) {
            const char* v = c < n ? cells[c] : "";
            lv_table_set_cell_value(tbl, r, c, v);
            widthCh[c] = std::max<uint16_t>(widthCh[c], strlen(v));
        }
    }
    int32_t totalW = 0;
    for (uint8_t c = 0; c < cols; c++) {
        // ~9 px per character of the 13 px font plus 2 x 12 px cell padding
        const int32_t w = constrain(widthCh[c] * 9 + 28, 72, 240);
        lv_table_set_column_width(tbl, c, w);
        totalW += w;
    }
    if (totalW < kSubW) lv_table_set_column_width(tbl, cols - 1, kSubW - totalW + lv_table_get_column_width(tbl, cols - 1));

    lv_obj_set_width(tbl, LV_PCT(100));
    lv_obj_set_height(tbl, LV_SIZE_CONTENT);
    lv_obj_set_scroll_dir(tbl, LV_DIR_HOR);  // wide tables scroll sideways, the page scrolls down
    lv_obj_set_scrollbar_mode(tbl, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_bg_opa(tbl, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(tbl, surface(), 0);
    lv_obj_set_style_radius(tbl, 12, 0);
    lv_obj_set_style_clip_corner(tbl, false, 0);
    lv_obj_set_style_text_font(tbl, &hmi_font_13_r, LV_PART_ITEMS);
    lv_obj_set_style_text_color(tbl, text(), LV_PART_ITEMS);
    lv_obj_set_style_pad_hor(tbl, 12, LV_PART_ITEMS);
    lv_obj_set_style_pad_ver(tbl, 8, LV_PART_ITEMS);
    lv_obj_set_style_border_side(tbl, LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS);
    lv_obj_set_style_border_width(tbl, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_color(tbl, elevated(), LV_PART_ITEMS);
    lv_obj_set_style_width(tbl, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(tbl, muted(), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(tbl, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_add_flag(tbl, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(tbl, csvDrawTask, LV_EVENT_DRAW_TASK_ADDED, nullptr);
    if (total > kCsvRows || tail) {
        snprintf(a, sizeof(a), "Hiển thị %u dòng mới nhất (dòng mới ở trên cùng).", count - 1);
        detailNoteIn(list, a);
    }
}

void applyFileView(const Model& m) {
    const FileView& v = m.view;
    if (!v.path[0] && !v.error[0]) {  // cleared: the file was deleted
        if (ui.current == P_FILEVIEW) selectPage(P_FILES);
        return;
    }
    if (strcmp(v.path, ui.viewPath) != 0) return;  // answer for another file
    lv_obj_t* list = ui.viewList;
    lv_obj_clean(list);
    char a[64];

    sectionTitle(list, "THÔNG TIN");
    lv_obj_t* c = settingsCard(list);
    kv(c, "Đường dẫn", v.path);
    fmtBytes(a, sizeof(a), v.size);
    kv(c, "Kích thước", a);
    if (v.error[0]) kv(c, "Lỗi", v.error, warning());
    closeCard(c);

    if (v.image) {
        imagePreview(list, v);
    } else if (v.previewable && isCsvPath(v.path)) {
        csvTable(list, v.tail);
    } else if (v.previewable) {
        sectionTitle(list, v.tail ? "NỘI DUNG · 64 KB CUỐI" : "NỘI DUNG");
        lv_obj_t* pc = settingsCard(list);
        lv_obj_set_style_pad_all(pc, 16, 0);
        lv_obj_set_flex_flow(pc, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(pc, 2, 0);
        ModelLock lock;  // fileText() is shared with the storage task
        const char* p = fileText();
        if (!*p) label(pc, &hmi_font_13_r, muted(), "(tệp trống)");
        char chunk[1100];
        while (*p) {
            // up to ~1 KB, cut after a newline when there is one
            size_t n = strnlen(p, 1024);
            if (p[n]) {
                size_t k = n;
                while (k > 1 && p[k - 1] != '\n') k--;
                if (k > 1) n = k;
            }
            memcpy(chunk, p, n);
            chunk[n] = 0;
            if (n && chunk[n - 1] == '\n') chunk[n - 1] = 0;  // the gap between labels is the line break
            lv_obj_t* t = label(pc, &hmi_font_13_r, text(), chunk);
            lv_obj_set_width(t, LV_PCT(100));
            lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_line_space(t, 2, 0);
            p += n;
        }
    } else if (!v.error[0]) {
        detailNoteIn(list, "Không xem trước được loại tệp này (chỉ xem tệp văn bản: .txt .log .csv .json …).");
    }

    // Delete: first tap arms for 4 s, second tap deletes.
    lv_obj_t* dc = settingsCard(list);
    lv_obj_t* row = box(dc);
    lv_obj_set_size(row, LV_PCT(100), 52);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_style_pad_hor(row, 16, 0);
    rowPressable(row);
    label(row, &hmi_icons_20, error(), ICON_TRASH);
    lv_obj_t* dl = label(row, &hmi_font_14_m, error(), "Xoá tệp");
    lv_obj_add_event_cb(row, [](lv_event_t* e) {
        lv_obj_t* l = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
        if (!ui.delArmed) {
            ui.delArmed = true;
            lv_label_set_text(l, "Chạm lần nữa để xoá");
            lv_timer_t* tm = lv_timer_create([](lv_timer_t* t) {
                ui.delArmed = false;
                lv_obj_t* lbl = static_cast<lv_obj_t*>(lv_timer_get_user_data(t));
                if (lv_obj_is_valid(lbl)) lv_label_set_text(lbl, "Xoá tệp");
            }, 4000, l);
            lv_timer_set_repeat_count(tm, 1);
            return;
        }
        ui.delArmed = false;
        lv_label_set_text(l, "Đang xoá…");
        if (ui.cb.fileDelete) ui.cb.fileDelete(ui.viewPath);
    }, LV_EVENT_CLICKED, dl);
}

// ---------- detail pages ----------

void buildDetail(lv_obj_t* page) {
    lv_obj_t* back = iconButton(page, ICON_BACK, "Quay lại");
    lv_obj_set_size(back, kBarBtn, kBarBtn);
    lv_obj_set_pos(back, kBarLeftX, kBarBtnY);
    lv_obj_add_event_cb(back, [](lv_event_t*) {
        if (ui.detailParent != T_COUNT) {  // e.g. config.json -> back to Thẻ nhớ
            const uint8_t parent = ui.detailParent;
            openDetail(parent, ui.detailBack);
        } else {
            selectPage(ui.detailBack);
        }
    }, LV_EVENT_CLICKED, nullptr);
    ui.detailTitle = label(page, &hmi_font_16_sb, text(), "");
    barTitle(ui.detailTitle);

    ui.detailList = box(page);
    lv_obj_set_pos(ui.detailList, 0, kBarH);
    lv_obj_set_size(ui.detailList, 480, kSubH - kBarH - kSubPad);
    makeScrollPage(ui.detailList);
    lv_obj_set_style_pad_hor(ui.detailList, kSubPad, 0);
}

void detailFmtSize(char* buf, size_t len, uint32_t kb) {
    if (kb >= 1024 * 1024) snprintf(buf, len, "%.1f GB", kb / (1024.0f * 1024.0f));
    else if (kb >= 1024) snprintf(buf, len, "%.1f MB", kb / 1024.0f);
    else snprintf(buf, len, "%lu KB", (unsigned long)kb);
}

// Key on the left, value right-aligned (wraps for long values).
void kv(lv_obj_t* card, const char* key, const char* value, lv_color_t color) {
    lv_obj_t* r = box(card);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(r, 48, 0);
    lv_obj_set_style_pad_hor(r, 16, 0);
    lv_obj_set_style_pad_ver(r, 12, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 16, 0);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_border_color(r, elevated(), 0);
    label(r, &hmi_font_14_m, text(), key);
    lv_obj_t* v = label(r, &hmi_font_14_m, color, value);
    lv_obj_set_flex_grow(v, 1);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
}
void kv(lv_obj_t* card, const char* key, const char* value) { kv(card, key, value, muted()); }

// "used / total" with a usage bar (amber above 85 %).
struct UsageRow {
    lv_obj_t* value = nullptr;
    lv_obj_t* bar = nullptr;
};

void usageText(char* line, size_t len, uint32_t usedKB, uint32_t totalKB, uint32_t* pctOut) {
    char u[16], t[16];
    detailFmtSize(u, sizeof(u), usedKB);
    detailFmtSize(t, sizeof(t), totalKB);
    const uint32_t pct = totalKB ? (uint64_t)usedKB * 100 / totalKB : 0;
    snprintf(line, len, "%s / %s · %lu%%", u, t, (unsigned long)pct);
    if (pctOut) *pctOut = pct;
}

UsageRow usageRow(lv_obj_t* card, const char* key, uint32_t usedKB, uint32_t totalKB) {
    char line[48];
    uint32_t pct = 0;
    usageText(line, sizeof(line), usedKB, totalKB, &pct);
    kv(card, key, line);
    lv_obj_t* row = lv_obj_get_child(card, -1);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(row, 10, 0);
    lv_obj_t* bar = lv_bar_create(row);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), 6);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, elevated(), LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, pct > 85 ? warning() : accent(), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
    lv_obj_set_style_anim_duration(bar, 300, 0);  // live updates slide
    return UsageRow{lv_obj_get_child(row, 1), bar};
}

// Live memory page: widgets refreshed in place by liveMemory().
struct MemMetric {
    lv_obj_t* value = nullptr;   // big "88"
    lv_obj_t* used = nullptr;    // "288 / 324 KB"
    lv_chart_series_t* ser = nullptr;
    lv_obj_t* marker = nullptr;  // dot on the newest sample
};
struct MemoryRefs {
    MemMetric ram, psram;
    lv_obj_t* chart = nullptr;   // both metrics share one trace
    lv_obj_t* stamp = nullptr;
} s_mem;

constexpr uint32_t kLiveMs = 250;        // 4 updates per second
constexpr uint16_t kChartPoints = 60;    // 15 s of history

lv_obj_t* detailCard(const char* title) {
    if (title) sectionTitle(ui.detailList, title);
    lv_obj_t* c = settingsCard(ui.detailList);
    return c;
}

void detailNoteIn(lv_obj_t* list, const char* note) {
    lv_obj_t* l = label(list, &hmi_font_13_r, muted(), note);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_hor(l, 4, 0);
}
void detailNote(const char* note) { detailNoteIn(ui.detailList, note); }

void closeCard(lv_obj_t* card) {  // no divider under the last row
    lv_obj_t* last = lv_obj_get_child(card, -1);
    if (last) lv_obj_set_style_border_width(last, 0, 0);
}

const char* mqttStateText(int8_t s) {
    switch (s) {
        case -4: return "Hết thời gian phản hồi";
        case -3: return "Mất kết nối";
        case -2: return "Không tới được broker";
        case -1: return "Chưa kết nối";
        case 0: return "Đã kết nối";
        case 1: return "Sai phiên bản giao thức";
        case 2: return "Client ID bị từ chối";
        case 3: return "Broker không sẵn sàng";
        case 4: return "Sai tài khoản / mật khẩu";
        case 5: return "Không có quyền";
        default: return "Không rõ";
    }
}

const char* resetText(uint8_t r) {
    switch (r) {
        case 1: return "Bật nguồn / nút EN / nạp firmware";
        case 2: return "Chân reset ngoài";
        case 3: return "Khởi động lại bằng phần mềm";
        case 4: return "Lỗi phần mềm (panic)";
        case 5: return "Watchdog ngắt";
        case 6: return "Watchdog task (treo > 10 s)";
        case 7: return "Watchdog khác";
        case 8: return "Thức dậy từ deep sleep";
        case 9: return "Sụt áp nguồn (brownout)";
        default: return "Không rõ";
    }
}

void fmtUptime(char* buf, size_t len, uint32_t s) {
    const uint32_t d = s / 86400, h = s / 3600 % 24, m = s / 60 % 60;
    if (d) snprintf(buf, len, "%lu ngày %lu giờ %lu phút", (unsigned long)d, (unsigned long)h, (unsigned long)m);
    else if (h) snprintf(buf, len, "%lu giờ %lu phút", (unsigned long)h, (unsigned long)m);
    else snprintf(buf, len, "%lu phút %lu giây", (unsigned long)m, (unsigned long)(s % 60));
}

// ----- live memory instrument -----

constexpr int32_t kTraceW = kSubW - 40;   // plot width (value axis labels sit to its right)
constexpr int32_t kTraceH = 112;
constexpr int32_t kTraceTop = 6;   // headroom so the newest-sample dot is never clipped

void memUsedText(char* buf, size_t len, uint32_t usedKB, uint32_t totalKB) {
    char u[16], t[16];
    detailFmtSize(u, sizeof(u), usedKB);
    detailFmtSize(t, sizeof(t), totalKB);
    snprintf(buf, len, "%s / %s", u, t);
}

lv_obj_t* metricHeader(lv_obj_t* parent, const char* title, lv_color_t color, lv_obj_t** rightOut) {
    lv_obj_t* head = box(parent);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 8, 0);
    lv_obj_t* d = box(head);
    lv_obj_set_size(d, 8, 8);
    lv_obj_set_style_radius(d, 4, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(d, color, 0);
    lv_obj_t* t = label(head, &hmi_font_12_sb, muted(), title);
    lv_obj_set_style_text_letter_space(t, 1, 0);
    lv_obj_set_flex_grow(t, 1);
    *rightOut = label(head, &hmi_font_13_r, muted(), "");
    return head;
}

lv_obj_t* bigPercent(lv_obj_t* parent, uint32_t pct, lv_color_t numColor) {
    lv_obj_t* row = flexRow(parent, 4, LV_FLEX_ALIGN_START);
    char v[8];
    snprintf(v, sizeof(v), "%lu", (unsigned long)pct);
    lv_obj_t* value = label(row, &hmi_font_64_sb, numColor, v);
    lv_obj_t* unit = label(row, &hmi_font_20_m, muted(), "%");
    lv_obj_set_style_pad_top(unit, 14, 0);
    return value;
}

// One headline metric: coloured title (doubles as the chart legend), used/total
// and a big percentage.
MemMetric memMetric(lv_obj_t* parent, const char* title, lv_color_t color, uint32_t usedKB, uint32_t totalKB) {
    MemMetric mm;
    const uint32_t pct = totalKB ? (uint64_t)usedKB * 100 / totalKB : 0;
    lv_obj_t* col = flexCol(parent, 2);
    lv_obj_set_flex_grow(col, 1);
    metricHeader(col, title, color, &mm.used);
    char u[40];
    memUsedText(u, sizeof(u), usedKB, totalKB);
    lv_label_set_text(mm.used, u);
    mm.value = bigPercent(col, pct, pct > 90 ? warning() : text());
    return mm;
}

lv_obj_t* traceMarker(lv_obj_t* parent, lv_color_t color) {
    lv_obj_t* m = box(parent);  // filled dot with a background ring
    lv_obj_set_size(m, 12, 12);
    lv_obj_set_style_radius(m, 6, 0);
    lv_obj_set_style_bg_opa(m, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(m, color, 0);
    lv_obj_set_style_border_width(m, 3, 0);
    lv_obj_set_style_border_color(m, bg(), 0);
    lv_obj_add_flag(m, LV_OBJ_FLAG_HIDDEN);
    return m;
}

// RAM + PSRAM: two headline metrics side by side, then one shared 15 s trace
// with a value axis (100/75/50/25/0 on the grid lines) and a time axis.
void memPanel(lv_obj_t* parent, uint32_t ramUsed, uint32_t ramTotal, uint32_t psUsed, uint32_t psTotal) {
    lv_obj_t* col = flexCol(parent, 8);
    lv_obj_set_width(col, LV_PCT(100));
    lv_obj_set_style_pad_top(col, 10, 0);
    lv_obj_set_style_pad_bottom(col, 10, 0);
    lv_obj_set_style_border_side(col, LV_BORDER_SIDE_TOP, 0);  // hairline between modules
    lv_obj_set_style_border_width(col, 1, 0);
    lv_obj_set_style_border_color(col, elevated(), 0);

    lv_obj_t* heads = box(col);
    lv_obj_set_size(heads, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(heads, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(heads, 24, 0);
    s_mem.ram = memMetric(heads, "RAM NỘI", accent(), ramUsed, ramTotal);
    s_mem.psram = memMetric(heads, "PSRAM", accent2(), psUsed, psTotal);

    lv_obj_t* area = box(col);
    lv_obj_set_size(area, LV_PCT(100), kTraceTop + kTraceH + 24);
    lv_obj_t* chart = lv_chart_create(area);
    s_mem.chart = chart;
    lv_obj_remove_style_all(chart);
    lv_obj_set_pos(chart, 0, kTraceTop);
    lv_obj_set_size(chart, kTraceW, kTraceH);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, kChartPoints);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_div_line_count(chart, 5, 0);  // lines at 0/25/50/75/100 %
    lv_obj_set_style_line_color(chart, elevated(), LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 1, LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_width(chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(chart, 0, LV_PART_INDICATOR);
    s_mem.ram.ser = lv_chart_add_series(chart, accent(), LV_CHART_AXIS_PRIMARY_Y);
    s_mem.psram.ser = lv_chart_add_series(chart, accent2(), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(chart, s_mem.ram.ser, LV_CHART_POINT_NONE);
    lv_chart_set_all_value(chart, s_mem.psram.ser, LV_CHART_POINT_NONE);

    static const char* const kTicks[] = {"100", "75", "50", "25", "0"};
    for (int i = 0; i < 5; i++) {
        lv_obj_t* l = label(area, &hmi_font_12_sb, muted(), kTicks[i]);
        lv_obj_set_width(l, kSubW - kTraceW - 6);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(l, kTraceW + 6, kTraceTop + kTraceH * i / 4 - 8);
    }
    lv_obj_t* from = label(area, &hmi_font_12_sb, muted(), "15 GIÂY TRƯỚC");
    lv_obj_set_style_text_letter_space(from, 1, 0);
    lv_obj_set_pos(from, 0, kTraceTop + kTraceH + 6);
    lv_obj_t* now = label(area, &hmi_font_12_sb, text(), "BÂY GIỜ");
    lv_obj_set_style_text_letter_space(now, 1, 0);
    lv_obj_set_width(now, 120);
    lv_obj_set_style_text_align(now, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(now, kTraceW - 120, kTraceTop + kTraceH + 6);

    s_mem.ram.marker = traceMarker(area, accent());
    s_mem.psram.marker = traceMarker(area, accent2());
}

// Flash: firmware vs OTA slot as a segmented bar.
void flashModule(lv_obj_t* parent, uint32_t appKB, uint32_t slotKB, uint32_t flashMB) {
    const uint32_t pct = slotKB ? (uint64_t)appKB * 100 / slotKB : 0;
    lv_obj_t* col = flexCol(parent, 8);
    lv_obj_set_width(col, LV_PCT(100));
    lv_obj_set_style_pad_top(col, 10, 0);
    lv_obj_set_style_border_side(col, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(col, 1, 0);
    lv_obj_set_style_border_color(col, elevated(), 0);
    lv_obj_t* right = nullptr;
    metricHeader(col, "FLASH · FIRMWARE", lamp(), &right);
    char a[48];
    snprintf(a, sizeof(a), "Chip %lu MB", (unsigned long)flashMB);
    lv_label_set_text(right, a);

    lv_obj_t* row = flexRow(col, 10, LV_FLEX_ALIGN_END);
    char v[8];
    snprintf(v, sizeof(v), "%lu", (unsigned long)pct);
    label(row, &hmi_font_28_sb, text(), v);
    lv_obj_t* unit = label(row, &hmi_font_14_m, muted(), "%");
    lv_obj_set_style_pad_bottom(unit, hmi_font_28_sb.base_line - hmi_font_14_m.base_line, 0);
    char appS[16], slotS[16];
    detailFmtSize(appS, sizeof(appS), appKB);
    detailFmtSize(slotS, sizeof(slotS), slotKB);
    snprintf(a, sizeof(a), "%s / khe OTA %s", appS, slotS);
    lv_obj_t* note = label(row, &hmi_font_13_r, muted(), a);
    lv_obj_set_style_pad_bottom(note, hmi_font_28_sb.base_line - hmi_font_13_r.base_line, 0);

    constexpr int kSegments = 24;
    lv_obj_t* bar = box(col);
    lv_obj_set_size(bar, LV_PCT(100), 10);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 3, 0);
    const int filled = (pct * kSegments + 50) / 100;
    for (int i = 0; i < kSegments; i++) {
        lv_obj_t* seg = box(bar);
        lv_obj_set_height(seg, 10);
        lv_obj_set_flex_grow(seg, 1);
        lv_obj_set_style_radius(seg, 2, 0);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(seg, i < filled ? lamp() : elevated(), 0);
    }
}

// ---------- data pages (PSRAM history, event log) ----------

void applyDetail(const Model& m);

constexpr int32_t kHistW = kSubW - 48;  // plot; value axis (up to "1234.5") on the right
constexpr uint16_t kHistPoints = 96;    // 4 px per point
constexpr int32_t kHistH = 64;

void fmtAxis(char* buf, size_t len, float v) { snprintf(buf, len, fabsf(v) >= 100 ? "%.0f" : "%.1f", v); }

// One metric: coloured title + latest value, then its trace with min/max on
// the value axis.  Values are plotted x10 (lv_chart is integer).
void historyMetric(uint8_t metric, const char* title, const char* unit, lv_color_t color, bool timeAxis) {
    static float pts[kHistPoints];
    datalog::query(ui.histRange, metric, pts, kHistPoints);
    float lo = INFINITY, hi = -INFINITY, last = NAN;
    uint16_t valid = 0;
    for (float v : pts) {
        if (isnan(v)) continue;
        lo = fminf(lo, v);
        hi = fmaxf(hi, v);
        last = v;
        valid++;
    }
    const bool any = !isnan(last);

    lv_obj_t* col = flexCol(ui.detailList, 6);
    lv_obj_set_width(col, LV_PCT(100));
    lv_obj_set_style_pad_top(col, 10, 0);
    lv_obj_set_style_pad_bottom(col, 6, 0);
    lv_obj_set_style_border_side(col, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(col, 1, 0);
    lv_obj_set_style_border_color(col, elevated(), 0);

    lv_obj_t* head = box(col);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_t* name = flexRow(head, 8);
    lv_obj_t* dot = box(name);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, 4, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, color, 0);
    lv_obj_t* t = label(name, &hmi_font_12_sb, muted(), title);
    lv_obj_set_style_text_letter_space(t, 1, 0);
    char a[48];
    if (any) snprintf(a, sizeof(a), "%.1f %s", last, unit);
    else strlcpy(a, "–", sizeof(a));
    lv_obj_t* v = label(head, &hmi_font_16_sb, any ? text() : muted(), a);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t* area = box(col);
    lv_obj_set_size(area, LV_PCT(100), kHistH + (timeAxis ? 24 : 4));
    lv_obj_t* chart = lv_chart_create(area);
    lv_obj_remove_style_all(chart);
    lv_obj_set_pos(chart, 0, 2);
    lv_obj_set_size(chart, kHistW, kHistH);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, kHistPoints);
    lv_chart_set_div_line_count(chart, 3, 0);  // max, middle, min
    lv_obj_set_style_line_color(chart, elevated(), LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 1, LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    // Lines need two neighbouring points: with very little data show dots.
    const int32_t mark = valid <= 3 ? 6 : 0;
    lv_obj_set_style_width(chart, mark, LV_PART_INDICATOR);
    lv_obj_set_style_height(chart, mark, LV_PART_INDICATOR);
    lv_obj_set_style_radius(chart, mark / 2, LV_PART_INDICATOR);
    float pad = any ? fmaxf((hi - lo) * 0.15f, 0.5f) : 1.0f;
    if (!any) lo = hi = 0;
    const float yMin = lo - pad, yMax = hi + pad;
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, lroundf(yMin * 10), lroundf(yMax * 10));
    lv_chart_series_t* ser = lv_chart_add_series(chart, color, LV_CHART_AXIS_PRIMARY_Y);
    for (uint16_t i = 0; i < kHistPoints; i++)
        lv_chart_set_value_by_id(chart, ser, i, isnan(pts[i]) ? LV_CHART_POINT_NONE : lroundf(pts[i] * 10));

    // value axis: the data's max and min, on the top and bottom grid lines
    for (int i = 0; i < 2; i++) {
        if (any) fmtAxis(a, sizeof(a), i == 0 ? hi : lo);
        else strlcpy(a, "–", sizeof(a));
        lv_obj_t* l = label(area, &hmi_font_12_sb, muted(), a);
        lv_obj_set_width(l, kSubW - kHistW - 4);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
        const int32_t y = 2 + lroundf((yMax - (i == 0 ? hi : lo)) / (yMax - yMin) * (kHistH - 1));
        lv_obj_set_pos(l, kHistW + 4, y - 8);
    }
    if (timeAxis) {
        static const char* const kFrom[] = {"1 GIỜ TRƯỚC", "24 GIỜ TRƯỚC", "7 NGÀY TRƯỚC"};
        lv_obj_t* from = label(area, &hmi_font_12_sb, muted(), kFrom[ui.histRange]);
        lv_obj_set_style_text_letter_space(from, 1, 0);
        lv_obj_set_pos(from, 0, kHistH + 8);
        lv_obj_t* now = label(area, &hmi_font_12_sb, text(), "BÂY GIỜ");
        lv_obj_set_style_text_letter_space(now, 1, 0);
        lv_obj_set_width(now, 120);
        lv_obj_set_style_text_align(now, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(now, kHistW - 120, kHistH + 8);
    }
}

void historyPage() {
    // range selector: 1 h / 24 h / 7 d
    lv_obj_t* seg = flexRow(ui.detailList, 6);
    lv_obj_set_width(seg, LV_PCT(100));
    static const char* const kNames[] = {"1 giờ", "24 giờ", "7 ngày"};
    for (uint8_t r = 0; r < datalog::R_COUNT; r++) {
        lv_obj_t* b = box(seg);
        lv_obj_set_height(b, 40);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        const bool on = r == ui.histRange;
        lv_obj_set_style_bg_color(b, on ? tint(accent()) : surface(), 0);
        makeClickable(b);
        lv_obj_t* l = label(b, &hmi_font_14_m, on ? accent() : muted(), kNames[r]);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, [](lv_event_t* e) {
            ui.histRange = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
            applyDetail(s_snap);
        }, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<uintptr_t>(r)));
    }
    historyMetric(0, HMI_PRIMARY_LABEL, HMI_PRIMARY_UNIT, accent(), false);
    historyMetric(1, "ĐỘ ẨM", HMI_METRIC1_UNIT, info(), false);
    historyMetric(2, "CÔNG SUẤT", HMI_METRIC2_UNIT, accent2(), true);
    detailNote(datalog::fineFilled() ? "Trung bình mỗi 10 giây (24 giờ) và mỗi phút (7 ngày), giữ trong PSRAM. "
                                       "Lưu vào thẻ nhớ 10 phút/lần và flash 1 giờ/lần."
                                     : "Chưa có dữ liệu: lịch sử bắt đầu ghi khi cảm biến gửi giá trị.");
    ui.histShownAt = lv_tick_get();
}

void eventsPage() {
    constexpr uint16_t kShown = 100;
    const uint16_t total = datalog::eventCount();
    lv_obj_t* c = settingsCard(ui.detailList);
    lv_obj_set_style_pad_hor(c, 16, 0);
    lv_obj_set_style_pad_ver(c, 4, 0);
    char line[datalog::kEventLen];
    uint16_t i = 0;
    for (; i < kShown && datalog::eventAt(i, line, sizeof(line)); i++) {
        // "YYYY-MM-DD HH:MM:SS message" or "+123s message"
        char* msg = strchr(line, ' ');
        if (msg && line[0] != '+') msg = strchr(msg + 1, ' ');
        const char* when = line;
        if (msg) *msg++ = 0;
        else msg = line, when = "";
        lv_obj_t* row = flexCol(c, 2);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_style_pad_ver(row, 10, 0);
        if (i) {
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_TOP, 0);
            lv_obj_set_style_border_width(row, 1, 0);
            lv_obj_set_style_border_color(row, elevated(), 0);
        }
        label(row, &hmi_font_12_sb, muted(), when[0] == '2' && strlen(when) > 5 ? when + 5 : when);  // drop the year
        lv_obj_t* m = label(row, &hmi_font_13_r, text(), msg);
        lv_obj_set_width(m, LV_PCT(100));
        lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
    }
    if (!i) label(c, &hmi_font_13_r, muted(), "Chưa có sự kiện");
    closeCard(c);
    char a[120];
    snprintf(a, sizeof(a), "%u sự kiện mới nhất (giữ %u trong PSRAM). Toàn bộ nằm trong /logs/events.log trên thẻ nhớ.",
             i, total);
    detailNote(a);
    ui.evShown = datalog::eventSerial();
}

// Uptime, clock and reset reason (part of the Hệ thống page).
void uptimeCards(const Model& m) {
    const SystemInfo& s = m.sys;
    char a[64], b[64];
    lv_obj_t* c = detailCard("THỜI GIAN");
    fmtUptime(a, sizeof(a), s.uptimeS);
    kv(c, "Đã chạy", a, text());
    if (s.timeSynced) snprintf(b, sizeof(b), "%s · %s", m.clock, s.date);
    kv(c, "Giờ hệ thống", s.timeSynced ? b : "Chưa đồng bộ (cần Wi-Fi)", s.timeSynced ? muted() : warning());
    kv(c, "Múi giờ", "GMT+7");
    snprintf(a, sizeof(a), "%s (%u)", resetText(s.resetReason), s.resetReason);
    kv(c, "Lần khởi động này", a, (s.resetReason >= 4 && s.resetReason <= 7) || s.resetReason == 9 ? warning() : muted());
    closeCard(c);
}

void applyDetail(const Model& m) {
    // Rebuilt on every refresh: keep the reader where they were.
    const int32_t keepY = lv_obj_get_scroll_y(ui.detailList);
    ui.sdFw = nullptr;  // lives in the list that is about to be deleted
    lv_obj_clean(ui.detailList);
    s_mem = MemoryRefs{};
    const SystemInfo& s = m.sys;
    const StorageInfo& st = m.storage;
    const bool linked = m.link == Link::Online || m.link == Link::BrokerDown;
    char a[64], b[64];
    lv_obj_t* c;

    switch (ui.detailTopic) {
        case T_NET:
            c = detailCard("WI-FI");
            kv(c, "Trạng thái", linked ? "Đã kết nối" : (m.link == Link::Connecting ? "Đang kết nối…" : "Mất kết nối"),
               linked ? success() : warning());
            if (linked) {
                kv(c, "Tên mạng (SSID)", s.ssid);
                snprintf(a, sizeof(a), "%d dBm · %s", s.rssi, s.rssi >= -60 ? "Tốt" : s.rssi >= -72 ? "Trung bình" : "Yếu");
                kv(c, "Cường độ sóng", a, s.rssi >= -72 ? muted() : warning());
                kv(c, "Điểm truy cập (BSSID)", s.bssid);
                snprintf(a, sizeof(a), "%u · 2.4 GHz", s.channel);
                kv(c, "Kênh", a);
            }
            closeCard(c);
            c = detailCard("ĐỊA CHỈ");
            kv(c, "Địa chỉ IP", s.ip[0] ? s.ip : "–", s.ip[0] ? text() : muted());
            if (linked) {
                kv(c, "Gateway", s.gateway);
                kv(c, "Subnet mask", s.subnet);
                kv(c, "DNS", s.dns);
            }
            kv(c, "MAC", s.mac);
            kv(c, "Hostname", s.hostname);
            closeCard(c);
            detailNote("Đổi mạng ở mục Wi-Fi trong Cài đặt. Khi mất kết nối, board thử lại các mạng đã biết, cách nhau 5–60 giây.");
            [[fallthrough]];  // MQTT on the same page

        case T_MQTT:
            c = detailCard("MQTT");
            snprintf(a, sizeof(a), "%s (%d)", mqttStateText(s.mqttState), s.mqttState);
            kv(c, "Trạng thái", a, s.mqttState == 0 ? success() : (linked ? warning() : muted()));
            snprintf(a, sizeof(a), "%s:%u", s.mqttHost, s.mqttPort);
            kv(c, "Broker", a);
            kv(c, "Đăng nhập", s.mqttAuth ? "Có tài khoản" : "Ẩn danh");
            kv(c, "Client ID", MQTT_CLIENT_ID "-xxxx");
            kv(c, "Keep-alive", "60 giây");
            kv(c, "Thử lại", "5–60 giây, timeout TCP 1 giây");
            closeCard(c);
            c = detailCard("TOPIC");
            kv(c, "Gốc", MQTT_TOPIC_BASE);
            kv(c, "Trạng thái", "/state, /availability");
            kv(c, "Lệnh", "/command");
            kv(c, "Cảm biến", "/sensor");
            closeCard(c);
            detailNote("Đổi broker bằng mục \"mqtt\" trong /config.json trên thẻ nhớ, hoặc MQTT_BROKER trong config.h.");
            break;

        case T_MEMORY: {
            // Data-first instrument layout: big value, then a 15 s trace with a
            // value axis; no cards, hairline grid, one colour per metric.
            lv_obj_t* live = flexRow(ui.detailList, 6);
            lv_obj_set_style_pad_left(live, 4, 0);
            lv_obj_set_style_pad_top(live, 6, 0);
            lv_obj_t* dot = box(live);
            lv_obj_set_size(dot, 8, 8);
            lv_obj_set_style_radius(dot, 4, 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(dot, success(), 0);
            lv_obj_t* lt = label(live, &hmi_font_12_sb, success(), "TRỰC TIẾP");
            lv_obj_set_style_text_letter_space(lt, 1, 0);
            s_mem.stamp = label(live, &hmi_font_12_sb, muted(), "· 4 lần/giây");

            memPanel(ui.detailList, s.heapTotalKB - s.heapKB, s.heapTotalKB, s.psramTotalKB - s.psramKB, s.psramTotalKB);
            flashModule(ui.detailList, s.appKB, s.appSlotKB ? s.appSlotKB : s.appKB, s.flashMB);
            {
                size_t lvIn, lvPs;
                lvmem_stats(&lvIn, &lvPs);
                c = detailCard("PSRAM ĐANG DÙNG CHO");
                fmtBytes(a, sizeof(a), DISPLAY_WIDTH * DISPLAY_HEIGHT * 2);
                kv(c, "Framebuffer màn hình", a);
                fmtBytes(a, sizeof(a), datalog::bytes());
                kv(c, "Lịch sử cảm biến + nhật ký", a);
                fmtBytes(a, sizeof(a), lvPs);
                kv(c, "Giao diện (LVGL)", a);
                fmtBytes(a, sizeof(a), 2 * 64 * 1024);
                kv(c, "Trình xem tệp", a);
                closeCard(c);
                c = detailCard("FLASH · LITTLEFS");
                if (st.lfsTotalKB) usageRow(c, "Đã dùng", st.lfsUsedKB, st.lfsTotalKB);
                else kv(c, "Trạng thái", "Chưa gắn", warning());
                kv(c, "Lưu", "config.json, lịch sử (mỗi giờ)");
                closeCard(c);
            }
            uptimeCards(m);
            c = settingsCard(ui.detailList);
            snprintf(a, sizeof(a), "%u", datalog::eventCount());
            lv_label_set_text(linkRow(c, ICON_FILE_PLAIN, "Nhật ký sự kiện", true,
                                      [](lv_event_t*) { openSubDetail(T_EVENTS); }), a);
            break;
        }

        case T_UPTIME:
            uptimeCards(m);
            break;

        case T_SD:
            c = detailCard("THẺ NHỚ");
            kv(c, "Trạng thái", st.present ? "Đã gắn" : "Không có thẻ", st.present ? success() : warning());
            if (st.present) {
                kv(c, "Loại thẻ", st.cardType);
                snprintf(a, sizeof(a), "SPI %lu MHz", (unsigned long)(st.spiKHz / 1000));
                kv(c, "Giao tiếp", a);
                usageRow(c, "Đã dùng", st.usedMB * 1024, st.totalMB * 1024);
            }
            closeCard(c);
            c = settingsCard(ui.detailList);
            if (st.present)
                lv_label_set_text(linkRow(c, ICON_FOLDER_TREE, "Tệp trên thẻ", false, [](lv_event_t*) { openFolder("/"); }), "");
            {
                lv_obj_t* v = linkRow(c, ICON_FILE, "config.json", !st.present, [](lv_event_t*) { openSubDetail(T_CONFIG); });
                lv_label_set_text(v, st.configSource == 1 ? "Từ thẻ" : st.configSource == 2 ? "Từ flash" : "Không có");
                lv_obj_set_style_text_color(v, st.configSource ? success() : muted(), 0);
            }
            if (st.present) {
                // Two taps: the first arms (4 s), the second starts flashing.
                lv_obj_t* fwRow = nullptr;
                ui.sdFw = settingsRow(c, ICON_DOWNLOAD, "Cập nhật firmware", true, &fwRow);
                rowPressable(fwRow);
                lv_obj_add_event_cb(fwRow, [](lv_event_t*) {
                    if (!s_snap.storage.firmwareKB || !ui.sdFw) return;
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
                applyStorage(m);
            }
            closeCard(c);
            if (st.present) {
                c = detailCard("TỆP TRÊN THẺ");
                kv(c, "Nhật ký", "/logs/events.log");
                snprintf(b, sizeof(b), "/logs/sensors/<ngày>.csv · giữ %u ngày", st.logDays);
                kv(c, "Cảm biến", b);
                kv(c, "Lịch sử", "/data/history.bin · 10 phút/lần");
                kv(c, "Ảnh chụp", "/screenshots/");
                closeCard(c);
                c = detailCard("SAO LƯU FIRMWARE");
                if (st.backupKB) {
                    snprintf(a, sizeof(a), "v%s · %.1f MB", st.backupVer, st.backupKB / 1024.0f);
                    kv(c, "Bản trước", a, text());
                } else {
                    kv(c, "Bản trước", "Chưa có");
                }
                closeCard(c);
                detailNote("Trước mỗi lần cập nhật từ thẻ, firmware đang chạy được chép vào /backup. "
                           "Cài lại bản trước bằng lệnh serial \"sd restore\".");
            }
            detailNote("Thống kê dung lượng cập nhật 10 phút một lần và khi cắm/rút thẻ.");
            break;

        case T_CONFIG:
            c = detailCard("/config.json");
            kv(c, "Trạng thái",
               st.configSource == 1 ? "Đã nạp từ thẻ nhớ"
               : st.configSource == 2 ? "Đã nạp từ bản sao trong flash"
               : (st.present ? "Không có tệp" : "Không có thẻ"),
               st.configLoaded ? success() : muted());
            if (st.configLoaded) {
                kv(c, "Tên phòng", st.cfgRoom[0] ? st.cfgRoom : "(mặc định)");
                snprintf(a, sizeof(a), "%u mạng", st.cfgWifi);
                kv(c, "Wi-Fi", st.cfgWifi ? a : "(không có)");
                kv(c, "MQTT broker", st.cfgMqtt[0] ? st.cfgMqtt : "(mặc định)");
                snprintf(a, sizeof(a), "%u thiết bị", st.cfgTasmota);
                kv(c, "Tasmota", st.cfgTasmota ? a : "(dùng config.h)");
            }
            closeCard(c);
            detailNote("Sửa /config.json trên thẻ nhớ rồi khởi động lại để áp dụng. Mẫu: docs/06-the-nho.md.");
            break;

        case T_HISTORY: historyPage(); break;
        case T_EVENTS: eventsPage(); break;

        case T_ABOUT:
        default: {
            lv_obj_t* head = settingsCard(ui.detailList);
            lv_obj_set_style_pad_all(head, 16, 0);
            lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(head, 14, 0);
            lv_obj_t* mark = lv_image_create(head);
            lv_image_set_src(mark, &bk_logo_48);
            lv_obj_t* col = flexCol(head, 2);
            label(col, &hmi_font_12_sb, muted(), "ĐẠI HỌC QUỐC GIA TP.HCM");
            label(col, &hmi_font_14_m, text(), "TRƯỜNG ĐẠI HỌC BÁCH KHOA");
            c = detailCard("THIẾT BỊ");
            kv(c, "Sản phẩm", "Smart Home Panel");
            kv(c, "Firmware", "v" FIRMWARE_VERSION, text());
            kv(c, "Ngày build", __DATE__ " " __TIME__);
            kv(c, "Phần cứng", "GUITION ESP32-4848S040");
            kv(c, "Màn hình", "4\" 480×480 · ST7701 · GT911");
            closeCard(c);
            c = detailCard("CHIP");
            snprintf(a, sizeof(a), "%s rev %u", s.chip, s.chipRev);
            kv(c, "Vi xử lý", a);
            snprintf(a, sizeof(a), "%u nhân · %u MHz", s.cores, s.cpuMHz);
            kv(c, "CPU", a);
            kv(c, "MAC", s.mac);
            kv(c, "ESP-IDF", s.sdk);
            snprintf(a, sizeof(a), "%d.%d.%d", lv_version_major(), lv_version_minor(), lv_version_patch());
            kv(c, "LVGL", a);
            closeCard(c);
            break;
        }
    }
    lv_obj_update_layout(ui.detailList);
    lv_obj_scroll_to_y(ui.detailList, keepY, LV_ANIM_OFF);
}

// While the Memory page is open, read the heap directly (heap_caps_* is safe
// from any task) every 250 ms, update the existing widgets and push one point
// into the 15 s graph: no rebuild, so no flicker and the scroll position stays.
void updateMetric(const MemMetric& mm, uint32_t usedKB, uint32_t totalKB) {
    const uint32_t pct = totalKB ? (uint64_t)usedKB * 100 / totalKB : 0;
    char v[8];
    snprintf(v, sizeof(v), "%lu", (unsigned long)pct);
    lv_label_set_text(mm.value, v);
    lv_obj_set_style_text_color(mm.value, pct > 90 ? warning() : text(), 0);
    char u[40];
    memUsedText(u, sizeof(u), usedKB, totalKB);
    lv_label_set_text(mm.used, u);
    lv_chart_set_next_value(s_mem.chart, mm.ser, pct);
    lv_point_t pt;
    lv_chart_get_point_pos_by_id(s_mem.chart, mm.ser, kChartPoints - 1, &pt);  // newest = right edge
    lv_obj_set_pos(mm.marker, pt.x - 6, kTraceTop + pt.y - 6);
    lv_obj_remove_flag(mm.marker, LV_OBJ_FLAG_HIDDEN);
}

void liveMemory() {
    static uint32_t last = 0;
    if (ui.current != P_DETAIL || ui.detailTopic != T_MEMORY || !s_mem.ram.value) return;
    const uint32_t now = lv_tick_get();
    if (now - last < kLiveMs) return;
    last = now;
    const uint32_t heapTotal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / 1024;
    const uint32_t heapFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
    const uint32_t psTotal = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024;
    const uint32_t psFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024;
    updateMetric(s_mem.ram, heapTotal - heapFree, heapTotal);
    updateMetric(s_mem.psram, psTotal - psFree, psTotal);
    char st[40];
    snprintf(st, sizeof(st), "· 4 lần/giây · %lu.%lus", (unsigned long)(now / 1000), (unsigned long)(now % 1000 / 100));
    lv_label_set_text(s_mem.stamp, st);
}

// History re-renders every 10 s, the event list when a new event arrives,
// the Settings "DỮ LIỆU" values every 2 s while Settings is visible.
void liveData() {
    static uint32_t last = 0;
    const uint32_t now = lv_tick_get();
    if (now - last < 1000) return;
    last = now;
    if (ui.current == P_DETAIL && ui.detailTopic == T_EVENTS && datalog::eventSerial() != ui.evShown) applyDetail(s_snap);
    if (ui.current == P_DETAIL && ui.detailTopic == T_HISTORY && now - ui.histShownAt >= 10000) applyDetail(s_snap);
    if (ui.current == P_SETTINGS && ui.dataHistory) {
        char a[40];
        const uint32_t sec = datalog::fineFilled() * datalog::kFineSec;
        if (!sec) strlcpy(a, "Chưa có", sizeof(a));
        else if (sec < 3600) snprintf(a, sizeof(a), "%lu phút", (unsigned long)(sec / 60));
        else snprintf(a, sizeof(a), "%lu giờ", (unsigned long)(sec / 3600));
        lv_label_set_text(ui.dataHistory, a);

    }
}

void showTopic(uint8_t topic) {
    static const char* const titles[] = {"Mạng & MQTT", "Mạng & MQTT", "Hệ thống", "Thời gian chạy", "Thẻ nhớ",
                                         "config.json", "Giới thiệu", "Lịch sử cảm biến", "Nhật ký sự kiện"};
    ui.detailTopic = topic;
    lv_label_set_text(ui.detailTitle, titles[topic]);
    applyDetail(s_snap);
    lv_obj_scroll_to_y(ui.detailList, 0, LV_ANIM_OFF);
    selectPage(P_DETAIL);
}

void openDetail(uint8_t topic, uint8_t back) {
    ui.detailBack = back;
    ui.detailParent = T_COUNT;
    showTopic(topic);
}

// From inside a detail page: its back arrow returns to the current page.
void openSubDetail(uint8_t topic) {
    ui.detailParent = ui.detailTopic;
    showTopic(topic);
}

// ---------- Wi-Fi page ----------

void openWifiPage() {
    selectPage(P_WIFI);
    if (ui.cb.wifiScan) ui.cb.wifiScan();
}

void openSheet(const WifiNetwork& n);

void buildWifi(lv_obj_t* page) {
    lv_obj_t* back = iconButton(page, ICON_BACK, "Quay lại");
    lv_obj_set_size(back, kBarBtn, kBarBtn);
    lv_obj_set_pos(back, kBarLeftX, kBarBtnY);
    lv_obj_add_event_cb(back, [](lv_event_t*) { selectPage(P_SETTINGS); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* title = label(page, &hmi_font_16_sb, text(), "Wi-Fi");
    barTitle(title);

    ui.wifiRefresh = iconButton(page, ICON_REFRESH, "Quét lại");
    lv_obj_set_size(ui.wifiRefresh, kBarBtn, kBarBtn);
    lv_obj_set_pos(ui.wifiRefresh, kBarRightX, kBarBtnY);
    lv_obj_add_event_cb(ui.wifiRefresh, [](lv_event_t*) {
        if (ui.cb.wifiScan) ui.cb.wifiScan();
    }, LV_EVENT_CLICKED, nullptr);
    ui.wifiSpinner = lv_spinner_create(page);
    lv_obj_remove_style_all(ui.wifiSpinner);
    lv_obj_set_size(ui.wifiSpinner, 22, 22);
    lv_obj_set_pos(ui.wifiSpinner, kBarRightX + (kBarBtn - 22) / 2, kBarBtnY + (kBarBtn - 22) / 2);
    lv_spinner_set_anim_params(ui.wifiSpinner, 900, 90);
    lv_obj_set_style_arc_opa(ui.wifiSpinner, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui.wifiSpinner, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ui.wifiSpinner, accent(), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(ui.wifiSpinner, true, LV_PART_INDICATOR);
    lv_obj_add_flag(ui.wifiSpinner, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* scroll = box(page);
    lv_obj_set_pos(scroll, kSubPad, kBarH);
    lv_obj_set_size(scroll, kSubW, kSubH - kBarH - kSubPad);
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
    lv_obj_t* wifiScroll = lv_obj_get_parent(ui.wifiList);
    const int32_t keepY = lv_obj_get_scroll_y(wifiScroll);
    struct Restore {  // runs on every return path below
        lv_obj_t* o;
        int32_t y;
        ~Restore() {
            lv_obj_update_layout(o);
            lv_obj_scroll_to_y(o, y, LV_ANIM_OFF);
        }
    } restore{wifiScroll, keepY};
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
    lv_keyboard_set_mode(ui.kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_remove_flag(ui.sheet, h);  // no fade: see selectPage()
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

// ---------- password keyboard ----------
//
// Phone-style layout: a digit row on top (Wi-Fi passwords are full of digits),
// 5 rows x ~48 px keys.  Mode keys keep LVGL's built-in labels ("ABC", "abc",
// "1#", LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK) so lv_keyboard handles them.
// lv_buttonmatrix_ctrl_t is an enum: C++ needs the OR-ed flags cast back.
#define KB_CTRL(v) static_cast<lv_buttonmatrix_ctrl_t>(v)
#define KB_W(w) KB_CTRL(w)                                                     // plain key (space)
#define KB_KEY(w) KB_CTRL(LV_BUTTONMATRIX_CTRL_POPOVER | (w))                  // letter: shows a popover
#define KB_MOD(w) KB_CTRL(LV_BUTTONMATRIX_CTRL_CHECKED | LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | (w))
#define KB_DEL KB_CTRL(LV_BUTTONMATRIX_CTRL_CHECKED | 6)                       // backspace repeats when held

static const char* const kKbLower[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
    "ABC", "z", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n",
    "1#", "-", " ", ".", LV_SYMBOL_OK, ""};
static const char* const kKbUpper[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
    "abc", "Z", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
    "1#", "_", " ", ".", LV_SYMBOL_OK, ""};
static const lv_buttonmatrix_ctrl_t kKbLetterCtrl[] = {
    KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4),
    KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4),
    KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4),
    KB_MOD(6), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_DEL,
    KB_MOD(6), KB_KEY(3), KB_W(14), KB_KEY(3), KB_MOD(6)};
static const char* const kKbSpecial[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "!", "@", "#", "$", "%", "^", "&", "*", "(", ")", "\n",
    "-", "_", "=", "+", "[", "]", "{", "}", ";", ":", "\n",
    "abc", "'", "\"", "/", "\\", "?", "<", ">", LV_SYMBOL_BACKSPACE, "\n",
    "ABC", ",", " ", ".", LV_SYMBOL_OK, ""};
static const lv_buttonmatrix_ctrl_t kKbSpecialCtrl[] = {
    KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4),
    KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4),
    KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4),
    KB_MOD(6), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_DEL,
    KB_MOD(6), KB_KEY(3), KB_W(14), KB_KEY(3), KB_MOD(6)};

// Be Vietnam Pro for the keys, Montserrat only for the ⌫ / ✓ symbols it lacks.
lv_font_t s_kbFont;

// Per-key drawing tweaks:
//  - the OK key uses the palette accent with dark text;
//  - Be Vietnam Pro reserves room above the cap height for stacked Vietnamese
//    marks, so single unaccented key labels (q, 1, ABC) sit ~2 px below centre.
//    Nudge them up.  LV_SYMBOL_* glyphs (U+F0xx, UTF-8 lead byte 0xEF) come from
//    Montserrat, which has no such headroom, and stay where LVGL puts them.
constexpr int32_t kKeyLabelLift = 2;

void kbDrawTask(lv_event_t* e) {
    lv_draw_task_t* t = lv_event_get_draw_task(e);
    auto* base = static_cast<lv_draw_dsc_base_t*>(lv_draw_task_get_draw_dsc(t));
    if (!base || base->part != LV_PART_ITEMS) return;
    const char* txt = lv_buttonmatrix_get_button_text(ui.kb, base->id1);
    if (!txt) return;
    const bool ok = strcmp(txt, LV_SYMBOL_OK) == 0;
    if (lv_draw_task_get_type(t) == LV_DRAW_TASK_TYPE_FILL) {
        if (ok) lv_draw_task_get_fill_dsc(t)->color = accent();
    } else if (lv_draw_task_get_type(t) == LV_DRAW_TASK_TYPE_LABEL) {
        lv_draw_label_dsc_t* l = lv_draw_task_get_label_dsc(t);
        if (ok) l->color = bg();
        if (static_cast<uint8_t>(txt[0]) != 0xEF) l->ofs_y = -kKeyLabelLift;
    }
}

void buildKeyboard(lv_obj_t* parent) {
    s_kbFont = hmi_font_16_sb;
    s_kbFont.fallback = &lv_font_montserrat_16;

    ui.kb = lv_keyboard_create(parent);
    // lv_keyboard_create() aligns to BOTTOM_MID; re-anchor so the position is absolute.
    lv_obj_align(ui.kb, LV_ALIGN_TOP_LEFT, 0, 188);
    lv_obj_set_size(ui.kb, 480, 292);
    lv_keyboard_set_map(ui.kb, LV_KEYBOARD_MODE_TEXT_LOWER, kKbLower, kKbLetterCtrl);
    lv_keyboard_set_map(ui.kb, LV_KEYBOARD_MODE_TEXT_UPPER, kKbUpper, kKbLetterCtrl);
    lv_keyboard_set_map(ui.kb, LV_KEYBOARD_MODE_SPECIAL, kKbSpecial, kKbSpecialCtrl);
    lv_keyboard_set_mode(ui.kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_popovers(ui.kb, true);

    lv_obj_set_style_bg_color(ui.kb, bg(), 0);
    lv_obj_set_style_bg_opa(ui.kb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui.kb, 0, 0);
    lv_obj_set_style_radius(ui.kb, 0, 0);
    lv_obj_set_style_pad_hor(ui.kb, 6, 0);
    lv_obj_set_style_pad_top(ui.kb, 6, 0);
    lv_obj_set_style_pad_bottom(ui.kb, 10, 0);
    lv_obj_set_style_pad_row(ui.kb, 7, 0);
    lv_obj_set_style_pad_column(ui.kb, 5, 0);

    // Letter keys: lifted grey; modifier keys (CHECKED): darker; pressed: lighter.
    lv_obj_set_style_bg_opa(ui.kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(ui.kb, lv_color_hex(0x262626), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(ui.kb, lv_color_hex(0x161616), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(ui.kb, lv_color_hex(0x4A4A4A), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(ui.kb, text(), LV_PART_ITEMS);
    lv_obj_set_style_text_color(ui.kb, text(), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_font(ui.kb, &s_kbFont, LV_PART_ITEMS);
    lv_obj_set_style_radius(ui.kb, 8, LV_PART_ITEMS);
    lv_obj_set_style_border_width(ui.kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(ui.kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_outline_width(ui.kb, 0, LV_PART_ITEMS | LV_STATE_FOCUS_KEY);

    lv_obj_add_flag(ui.kb, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(ui.kb, kbDrawTask, LV_EVENT_DRAW_TASK_ADDED, nullptr);
    lv_keyboard_set_textarea(ui.kb, ui.pass);
    lv_obj_add_event_cb(ui.kb, [](lv_event_t*) { submitJoin(); }, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(ui.kb, [](lv_event_t*) { closeSheet(); }, LV_EVENT_CANCEL, nullptr);
}

void buildSheet(lv_obj_t* scr) {
    ui.sheet = box(scr);
    lv_obj_set_size(ui.sheet, 480, 480);
    lv_obj_set_style_bg_opa(ui.sheet, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(ui.sheet, bg(), 0);
    lv_obj_add_flag(ui.sheet, LV_OBJ_FLAG_CLICKABLE);  // swallow taps meant for the page below
    lv_obj_add_flag(ui.sheet, LV_OBJ_FLAG_HIDDEN);

    // Same back bar as the sub-pages; two centred lines: title, network name.
    lv_obj_t* back = iconButton(ui.sheet, ICON_BACK, "Đóng");
    lv_obj_set_size(back, kBarBtn, kBarBtn);
    lv_obj_set_pos(back, kBarLeftX, kBarBtnY);
    lv_obj_add_event_cb(back, [](lv_event_t*) { closeSheet(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* t = label(ui.sheet, &hmi_font_16_sb, text(), "Kết nối Wi-Fi");
    barTitle(t);
    lv_obj_set_y(t, kBarTitleY - 9);
    ui.sheetSsid = label(ui.sheet, &hmi_font_13_r, muted(), "");
    barTitle(ui.sheetSsid);
    lv_obj_set_y(ui.sheetSsid, kBarTitleY + 13);

    ui.pass = lv_textarea_create(ui.sheet);
    lv_obj_remove_style_all(ui.pass);
    lv_obj_set_pos(ui.pass, kMargin, 72);
    lv_obj_set_size(ui.pass, 448, 52);
    lv_obj_set_style_bg_opa(ui.pass, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(ui.pass, surface(), 0);
    lv_obj_set_style_radius(ui.pass, 12, 0);
    lv_obj_set_style_pad_left(ui.pass, 16, 0);
    lv_obj_set_style_pad_right(ui.pass, 56, 0);
    // 52 px box, 1 px focus border, 24 px line.  The content box must be exactly
    // one line tall (52 - 2 - 11 - 15 = 24): any smaller and the one-line
    // textarea scrolls the text out of view.  11/15 instead of 13/13 lifts the
    // text 2 px, because Be Vietnam Pro keeps headroom above the caps for
    // stacked Vietnamese marks (same lift as the keyboard keys).
    lv_obj_set_style_pad_top(ui.pass, 11, 0);
    lv_obj_set_style_pad_bottom(ui.pass, 15, 0);
    lv_obj_set_style_border_width(ui.pass, 1, 0);  // same box in every state
    lv_obj_set_style_border_color(ui.pass, surface(), 0);
    lv_obj_set_scroll_dir(ui.pass, LV_DIR_HOR);
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

    buildKeyboard(ui.sheet);
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
    // Mạng & MQTT: the address when all is well, else what is wrong.
    if (m.link == Link::Online) {
        lv_label_set_text(ui.sysIp, s.ip[0] ? s.ip : "–");
        lv_obj_set_style_text_color(ui.sysIp, muted(), 0);
    } else {
        lv_label_set_text(ui.sysIp, m.link == Link::BrokerDown ? "MQTT lỗi" : "Chờ Wi-Fi");
        lv_obj_set_style_text_color(ui.sysIp, m.link == Link::BrokerDown ? warning() : muted(), 0);
    }
    // Hệ thống: internal RAM use and uptime.
    const uint32_t ramPct = s.heapTotalKB ? (s.heapTotalKB - s.heapKB) * 100 / s.heapTotalKB : 0;
    const uint32_t min = s.uptimeS / 60;
    if (min >= 60) snprintf(buf, sizeof(buf), "RAM %lu%% · %lu giờ", (unsigned long)ramPct, (unsigned long)(min / 60));
    else snprintf(buf, sizeof(buf), "RAM %lu%% · %lu phút", (unsigned long)ramPct, (unsigned long)min);
    lv_label_set_text(ui.sysMem, buf);
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

    lv_obj_delete_delayed(s, 2000);  // no fade: see selectPage()
}

// Build (or rebuild after a palette change) every widget for the current palette.
void buildAll() {
    const Callbacks cb = ui.cb;
    const uint8_t page = ui.current;
    const bool dimmed = ui.dimmed;
    // A palette change rebuilds every widget: keep the reader where they were.
    const int32_t keepY = ui.page[page] ? lv_obj_get_scroll_y(ui.page[page]) : 0;
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

    for (uint8_t p = 0; p < P_COUNT; p++) ui.page[p] = makePage(scr, p);
    buildHome(ui.page[P_HOME]);
    buildControl(ui.page[P_CONTROL]);
    buildSettings(ui.page[P_SETTINGS]);
    buildWifi(ui.page[P_WIFI]);
    buildDetail(ui.page[P_DETAIL]);
    buildFiles(ui.page[P_FILES]);
    buildFileView(ui.page[P_FILEVIEW]);
    for (uint8_t p = P_CONTROL; p < P_COUNT; p++) lv_obj_add_flag(ui.page[p], LV_OBJ_FLAG_HIDDEN);
    buildHeader(scr);
    buildNav(scr);
    buildSheet(scr);
    buildUpdateOverlay(scr);
    selectPage(page);
    if (keepY) {
        lv_obj_update_layout(ui.page[page]);  // the content height must be known to scroll
        lv_obj_scroll_to_y(ui.page[page], keepY, LV_ANIM_OFF);
    }
    static bool splashShown = false;
    if (!splashShown) {
        splashShown = true;
        buildSplash(scr);
    }
}

// ---------- UI task ----------

namespace {

enum ReqType : uint8_t { REQ_PAGE, REQ_SNAPSHOT, REQ_SCROLL, REQ_KEYBOARD, REQ_DETAIL, REQ_BENCH, REQ_FILE };

struct Request {
    ReqType type;
    uint8_t page;
    lv_draw_buf_t** result;   // REQ_SNAPSHOT: filled by the UI task
    SemaphoreHandle_t done;   // REQ_SNAPSHOT: given when result is ready
};

QueueHandle_t s_requests = nullptr;
char s_reqPath[96];  // REQ_FILE argument (console only, one at a time)
LGFX* s_lcd = nullptr;

// Console "bench [psram|hybrid]": optionally switch the LVGL allocator policy,
// rebuild every page (so all widgets are reallocated under it), then time
// full-screen redraws of the current page, flush to the panel included.
void bench(uint8_t policy) {
    if (policy) lvmem_set_policy(policy == 1 ? LVMEM_PSRAM : LVMEM_HYBRID);
    const uint32_t t0 = millis();
    buildAll();
    lv_refr_now(nullptr);
    const uint32_t buildMs = millis() - t0;
    constexpr int kFrames = 20;
    uint32_t flushUs, flushes;
    port::takeFlushStats(&flushUs, &flushes);
    const uint32_t t1 = micros();
    for (int i = 0; i < kFrames; i++) {
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(nullptr);
        esp_task_wdt_reset();
    }
    const uint32_t frameUs = (micros() - t1) / kFrames;
    port::takeFlushStats(&flushUs, &flushes);
    Serial.printf("bench: per frame %lu us = render %lu us + flush %lu us (%lu chunks)\n",
                  (unsigned long)frameUs, (unsigned long)(frameUs - flushUs / kFrames),
                  (unsigned long)(flushUs / kFrames), (unsigned long)(flushes / kFrames));
    size_t inLv, exLv;
    lvmem_stats(&inLv, &exLv);
    Serial.printf("bench: alloc %s | build %lu ms | full redraw %lu.%lu ms (%lu fps) | LVGL %u KB internal, %u KB PSRAM | heap free %u KB\n",
                  lvmem_policy() == LVMEM_PSRAM ? "psram" : "hybrid", (unsigned long)buildMs,
                  (unsigned long)(frameUs / 1000), (unsigned long)(frameUs % 1000 / 100),
                  (unsigned long)(1000000 / frameUs), (unsigned)(inLv / 1024), (unsigned)(exLv / 1024),
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
}

void serveRequests() {
    Request r;
    while (xQueueReceive(s_requests, &r, 0) == pdTRUE) {
        if (r.type == REQ_PAGE) {
            selectPage(r.page);
        } else if (r.type == REQ_DETAIL) {
            if (r.page < T_COUNT) openDetail(r.page);
        } else if (r.type == REQ_KEYBOARD) {
            WifiNetwork n{"Wi-Fi thử nghiệm", -58, true};
            openSheet(n);
        } else if (r.type == REQ_FILE) {
            openFile(s_reqPath);
        } else if (r.type == REQ_BENCH) {
            bench(r.page);
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
        if (d) {
            model().dirty = 0;
            s_snap = model();
        }
    }
    if (d & D_THEME) {
        buildAll();
        d = D_ALL & ~D_THEME;  // fresh widgets need every value
    }
    const Model& m = s_snap;
    if (d & D_PRIMARY) applyPrimary(m);
    if (d & D_METRICS) applyMetrics(m);
    if (d & D_LINK) applyLink(m);
    if (d & D_CLOCK) {
        lv_label_set_text(ui.clock, m.clock);
        const bool synced = m.clock[0] != '-';  // "--:--" until NTP answers
        if (synced) lv_obj_remove_flag(ui.clock, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(ui.clock, LV_OBJ_FLAG_HIDDEN);
    }
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
    // Memory, history and events refresh themselves (liveMemory / liveData).
    const bool selfRefresh = ui.detailTopic == T_MEMORY || ui.detailTopic == T_HISTORY || ui.detailTopic == T_EVENTS;
    if ((d & (D_SYSTEM | D_STORAGE | D_LINK | D_CLOCK)) && ui.current == P_DETAIL && !selfRefresh) applyDetail(m);
    liveMemory();
    liveData();
    if (d & D_FILES) applyFiles(m);
    if (d & D_FILEVIEW) applyFileView(m);

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

void showDetail(uint8_t topic) {
    Request r{REQ_DETAIL, topic, nullptr, nullptr};
    if (s_requests) xQueueSend(s_requests, &r, 0);
}

void previewFile(const char* path) {
    strlcpy(s_reqPath, path, sizeof(s_reqPath));
    Request r{REQ_FILE, 0, nullptr, nullptr};
    if (s_requests) xQueueSend(s_requests, &r, 0);
}

void benchmark(uint8_t policy) {
    Request r{REQ_BENCH, policy, nullptr, nullptr};
    if (s_requests) xQueueSend(s_requests, &r, 0);
}

void previewKeyboard() {
    Request r{REQ_KEYBOARD, 0, nullptr, nullptr};
    if (s_requests) xQueueSend(s_requests, &r, 0);
}

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

namespace {
// `data`: RGB565 rows, native-endian (LVGL) or already byte-swapped (panel).
void streamRle(Print& out, const uint8_t* data, uint32_t w, uint32_t h, uint32_t stride, bool swapped) {
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
        const auto* row = reinterpret_cast<const uint16_t*>(data + y * stride);
        for (uint32_t x = 0; x < w; x++) {
            const uint16_t c = swapped ? row[x] : static_cast<uint16_t>((row[x] >> 8) | (row[x] << 8));
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
}
}  // namespace

void screenshot(Print& out) {
    lv_draw_buf_t* snap = takeSnapshot();
    if (!snap) {
        out.println("SHOT failed: no memory");
        return;
    }
    streamRle(out, snap->data, snap->header.w, snap->header.h, snap->header.stride, false);
    lv_draw_buf_destroy(snap);
}

// What the panel really shows (tearing, half-drawn frames included), taken
// `delayMs` after the call: copy the framebuffer at once, stream it after.
void screenshotFramebuffer(Print& out, uint32_t delayMs) {
    const size_t bytes = DISPLAY_WIDTH * DISPLAY_HEIGHT * 2;
    uint8_t* copy = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
    if (!copy) {
        out.println("SHOT failed: no memory");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(delayMs));
    port::copyFramebuffer(copy);
    streamRle(out, copy, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_WIDTH * 2, true);
    heap_caps_free(copy);
}

}  // namespace hmi
