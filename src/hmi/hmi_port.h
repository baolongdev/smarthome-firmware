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

}  // namespace hmi::port
