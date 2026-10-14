#pragma once
// HMI state: plain data + dirty bits.  Setters in hmi.h may be called from any
// FreeRTOS task; they take the model lock.  The UI task copies a snapshot under
// the same lock and refreshes only the widgets that changed.

#include "config.h"
#include "hmi.h"

namespace hmi {

enum Dirty : uint32_t {
    D_PRIMARY = 1u << 0,
    D_METRICS = 1u << 1,
    D_LINK = 1u << 2,
    D_CLOCK = 1u << 3,
    D_RELAYS = 1u << 4,
    D_BRIGHTNESS = 1u << 5,
    D_AUTODIM = 1u << 6,
    D_SYSTEM = 1u << 7,
    D_THEME = 1u << 8,  // palette changed: widgets are rebuilt
    D_WIFI_LIST = 1u << 9,
    D_WIFI_JOIN = 1u << 10,
    D_STORAGE = 1u << 11,
    D_UPDATE = 1u << 12,
    D_ROOM = 1u << 13,
    D_DEVICES = 1u << 14,
    D_FILES = 1u << 15,
    D_FILEVIEW = 1u << 16,
    D_ALL = 0xFFFFFFFFu,
};

struct Metric {
    float value = 0;
    SensorState state = SensorState::Loading;
};

struct Model {
    Metric primary;
    Metric metric[2];
    char lastOkClock[6] = "--:--";  // shown while the primary value is stale
    Link link = Link::Connecting;
    char clock[6] = "--:--";
    bool relay[RELAY_COUNT] = {};
    uint8_t brightness = 80;
    bool autoDim = true;
    uint8_t theme = 0;
    char room[32] = HMI_ROOM_NAME;
    WifiNetwork networks[20];
    uint8_t networkCount = 0;
    bool scanning = false;
    WifiJoin join = WifiJoin::Idle;
    char joinMessage[48] = "";
    StorageInfo storage;
    int8_t updatePct = -1;
    uint8_t deviceCount = 0;           // fixed after configureDevices()
    DeviceInfo devices[kMaxDevices];
    uint8_t deviceMask[kMaxDevices] = {};
    bool deviceOnline[kMaxDevices] = {};
    bool deviceKnown[kMaxDevices] = {};  // false until the first poll answers or fails
    SystemInfo sys;
    FileList files;
    FileView view;
    uint32_t dirty = D_ALL;
};

Model& model();  // hold ModelLock while touching it
Model& newModel();  // zero-state Model allocated in PSRAM
// Text of the file shown in the viewer (FileView::textLen bytes), PSRAM.
// Read it under ModelLock: setFileView() rewrites it.
const char* fileText();
const uint16_t* fileImage();  // FileView::imgW x imgH native RGB565, same rules

void modelLock();
void modelUnlock();
struct ModelLock {
    ModelLock() { modelLock(); }
    ~ModelLock() { modelUnlock(); }
    ModelLock(const ModelLock&) = delete;
    ModelLock& operator=(const ModelLock&) = delete;
};

}  // namespace hmi
