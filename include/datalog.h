#pragma once
// In-memory data kept in PSRAM, shared by all tasks (thread-safe):
//
//  * Sensor history: two rings of {primary, metric1, metric2} averages.
//      fine   : one slot per 10 s, 24 h  (8640 slots, ~101 KB)
//      coarse : one slot per minute, 7 d (10080 slots, ~118 KB)
//    A slot without samples holds NaN (gap).  The storage task saves both
//    rings to the SD card every 10 min and to LittleFS (flash) every hour;
//    begin() + load() restore the newest copy at boot.
//  * Event log: the last kEvents lines of storage::logEvent() (also preloaded
//    from the tail of /logs/events.log), newest first for the UI.

#include <Arduino.h>
#include <FS.h>

namespace datalog {

constexpr uint32_t kFineSec = 10, kFineSlots = 24 * 3600 / kFineSec;
constexpr uint32_t kCoarseSec = 60, kCoarseSlots = 7 * 24 * 3600 / kCoarseSec;
constexpr uint16_t kEvents = 400;
constexpr uint16_t kEventLen = 112;

enum Range : uint8_t { R_1H, R_24H, R_7D, R_COUNT };

void begin();  // allocate the PSRAM buffers (setup, before any other call)

// --- sensor history ---
void addSample(float primary, float metric1, float metric2);  // app task, every reading
void tick();  // close finished 10 s / 1 min slots; call at least once per second
// Average of `metric` (0 primary, 1 metric1, 2 metric2) over `points` equal
// buckets spanning `range`, oldest first; NaN where a bucket has no data.
uint16_t query(uint8_t range, uint8_t metric, float* out, uint16_t points);
uint32_t fineFilled();  // 10 s slots holding data or gaps since history began

// Persistence (storage task).  `seq` grows with every save, so at boot the
// newer of the SD and flash copies wins.
bool save(fs::FS& fs, const char* path, bool slowWrites);
bool load(fs::FS& fs, const char* path);  // only if newer than what is loaded
uint32_t seq();
size_t bytes();  // PSRAM used by history + events

// --- event log ---
void addEvent(const char* line);
uint16_t eventCount();
bool eventAt(uint16_t i, char* out, size_t len);  // 0 = newest
uint32_t eventSerial();  // +1 per addEvent(), for cheap UI refresh checks

}  // namespace datalog
