#pragma once
// Hardware adapter: LVGL display/input on top of the LovyanGFX panel driver
// (include/lgfx_4848s040_reference.h).  The UI layer never calls LGFX.

#include <lvgl.h>

class LGFX;

namespace hmi::port {

// lv_init(), draw buffers, flush, touch (and keypad if HMI_BTN_* pins exist).
void begin(LGFX& lcd);

// While set, the next touch only wakes the panel: it is swallowed until the
// finger lifts, then onWake() runs.  Used by auto-dim.
void armWakeGuard(void (*onWake)());

// Testing: replace the touch panel with a synthetic vertical drag from (x, y0)
// to (x, y1) over `ms`, then release.  Goes through the real LVGL indev path.
void injectDrag(int16_t x, int16_t y0, int16_t y1, uint16_t ms);

}  // namespace hmi::port
