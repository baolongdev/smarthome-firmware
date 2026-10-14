// LVGL allocator (LV_STDLIB_CUSTOM).
//
// With the stock CLIB allocator every block under 4 KB (all widgets and
// styles) landed in internal RAM and the memory page alone pushed it past
// 90 %.  Policies (lvmem_set_policy, console "bench psram|hybrid"):
//   LVMEM_PSRAM (default): every block in PSRAM, internal RAM only as a
//     fallback.  Console "bench" measured the same full-redraw time as hybrid
//     (62-107 ms per 480x480 frame depending on the page) with ~70 KB more
//     internal RAM left free.
//   LVMEM_HYBRID: small blocks go to internal RAM (expected to be faster
//     to walk, but not measurably so) as long as internal RAM keeps
//     kReserve bytes free for Wi-Fi, SD/DMA and task stacks; everything else
//     (big blocks, draw layers, image caches) goes to PSRAM.
#include "lv_mem_psram.h"

#include <esp_heap_caps.h>
#include <soc/soc_memory_layout.h>  // esp_ptr_external_ram (IDF 4.4)
#include <lvgl.h>

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

#define CAP_INT (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define CAP_EXT (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

static const size_t kSmallBlock = 1024;     // hybrid: widgets, styles, labels
static const size_t kReserve = 96 * 1024;   // hybrid: internal RAM kept free

static int s_policy = LVMEM_PSRAM;
static size_t s_bytes[2];  // [0] internal, [1] PSRAM, as seen by LVGL

static void account(void* p, int sign) {
    if (!p) return;
    const size_t n = heap_caps_get_allocated_size(p);
    s_bytes[esp_ptr_external_ram(p) ? 1 : 0] += sign * (long)n;
}

static int wantsInternal(size_t size) {
    return s_policy == LVMEM_HYBRID && size <= kSmallBlock &&
           heap_caps_get_free_size(MALLOC_CAP_INTERNAL) > kReserve + size;
}

void lvmem_set_policy(int policy) { s_policy = policy; }
int lvmem_policy(void) { return s_policy; }
void lvmem_stats(size_t* internalBytes, size_t* psramBytes) {
    *internalBytes = s_bytes[0];
    *psramBytes = s_bytes[1];
}

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void* mem, size_t bytes) {
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}
void lv_mem_remove_pool(lv_mem_pool_t pool) { LV_UNUSED(pool); }

void* lv_malloc_core(size_t size) {
    void* p = NULL;
    if (wantsInternal(size)) p = heap_caps_malloc(size, CAP_INT);
    if (!p) p = heap_caps_malloc(size, CAP_EXT);
    if (!p) p = heap_caps_malloc(size, CAP_INT);
    account(p, 1);
    return p;
}

void* lv_realloc_core(void* p, size_t new_size) {
    if (!p) return lv_malloc_core(new_size);
    // Grow in place where the block already lives; move only if that fails.
    const uint32_t caps = esp_ptr_external_ram(p) ? CAP_EXT : CAP_INT;
    account(p, -1);
    void* q = heap_caps_realloc(p, new_size, caps);
    if (!q) q = heap_caps_realloc(p, new_size, caps == CAP_EXT ? CAP_INT : CAP_EXT);
    account(q ? q : p, 1);
    return q;
}

void lv_free_core(void* p) {
    account(p, -1);
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t* mon_p) { LV_UNUSED(mon_p); }
lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }

#endif
