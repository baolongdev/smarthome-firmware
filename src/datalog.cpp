#include "datalog.h"

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <math.h>
#include <algorithm>
#include <time.h>

namespace datalog {
namespace {

// Created during static init, before any task can call in.
SemaphoreHandle_t s_lock = xSemaphoreCreateMutex();
struct Lock {
    Lock() { xSemaphoreTake(s_lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_lock); }
};

struct Ring {
    float* v = nullptr;  // len * 3 floats in PSRAM
    uint32_t len = 0;
    uint32_t head = 0;    // next slot to write
    uint32_t filled = 0;  // slots written so far (<= len)

    void push(const float* x) {
        memcpy(&v[head * 3], x, 3 * sizeof(float));
        head = (head + 1) % len;
        if (filled < len) filled++;
    }
    // k-th slot counted back from the newest (k = 0 newest); NaN if never written
    const float* back(uint32_t k) const {
        static const float kNone[3] = {NAN, NAN, NAN};
        if (k >= filled) return kNone;
        return &v[((head + len - 1 - k) % len) * 3];
    }
};

struct Acc {  // running average of the open slot
    float sum[3] = {};
    uint16_t n = 0;
    uint32_t slot = 0;  // slot number (millis / period) it belongs to

    void add(const float* x) {
        for (int k = 0; k < 3; k++) sum[k] += x[k];
        n++;
    }
    void close(float* out) {
        for (int k = 0; k < 3; k++) {
            out[k] = n ? sum[k] / n : NAN;
            sum[k] = 0;
        }
        n = 0;
    }
};

Ring s_fine, s_coarse;
Acc s_fineAcc, s_coarseAcc;
bool s_started = false;
uint32_t s_seq = 0;

char* s_events = nullptr;  // kEvents * kEventLen in PSRAM
uint16_t s_evHead = 0, s_evCount = 0;
uint32_t s_evSerial = 0;

bool timeValid(time_t* now) {
    *now = time(nullptr);
    return *now > 1700000000;  // NTP has answered
}

// Close every finished slot of `acc` up to `slot` (gaps become NaN).
void advance(Ring& r, Acc& acc, uint32_t slot) {
    uint32_t steps = slot - acc.slot;
    if (steps > r.len) steps = r.len + 1;  // long stall: the whole ring is gaps
    for (uint32_t i = 0; i < steps; i++) {
        float v[3];
        acc.close(v);
        r.push(v);
    }
    acc.slot = slot;
}

struct Header {
    uint32_t magic = 0x54534948;  // "HIST"
    uint16_t version = 1;
    uint16_t pad = 0;
    uint32_t seq = 0;
    uint32_t epoch = 0;  // wall clock at save, 0 if unknown
    uint32_t fineLen, fineHead, fineFilled;
    uint32_t coarseLen, coarseHead, coarseFilled;
};

}  // namespace

void begin() {
    if (s_fine.v) return;
    s_fine.len = kFineSlots;
    s_coarse.len = kCoarseSlots;
    s_fine.v = static_cast<float*>(heap_caps_malloc(kFineSlots * 3 * sizeof(float), MALLOC_CAP_SPIRAM));
    s_coarse.v = static_cast<float*>(heap_caps_malloc(kCoarseSlots * 3 * sizeof(float), MALLOC_CAP_SPIRAM));
    s_events = static_cast<char*>(heap_caps_calloc(kEvents, kEventLen, MALLOC_CAP_SPIRAM));
    if (!s_fine.v || !s_coarse.v || !s_events) Serial.println("datalog: PSRAM allocation failed");
}

void addSample(float primary, float metric1, float metric2) {
    if (!s_fine.v || isnan(primary)) return;
    const float x[3] = {primary, metric1, metric2};
    Lock lock;
    if (!s_started) {  // history starts with the first reading
        s_started = true;
        s_fineAcc.slot = millis() / (kFineSec * 1000);
        s_coarseAcc.slot = millis() / (kCoarseSec * 1000);
    }
    s_fineAcc.add(x);
    s_coarseAcc.add(x);
}

void tick() {
    if (!s_fine.v) return;
    Lock lock;
    if (!s_started) return;
    const uint32_t ms = millis();
    advance(s_fine, s_fineAcc, ms / (kFineSec * 1000));
    advance(s_coarse, s_coarseAcc, ms / (kCoarseSec * 1000));
}

uint16_t query(uint8_t range, uint8_t metric, float* out, uint16_t points) {
    if (!s_fine.v || metric > 2 || !points) return 0;
    const Ring& r = range == R_7D ? s_coarse : s_fine;
    const uint32_t span = range == R_1H ? 3600 / kFineSec : range == R_24H ? kFineSlots : kCoarseSlots;
    Lock lock;
    for (uint16_t p = 0; p < points; p++) {
        // bucket p covers slots [lo, hi) counted back from the newest, oldest bucket first
        const uint32_t hi = span - (uint64_t)p * span / points;
        const uint32_t lo = span - (uint64_t)(p + 1) * span / points;
        float sum = 0;
        uint32_t n = 0;
        for (uint32_t k = lo; k < hi; k++) {
            const float v = r.back(k)[metric];
            if (!isnan(v)) {
                sum += v;
                n++;
            }
        }
        out[p] = n ? sum / n : NAN;
    }
    return points;
}

uint32_t fineFilled() {
    Lock lock;
    return s_fine.filled;
}

uint32_t seq() { return s_seq; }

size_t bytes() {
    return (kFineSlots + kCoarseSlots) * 3 * sizeof(float) + (size_t)kEvents * kEventLen;
}

bool save(fs::FS& fs, const char* path, bool slowWrites) {
    if (!s_fine.v) return false;
    const size_t fineBytes = kFineSlots * 3 * sizeof(float), coarseBytes = kCoarseSlots * 3 * sizeof(float);
    // Copy under the lock (fast, PSRAM to PSRAM), write without it.
    uint8_t* buf = static_cast<uint8_t*>(heap_caps_malloc(sizeof(Header) + fineBytes + coarseBytes, MALLOC_CAP_SPIRAM));
    if (!buf) return false;
    Header h;
    {
        Lock lock;
        if (!s_started && s_fine.filled == 0) {
            heap_caps_free(buf);
            return false;  // nothing recorded yet: keep the older copy
        }
        h.seq = ++s_seq;
        time_t now;
        h.epoch = timeValid(&now) ? (uint32_t)now : 0;
        h.fineLen = s_fine.len, h.fineHead = s_fine.head, h.fineFilled = s_fine.filled;
        h.coarseLen = s_coarse.len, h.coarseHead = s_coarse.head, h.coarseFilled = s_coarse.filled;
        memcpy(buf + sizeof(Header), s_fine.v, fineBytes);
        memcpy(buf + sizeof(Header) + fineBytes, s_coarse.v, coarseBytes);
    }
    memcpy(buf, &h, sizeof(h));
    const size_t total = sizeof(Header) + fineBytes + coarseBytes;
    char tmp[48];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    File f = fs.open(tmp, FILE_WRITE);
    bool ok = false;
    if (f) {
        // Flash writes stall the cache (and the RGB panel's PSRAM reads):
        // 4 KB at a time with a pause keeps each stall short.
        const size_t chunk = slowWrites ? 4096 : 32768;
        size_t done = 0;
        while (done < total) {
            const size_t n = std::min(chunk, total - done);
            if (f.write(buf + done, n) != n) break;
            done += n;
            if (slowWrites) vTaskDelay(pdMS_TO_TICKS(20));
        }
        f.close();
        ok = done == total;
    }
    heap_caps_free(buf);
    if (!ok) {
        fs.remove(tmp);
        return false;
    }
    if (fs.exists(path)) fs.remove(path);
    return fs.rename(tmp, path);
}

bool load(fs::FS& fs, const char* path) {
    if (!s_fine.v || !fs.exists(path)) return false;
    File f = fs.open(path, FILE_READ);
    if (!f) return false;
    Header h;
    bool ok = f.read(reinterpret_cast<uint8_t*>(&h), sizeof(h)) == sizeof(h) && h.magic == Header{}.magic &&
              h.version == 1 && h.fineLen == kFineSlots && h.coarseLen == kCoarseSlots && h.seq > s_seq;
    if (ok) {
        Lock lock;
        ok = f.read(reinterpret_cast<uint8_t*>(s_fine.v), kFineSlots * 3 * sizeof(float)) == kFineSlots * 3 * sizeof(float) &&
             f.read(reinterpret_cast<uint8_t*>(s_coarse.v), kCoarseSlots * 3 * sizeof(float)) == kCoarseSlots * 3 * sizeof(float);
        if (ok) {
            s_fine.head = h.fineHead % kFineSlots, s_fine.filled = std::min<uint32_t>(h.fineFilled, kFineSlots);
            s_coarse.head = h.coarseHead % kCoarseSlots, s_coarse.filled = std::min<uint32_t>(h.coarseFilled, kCoarseSlots);
            s_seq = h.seq;
            // Time spent switched off becomes a gap (only if both clocks are known).
            time_t now;
            if (h.epoch && timeValid(&now) && (uint32_t)now > h.epoch) {
                const float none[3] = {NAN, NAN, NAN};
                const uint32_t off = (uint32_t)now - h.epoch;
                for (uint32_t i = 0; i < std::min(off / kFineSec, kFineSlots); i++) s_fine.push(none);
                for (uint32_t i = 0; i < std::min(off / kCoarseSec, kCoarseSlots); i++) s_coarse.push(none);
            }
        } else {
            s_fine.filled = s_coarse.filled = 0;
        }
    }
    f.close();
    return ok;
}

void addEvent(const char* line) {
    if (!s_events || !line) return;
    Lock lock;
    strlcpy(&s_events[s_evHead * kEventLen], line, kEventLen);
    s_evHead = (s_evHead + 1) % kEvents;
    if (s_evCount < kEvents) s_evCount++;
    s_evSerial++;
}

uint16_t eventCount() {
    Lock lock;
    return s_evCount;
}

bool eventAt(uint16_t i, char* out, size_t len) {
    Lock lock;
    if (!s_events || i >= s_evCount) return false;
    strlcpy(out, &s_events[((s_evHead + kEvents - 1 - i) % kEvents) * kEventLen], len);
    return true;
}

uint32_t eventSerial() { return s_evSerial; }

}  // namespace datalog
