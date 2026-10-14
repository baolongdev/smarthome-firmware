#include "hmi_port.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include "config.h"
#include "lgfx_4848s040_reference.h"

namespace hmi::port {
namespace {

// One 480x64 RGB565 render buffer (60 KB, internal RAM).  The flush is a
// synchronous CPU copy, so a second buffer never overlapped anything; one
// taller buffer halves the render passes per frame for the same RAM.
constexpr int kBufLines = 64;

LGFX* s_lcd = nullptr;
void (*s_onWake)() = nullptr;
bool s_guard = false;
bool s_swallowing = false;

struct SynthDrag {
    int16_t x, y0, y1;
    uint16_t ms;
    uint32_t start;
    volatile bool active;
} s_drag{};

uint32_t tickCb() { return millis(); }

uint32_t s_flushUs = 0;
uint32_t s_flushCount = 0;

void flushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px) {
    const uint32_t t0 = micros();
    const int32_t w = lv_area_get_width(area);
    const int32_t h = lv_area_get_height(area);
    // The RGB panel framebuffer holds byte-swapped RGB565 (lgfx::swap565_t).
    // Pushing LVGL's native-endian pixels made LovyanGFX convert them one by
    // one (~42 ms per full frame); swapping the chunk in place first lets it
    // memcpy each row into the framebuffer instead (~25 ms per full frame,
    // bound by PSRAM bandwidth shared with the LCD DMA: writing the rows
    // directly into the framebuffer measured the same).
    lv_draw_sw_rgb565_swap(px, w * h);
    s_lcd->pushImage(area->x1, area->y1, w, h, reinterpret_cast<const lgfx::swap565_t*>(px));
    s_flushUs += micros() - t0;
    s_flushCount++;
    lv_display_flush_ready(disp);
}

void touchCb(lv_indev_t*, lv_indev_data_t* data) {
    if (s_drag.active) {
        const uint32_t t = millis() - s_drag.start;
        if (t > s_drag.ms + 100u) {  // hold briefly at the end, then lift
            s_drag.active = false;
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }
        const uint32_t k = t > s_drag.ms ? s_drag.ms : t;
        data->point.x = s_drag.x;
        data->point.y = s_drag.y0 + (int32_t)(s_drag.y1 - s_drag.y0) * (int32_t)k / (int32_t)s_drag.ms;
        data->state = LV_INDEV_STATE_PRESSED;
        lv_display_trigger_activity(nullptr);
        return;
    }
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
    void* buf = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        // Fall back to PSRAM: slower, but the panel still works.
        Serial.println("HMI: internal draw buffer unavailable, using PSRAM");
        buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    }
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, buf, nullptr, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
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

void injectDrag(int16_t x, int16_t y0, int16_t y1, uint16_t ms) {
    s_drag.x = x;
    s_drag.y0 = y0;
    s_drag.y1 = y1;
    s_drag.ms = ms ? ms : 1;
    s_drag.start = millis();
    s_drag.active = true;  // set last: the UI task polls this flag
}

void armWakeGuard(void (*onWake)()) {
    s_onWake = onWake;
    s_guard = true;
}

void copyFramebuffer(uint8_t* dst) {
    s_lcd->readRect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, reinterpret_cast<lgfx::swap565_t*>(dst));
}

void takeFlushStats(uint32_t* us, uint32_t* count) {
    *us = s_flushUs;
    *count = s_flushCount;
    s_flushUs = s_flushCount = 0;
}

}  // namespace hmi::port
