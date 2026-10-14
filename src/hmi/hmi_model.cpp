#include "hmi_model.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>
#include <string.h>

#include <new>

namespace hmi {

namespace {
// Created during static init (single-threaded), before any task can call a setter.
SemaphoreHandle_t s_lock = xSemaphoreCreateRecursiveMutex();
Model& s_model = newModel();
}  // namespace

// ~5.5 KB each (file list + file preview): kept in PSRAM, which is already
// mapped when static constructors run (CONFIG_SPIRAM_BOOT_INIT).
Model& newModel() {
    void* p = heap_caps_malloc(sizeof(Model), MALLOC_CAP_SPIRAM);
    if (!p) p = malloc(sizeof(Model));
    return *new (p) Model();
}

Model& model() { return s_model; }
void modelLock() { xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }
void modelUnlock() { xSemaphoreGiveRecursive(s_lock); }

void setPrimary(float value, SensorState state) {
    ModelLock lock;
    Model& m = model();
    if (m.primary.value == value && m.primary.state == state) return;
    m.primary = {value, state};
    if (state == SensorState::Ok) strlcpy(m.lastOkClock, m.clock, sizeof(m.lastOkClock));
    m.dirty |= D_PRIMARY;
}

void setMetric(uint8_t index, float value, SensorState state) {
    ModelLock lock;
    if (index > 1) return;
    Metric& mt = model().metric[index];
    if (mt.value == value && mt.state == state) return;
    mt = {value, state};
    model().dirty |= D_METRICS;
}

void setLink(Link link) {
    ModelLock lock;
    if (model().link == link) return;
    model().link = link;
    model().dirty |= D_LINK | D_SYSTEM;
}

void setClock(const char* hhmm) {
    ModelLock lock;
    Model& m = model();
    if (strcmp(m.clock, hhmm) == 0) return;
    strlcpy(m.clock, hhmm, sizeof(m.clock));
    m.dirty |= D_CLOCK;
}

void setRelay(uint8_t index, bool on) {
    ModelLock lock;
    if (index >= RELAY_COUNT || model().relay[index] == on) return;
    model().relay[index] = on;
    model().dirty |= D_RELAYS;
}

void setBrightness(uint8_t pct) {
    ModelLock lock;
    pct = pct < 10 ? 10 : (pct > 100 ? 100 : pct);
    if (model().brightness == pct) return;
    model().brightness = pct;
    model().dirty |= D_BRIGHTNESS;
}

void setAutoDim(bool on) {
    ModelLock lock;
    if (model().autoDim == on) return;
    model().autoDim = on;
    model().dirty |= D_AUTODIM;
}

void setTheme(uint8_t index) {
    ModelLock lock;
    if (model().theme == index) return;
    model().theme = index;
    model().dirty |= D_THEME;
}

void setRoomName(const char* name) {
    ModelLock lock;
    if (!name || !name[0] || strcmp(model().room, name) == 0) return;
    strlcpy(model().room, name, sizeof(model().room));
    model().dirty |= D_ROOM;
}

void setWifiNetworks(const WifiNetwork* list, uint8_t count) {
    ModelLock lock;
    Model& m = model();
    m.networkCount = count > 20 ? 20 : count;
    memcpy(m.networks, list, sizeof(WifiNetwork) * m.networkCount);
    m.dirty |= D_WIFI_LIST;
}

void setWifiScanning(bool scanning) {
    ModelLock lock;
    if (model().scanning == scanning) return;
    model().scanning = scanning;
    model().dirty |= D_WIFI_LIST;
}

void setWifiJoin(WifiJoin state, const char* message) {
    ModelLock lock;
    Model& m = model();
    m.join = state;
    strlcpy(m.joinMessage, message ? message : "", sizeof(m.joinMessage));
    m.dirty |= D_WIFI_JOIN;
}

void setStorage(const StorageInfo& info) {
    ModelLock lock;
    model().storage = info;  // sent rarely (mount, card change, every 10 min)
    model().dirty |= D_STORAGE;
}

void setUpdateProgress(int8_t pct) {
    ModelLock lock;
    if (model().updatePct == pct) return;
    model().updatePct = pct;
    model().dirty |= D_UPDATE;
}

void configureDevices(const DeviceInfo* list, uint8_t count) {
    ModelLock lock;
    Model& m = model();
    m.deviceCount = count > kMaxDevices ? kMaxDevices : count;
    memcpy(m.devices, list, sizeof(DeviceInfo) * m.deviceCount);
    m.dirty |= D_DEVICES;
}

void setDeviceState(uint8_t device, uint8_t mask, bool online) {
    ModelLock lock;
    Model& m = model();
    if (device >= m.deviceCount) return;
    if (m.deviceKnown[device] && m.deviceMask[device] == mask && m.deviceOnline[device] == online) return;
    m.deviceKnown[device] = true;
    m.deviceMask[device] = mask;
    m.deviceOnline[device] = online;
    m.dirty |= D_DEVICES;
}

void setFileList(const FileList& list) {
    ModelLock lock;
    model().files = list;
    model().dirty |= D_FILES;
}

namespace {
constexpr size_t kFileTextCap = 64 * 1024;
char* s_fileText = nullptr;  // kFileTextCap + 1, allocated on first use
uint16_t* s_fileImage = nullptr;  // kImageMax^2 RGB565, allocated on first image
}  // namespace

const char* fileText() { return s_fileText ? s_fileText : ""; }
const uint16_t* fileImage() { return s_fileImage; }

void setFileView(const FileView& view, const char* text, const uint16_t* pixels) {
    ModelLock lock;
    if (!s_fileText) s_fileText = static_cast<char*>(heap_caps_calloc(1, kFileTextCap + 1, MALLOC_CAP_SPIRAM));
    model().view = view;
    if (pixels && view.image) {
        if (!s_fileImage)
            s_fileImage = static_cast<uint16_t*>(heap_caps_malloc((size_t)kImageMax * kImageMax * 2, MALLOC_CAP_SPIRAM));
        if (s_fileImage && view.imgW <= kImageMax && view.imgH <= kImageMax)
            memcpy(s_fileImage, pixels, (size_t)view.imgW * view.imgH * 2);
        else
            model().view.image = false;
    }
    if (s_fileText) {
        const size_t n = text && view.textLen < kFileTextCap ? view.textLen : (text ? kFileTextCap : 0);
        if (n) memcpy(s_fileText, text, n);
        s_fileText[n] = 0;
        model().view.textLen = n;
    }
    model().dirty |= D_FILEVIEW;
}

void setSystemInfo(const SystemInfo& info) {
    ModelLock lock;
    Model& m = model();
    m.sys = info;  // the net task already rate-limits this to every 5 s
    m.dirty |= D_SYSTEM;
}

}  // namespace hmi
