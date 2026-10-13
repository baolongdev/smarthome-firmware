#include "hmi_port.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include "config.h"
#include "lgfx_4848s040_reference.h"

namespace hmi::port {
namespace {

constexpr int kBufLines = 48;  // 480x48 RGB565 = 45 KB per buffer, internal DMA RAM

LGFX* s_lcd = nullptr;
void (*s_onWake)() = nullptr;
bool s_guard = false;
bool s_swallowing = false;

uint32_t tickCb() { return millis(); }

void flushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px) {
    const int32_t w = lv_area_get_width(area);
    const int32_t h = lv_area_get_height(area);
    // LVGL RGB565 is native-endian, which is lgfx::rgb565_t.
    s_lcd->pushImage(area->x1, area->y1, w, h, reinterpret_cast<const lgfx::rgb565_t*>(px));
    lv_display_flush_ready(disp);
}

void touchCb(lv_indev_t*, lv_indev_data_t* data) {
    uint16_t x = 0, y = 0;
    const bool down = s_lcd->getTouch(&x, &y) > 0;

    if (s_guard && down) {  // first touch after dimming only wakes
        s_guard = false;
        s_swallowing = true;
    }
    if (s_swallowing) {
        data->state = LV_INDEV_STATE_RELEASED;
        if (!down) {
            s_swallowing = false;
            if (s_onWake) s_onWake();
        }
        return;
    }
    static bool wasDown = false;
    if (down != wasDown) {
        if (down) Serial.printf("TOUCH DOWN X=%u Y=%u\n", x, y);
        else Serial.println("TOUCH UP");
        wasDown = down;
    }
    if (down) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

#if defined(HMI_BTN_PREV_PIN) && defined(HMI_BTN_NEXT_PIN) && defined(HMI_BTN_OK_PIN)
void keypadCb(lv_indev_t*, lv_indev_data_t* data) {
    static uint32_t last = 0;
    uint32_t key = 0;
    if (!digitalRead(HMI_BTN_PREV_PIN)) key = LV_KEY_PREV;
    else if (!digitalRead(HMI_BTN_NEXT_PIN)) key = LV_KEY_NEXT;
    else if (!digitalRead(HMI_BTN_OK_PIN)) key = LV_KEY_ENTER;
    data->state = key ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    if (key) last = key;
    data->key = last;
}
#endif

}  // namespace

void begin(LGFX& lcd) {
    s_lcd = &lcd;
    lv_init();
    lv_tick_set_cb(tickCb);

    lv_display_t* disp = lv_display_create(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    const size_t bytes = DISPLAY_WIDTH * kBufLines * sizeof(uint16_t);
    void* buf1 = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    void* buf2 = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!buf1 || !buf2) {
        // Fall back to PSRAM: slower, but the panel still works.
        Serial.println("HMI: internal draw buffers unavailable, using PSRAM");
        buf1 = buf1 ? buf1 : heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
        buf2 = buf2 ? buf2 : heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    }
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, buf1, buf2, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flushCb);

    lv_indev_t* touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, touchCb);

    lv_group_set_default(lv_group_create());
#if defined(HMI_BTN_PREV_PIN) && defined(HMI_BTN_NEXT_PIN) && defined(HMI_BTN_OK_PIN)
    pinMode(HMI_BTN_PREV_PIN, INPUT_PULLUP);
    pinMode(HMI_BTN_NEXT_PIN, INPUT_PULLUP);
    pinMode(HMI_BTN_OK_PIN, INPUT_PULLUP);
    lv_indev_t* keys = lv_indev_create();
    lv_indev_set_type(keys, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(keys, keypadCb);
    lv_indev_set_group(keys, lv_group_get_default());
#endif
}

void armWakeGuard(void (*onWake)()) {
    s_onWake = onWake;
    s_guard = true;
}

}  // namespace hmi::port
