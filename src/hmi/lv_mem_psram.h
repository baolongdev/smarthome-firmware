#pragma once
// Placement policy and accounting of the LVGL allocator (lv_mem_psram.c).
// Called from the UI task only.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { LVMEM_PSRAM = 0, LVMEM_HYBRID = 1 };

void lvmem_set_policy(int policy);
int lvmem_policy(void);
// Bytes LVGL currently holds in internal RAM and in PSRAM.
void lvmem_stats(size_t* internalBytes, size_t* psramBytes);

#ifdef __cplusplus
}
#endif
