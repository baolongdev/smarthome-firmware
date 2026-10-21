#include "storage.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>
// After SD.h: LovyanGFX only adds its drawXxxFile(fs::FS&, ...) helpers when FS.h is in.
#include <LovyanGFX.hpp>
#include <Update.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <algorithm>
#include <stdarg.h>
#include <time.h>

#include "config.h"
#include "datalog.h"
#include "hmi.h"

namespace storage {
namespace {

constexpr int kCs = 42, kMiso = 41, kSck = 48, kMosi = 47;
constexpr uint32_t kEventLogMax = 256 * 1024;
constexpr uint32_t kStatsEveryMs = 10 * 60 * 1000;  // FAT free-space scan is slow
constexpr const char* kEventLog = "/logs/events.log";
constexpr const char* kFirmware = "/firmware.bin";
constexpr const char* kBackupFw = "/backup/firmware-prev.bin";   // running image, saved before an update
constexpr const char* kBackupVer = "/backup/firmware-prev.txt";  // its version string
constexpr const char* kHistorySd = "/data/history.bin";
constexpr const char* kHistoryLfs = "/history.bin";
constexpr uint32_t kHistorySdEveryMs = 10 * 60 * 1000;
constexpr uint32_t kHistoryLfsEveryMs = 60 * 60 * 1000;  // flash: few writes, see datalog::save
constexpr uint32_t kRetentionEveryMs = 6 * 60 * 60 * 1000;
constexpr uint32_t kTextCap = 64 * 1024;                  // file viewer: up to 64 KB of text

enum ItemType : uint8_t { EVENT, SENSORS, FW_UPDATE, SCREENSHOT, LIST, READ, REMOVE, TREE, WIPE, RESTORE, SAVE_HISTORY };

struct Item {
    ItemType type;
    float values[3];
    char text[176];  // EVENT: "<timestamp> <message>"
};

QueueHandle_t s_queue = nullptr;
bool s_mounted = false;
bool s_configLoaded = false;
uint32_t s_mountHz = 0;
char s_cfgRoom[32] = "";
uint8_t s_cfgWifi = 0, s_cfgTasmota = 0;
char s_cfgMqtt[64] = "";
bool s_statsDirty = true;
bool s_lfs = false;          // LittleFS on the 3.4 MB "spiffs" flash partition
uint8_t s_configSource = 0;  // 0 none, 1 SD card, 2 flash copy
uint16_t s_logDays = 90;     // sensor CSV retention (config.json "logDays")
uint32_t s_lastHistorySd = 0, s_lastHistoryLfs = 0, s_lastRetention = 0;
uint32_t s_lastStats = 0;
uint32_t s_lastCheck = 0;

bool mount() {
    pinMode(kCs, OUTPUT);
    digitalWrite(kCs, HIGH);
    SPI.begin(kSck, kMiso, kMosi, kCs);
    // Fast first; some cards only come up at the vendor's 1 MHz.
    for (uint32_t hz : {20000000u, 4000000u, 1000000u}) {
        if (SD.begin(kCs, SPI, hz) && SD.cardType() != CARD_NONE) {
            s_mountHz = hz;
            SD.mkdir("/logs");
            SD.mkdir("/logs/sensors");
            Serial.printf("SD: mounted at %lu Hz, %lu MB\n", (unsigned long)hz,
                          (unsigned long)(SD.cardSize() / (1024 * 1024)));
            return true;
        }
        SD.end();
    }
    return false;
}

// "2026-10-13 14:05:09" when NTP is synced, else "+123s" uptime.
void timestamp(char* buf, size_t len) {
    struct tm t;
    if (getLocalTime(&t, 0)) strftime(buf, len, "%Y-%m-%d %H:%M:%S", &t);
    else snprintf(buf, len, "+%lus", (unsigned long)(millis() / 1000));
}

void enqueue(const Item& it) {
    if (s_queue) xQueueSend(s_queue, &it, 0);
}

// ----- work done inside the storage task -----

void writeEvent(const char* line) {
    File f = SD.open(kEventLog, FILE_READ);
    const size_t size = f ? f.size() : 0;
    if (f) f.close();
    if (size >= kEventLogMax) {
        SD.remove("/logs/events.1.log");
        SD.rename(kEventLog, "/logs/events.1.log");
    }
    f = SD.open(kEventLog, FILE_APPEND);
    if (!f) return;
    f.println(line);
    f.close();
}

void writeSensors(const float* v) {
    char path[40];
    struct tm t;
    if (getLocalTime(&t, 0)) strftime(path, sizeof(path), "/logs/sensors/%Y-%m-%d.csv", &t);
    else strlcpy(path, "/logs/sensors/undated.csv", sizeof(path));
    const bool fresh = !SD.exists(path);
    File f = SD.open(path, FILE_APPEND);
    if (!f) return;
    if (fresh) f.println("time,primary,metric1,metric2");
    char ts[24];
    timestamp(ts, sizeof(ts));
    f.printf("%s,%.2f,%.1f,%.1f\n", ts, v[0], v[1], v[2]);
    f.close();
}

// Copy the running app image to /backup before it is replaced, so the
// previous version can be reinstalled from the card ("sd restore").
void backupRunningFirmware() {
    const esp_partition_t* part = esp_ota_get_running_partition();
    const uint32_t size = ESP.getSketchSize();
    uint8_t* buf = static_cast<uint8_t*>(heap_caps_malloc(4096, MALLOC_CAP_SPIRAM));
    if (!part || !size || !buf) {
        heap_caps_free(buf);
        return;
    }
    SD.mkdir("/backup");
    File f = SD.open("/backup/firmware-prev.tmp", FILE_WRITE);
    uint32_t done = 0;
    while (f && done < size) {
        const uint32_t n = std::min<uint32_t>(4096, size - done);
        if (esp_partition_read(part, done, buf, n) != ESP_OK || f.write(buf, n) != n) break;
        done += n;
    }
    if (f) f.close();
    heap_caps_free(buf);
    if (done != size) {
        SD.remove("/backup/firmware-prev.tmp");
        writeEvent("Firmware backup failed");
        return;
    }
    SD.remove(kBackupFw);
    SD.rename("/backup/firmware-prev.tmp", kBackupFw);
    File v = SD.open(kBackupVer, FILE_WRITE);
    if (v) {
        v.print(FIRMWARE_VERSION);
        v.close();
    }
    char line[96];
    snprintf(line, sizeof(line), "Firmware v%s backed up to %s (%lu bytes)", FIRMWARE_VERSION, kBackupFw, (unsigned long)size);
    writeEvent(line);
}

void installFirmware() {
    if (!SD.exists(kFirmware)) return;
    backupRunningFirmware();
    File f = SD.open(kFirmware, FILE_READ);
    if (!f) return;
    const size_t size = f.size();
    if (!Update.begin(size, U_FLASH)) {
        logEvent("SD update: begin failed (%s)", Update.errorString());
        f.close();
        return;
    }
    logEvent("SD firmware update started (%u bytes)", (unsigned)size);
    hmi::setUpdateProgress(0);
    static uint8_t* buf = (uint8_t*)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);  // only used here
    size_t done = 0;
    while (buf && done < size) {
        const size_t n = f.read(buf, 4096);
        if (n == 0 || Update.write(buf, n) != n) break;
        done += n;
        hmi::setUpdateProgress(done * 100 / size);  // UI task renders it
    }
    f.close();
    if (done != size || !Update.end(true)) {
        hmi::setUpdateProgress(-1);
        Serial.printf("SD update failed at %u/%u (%s)\n", (unsigned)done, (unsigned)size, Update.errorString());
        char line[120];
        snprintf(line, sizeof(line), "SD update failed at %u/%u bytes", (unsigned)done, (unsigned)size);
        writeEvent(line);
        Update.abort();
        return;
    }
    SD.remove("/firmware.installed.bin");
    SD.rename(kFirmware, "/firmware.installed.bin");
    writeEvent("SD update OK, restarting");
    delay(300);
    ESP.restart();
}

// Console "sd restore": put the backed-up image back as /firmware.bin and
// install it (the current image becomes the new backup).
void restoreFirmware() {
    File src = SD.open(kBackupFw, FILE_READ);
    if (!src) {
        Serial.println("SD restore: no backup");
        return;
    }
    File dst = SD.open(kFirmware, FILE_WRITE);
    uint8_t* buf = static_cast<uint8_t*>(heap_caps_malloc(4096, MALLOC_CAP_SPIRAM));
    bool ok = dst && buf;
    while (ok && src.available()) {
        const size_t n = src.read(buf, 4096);
        ok = n && dst.write(buf, n) == n;
    }
    heap_caps_free(buf);
    src.close();
    if (dst) dst.close();
    if (!ok) {
        SD.remove(kFirmware);
        Serial.println("SD restore: copy failed");
        return;
    }
    writeEvent("Restoring backed-up firmware");
    installFirmware();
}

void saveScreenshot() {
    SD.mkdir("/screenshots");
    char path[40];
    for (int i = 1; i < 1000; i++) {
        snprintf(path, sizeof(path), "/screenshots/shot_%03d.bmp", i);
        if (SD.exists(path)) continue;
        File f = SD.open(path, FILE_WRITE);
        const bool ok = f && hmi::screenshotBmp(f);
        if (f) f.close();
        Serial.printf(ok ? "Saved %s\n" : "Screenshot failed: %s\n", path);
        s_statsDirty = true;
        return;
    }
}

// ----- file browser -----

hmi::FileList s_list;  // static: too big for the task stack
hmi::FileView s_view;
char* s_text = nullptr;  // kTextCap + 1 in PSRAM, read buffer for the viewer

void parentOf(const char* path, char* out, size_t len) {
    strlcpy(out, path, len);
    char* slash = strrchr(out, '/');
    if (!slash || slash == out) strlcpy(out, "/", len);
    else *slash = 0;
}

void listDir(const char* dir) {
    s_list = hmi::FileList{};
    strlcpy(s_list.path, dir, sizeof(s_list.path));
    if (!s_mounted) {
        strlcpy(s_list.error, "Không có thẻ nhớ", sizeof(s_list.error));
    } else if (File d = SD.open(dir)) {
        if (!d.isDirectory()) {
            strlcpy(s_list.error, "Không phải thư mục", sizeof(s_list.error));
        } else {
            for (File f = d.openNextFile(); f; f = d.openNextFile()) {
                if (s_list.count >= hmi::kMaxFiles) {
                    s_list.truncated = true;
                    f.close();
                    break;
                }
                hmi::FileEntry& e = s_list.items[s_list.count++];
                strlcpy(e.name, f.name(), sizeof(e.name));  // basename on arduino-esp32 2.x
                e.dir = f.isDirectory();
                e.size = e.dir ? 0 : f.size();
                f.close();
            }
            // Folders first, then by name.
            std::sort(s_list.items, s_list.items + s_list.count, [](const hmi::FileEntry& a, const hmi::FileEntry& b) {
                if (a.dir != b.dir) return a.dir;
                return strcasecmp(a.name, b.name) < 0;
            });
        }
        d.close();
    } else {
        strlcpy(s_list.error, "Không mở được thư mục", sizeof(s_list.error));
    }
    hmi::setFileList(s_list);
}

bool isTextFile(const char* path) {
    const char* dot = strrchr(path, '.');
    if (!dot) return false;
    static const char* const kText[] = {".txt", ".log", ".csv", ".json", ".cfg", ".ini", ".md", ".yaml", ".yml"};
    for (const char* ext : kText) {
        if (strcasecmp(dot, ext) == 0) return true;
    }
    return false;
}

bool isImageFile(const char* path) {
    const char* dot = strrchr(path, '.');
    if (!dot) return false;
    for (const char* ext : {".bmp", ".jpg", ".jpeg", ".png"}) {
        if (strcasecmp(dot, ext) == 0) return true;
    }
    return false;
}

bool isCsv(const char* path) {
    const char* dot = strrchr(path, '.');
    return dot && strcasecmp(dot, ".csv") == 0;
}

// Width/height from the file header (BMP, PNG IHDR, JPEG SOFn); false if unknown.
bool imageSize(File& f, const char* path, uint16_t* w, uint16_t* h) {
    uint8_t b[32];
    f.seek(0);
    if (f.read(b, sizeof(b)) != sizeof(b)) return false;
    const char* dot = strrchr(path, '.');
    if (b[0] == 'B' && b[1] == 'M') {
        const int32_t bw = b[18] | b[19] << 8 | b[20] << 16 | b[21] << 24;
        const int32_t bh = b[22] | b[23] << 8 | b[24] << 16 | b[25] << 24;
        *w = abs(bw), *h = abs(bh);
    } else if (b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') {
        *w = b[18] << 8 | b[19];  // IHDR width/height are big-endian 32-bit
        *h = b[22] << 8 | b[23];
    } else if (b[0] == 0xFF && b[1] == 0xD8) {
        // walk the markers up to the first SOF (C0..CF except C4, C8, CC)
        uint32_t pos = 2;
        for (int guard = 0; guard < 64; guard++) {
            uint8_t m[9];
            f.seek(pos);
            if (f.read(m, 4) != 4 || m[0] != 0xFF) return false;
            const uint16_t len = m[2] << 8 | m[3];
            if (m[1] >= 0xC0 && m[1] <= 0xCF && m[1] != 0xC4 && m[1] != 0xC8 && m[1] != 0xCC) {
                if (f.read(m, 5) != 5) return false;
                *h = m[1] << 8 | m[2];
                *w = m[3] << 8 | m[4];
                break;
            }
            pos += 2 + len;
        }
    } else {
        (void)dot;
        return false;
    }
    f.seek(0);
    return *w && *h;
}

// Decode into a PSRAM sprite scaled to fit kImageMax x kImageMax (aspect kept,
// never enlarged), then hand native-endian RGB565 to the UI.
void readImage(File& f, const char* path) {
    uint16_t w = 0, h = 0;
    const bool known = imageSize(f, path, &w, &h);
    s_view.srcW = w, s_view.srcH = h;
    float scale = 1.0f;
    if (known) scale = std::min(1.0f, std::min((float)hmi::kImageMax / w, (float)hmi::kImageMax / h));
    const uint16_t fw = known ? std::max<uint16_t>(1, w * scale) : hmi::kImageMax;
    const uint16_t fh = known ? std::max<uint16_t>(1, h * scale) : hmi::kImageMax;
    LGFX_Sprite spr;
    spr.setPsram(true);
    spr.setColorDepth(16);
    if (!spr.createSprite(fw, fh)) {
        strlcpy(s_view.error, "Không đủ bộ nhớ để xem ảnh", sizeof(s_view.error));
        hmi::setFileView(s_view);
        return;
    }
    spr.fillScreen(TFT_BLACK);
    const uint32_t t0 = millis();
    const char* dot = strrchr(path, '.');
    const float z = known ? scale : 0.0f;  // 0: LovyanGFX fits the box itself
    bool ok;
    if (strcasecmp(dot, ".png") == 0) ok = spr.drawPngFile(SD, path, 0, 0, fw, fh, 0, 0, z, z);
    else if (strcasecmp(dot, ".bmp") == 0) ok = spr.drawBmpFile(SD, path, 0, 0, fw, fh, 0, 0, z, z);
    else ok = spr.drawJpgFile(SD, path, 0, 0, fw, fh, 0, 0, z, z);
    Serial.printf("image: %s %ux%u -> %ux%u in %lu ms%s\n", path, w, h, fw, fh, (unsigned long)(millis() - t0),
                  ok ? "" : " (decode failed)");
    if (!ok) {
        strlcpy(s_view.error, "Không giải mã được ảnh", sizeof(s_view.error));
        hmi::setFileView(s_view);
        spr.deleteSprite();
        return;
    }
    // LovyanGFX 16-bit sprites are byte-swapped; LVGL wants native RGB565.
    uint16_t* px = static_cast<uint16_t*>(spr.getBuffer());
    for (uint32_t i = 0, n = (uint32_t)fw * fh; i < n; i++) px[i] = __builtin_bswap16(px[i]);
    s_view.image = true;
    s_view.imgW = fw, s_view.imgH = fh;
    hmi::setFileView(s_view, nullptr, px);
    spr.deleteSprite();
}

void readFile(const char* path) {
    s_view = hmi::FileView{};
    strlcpy(s_view.path, path, sizeof(s_view.path));
    File f = s_mounted ? SD.open(path, FILE_READ) : File();
    if (!f || f.isDirectory()) {
        strlcpy(s_view.error, "Không mở được tệp", sizeof(s_view.error));
        if (f) f.close();
        hmi::setFileView(s_view);
        return;
    }
    s_view.size = f.size();
    if (isImageFile(path)) {
        readImage(f, path);
        f.close();
        return;
    }
    s_view.previewable = isTextFile(path) && s_text;
    const char* text = nullptr;
    if (s_view.previewable) {
        const size_t cap = kTextCap;
        if (s_view.size > cap) {  // logs: the newest lines are at the end
            s_view.tail = true;
            f.seek(s_view.size - cap);
        }
        size_t n = f.read(reinterpret_cast<uint8_t*>(s_text), cap);
        s_text[n] = 0;
        text = s_text;
        if (s_view.tail) {  // start at a line boundary, not mid-way through UTF-8
            char* nl = strchr(s_text, '\n');
            if (nl) text = nl + 1;
            if (nl && isCsv(path)) {  // the table needs the header row: put it back in front
                char head[256];
                f.seek(0);
                const size_t hn = f.read(reinterpret_cast<uint8_t*>(head), sizeof(head) - 1);
                head[hn] = 0;
                char* hend = strchr(head, '\n');
                if (hend && (size_t)(hend - head + 1) <= (size_t)(nl + 1 - s_text)) {
                    const size_t len = hend - head + 1;
                    memcpy(nl + 1 - len, head, len);  // overwrite the cut first line
                    text = nl + 1 - len;
                }
            }
        }
        for (char* c = s_text; *c; c++) {  // CR and control bytes -> space
            if (*c == '\r' || (static_cast<uint8_t>(*c) < 0x20 && *c != '\n' && *c != '\t')) *c = ' ';
        }
        s_view.textLen = strlen(text);
    }
    f.close();
    hmi::setFileView(s_view, text);
}

void removePath(const char* path) {
    char parent[96];
    parentOf(path, parent, sizeof(parent));
    File f = SD.open(path);
    const bool dir = f && f.isDirectory();
    if (f) f.close();
    const bool ok = dir ? SD.rmdir(path) : SD.remove(path);
    if (ok) {
        logEvent("File deleted from panel: %s", path);
        s_statsDirty = true;
        hmi::FileView cleared;
        hmi::setFileView(cleared);
    } else {
        hmi::FileView v = s_view;
        strlcpy(v.error, dir ? "Chỉ xoá được thư mục trống" : "Không xoá được tệp", sizeof(v.error));
        hmi::setFileView(v);
    }
    listDir(parent);
}

void childPath(const char* dir, const char* name, char* out, size_t len) {
    snprintf(out, len, "%s%s%s", dir, strcmp(dir, "/") == 0 ? "" : "/", name);
}

// Console "sd tree": every folder and file with its size.
void printTree(const char* dir, int depth) {
    File d = SD.open(dir);
    if (!d || !d.isDirectory()) return;
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
        char child[128];
        childPath(dir, f.name(), child, sizeof(child));
        const bool isDir = f.isDirectory();
        if (isDir) Serial.printf("%*s%s/\n", depth * 2, "", f.name());
        else Serial.printf("%*s%s  %lu B\n", depth * 2, "", f.name(), (unsigned long)f.size());
        f.close();
        if (isDir && depth < 8) printTree(child, depth + 1);
    }
    d.close();
}

// Delete everything below `dir` (files first, then the emptied folders).
void wipeDir(const char* dir, uint32_t& files, uint32_t& dirs, int depth) {
    File d = SD.open(dir);
    if (!d || !d.isDirectory()) return;
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
        char child[128];
        childPath(dir, f.name(), child, sizeof(child));
        const bool isDir = f.isDirectory();
        f.close();
        if (isDir) {
            if (depth < 8) wipeDir(child, files, dirs, depth + 1);
            if (SD.rmdir(child)) dirs++;
        } else if (SD.remove(child)) {
            files++;
        }
    }
    d.close();
}

// Console "sd wipe": empty the card (the SD library cannot format FAT).
// Deleting while iterating can skip entries, so repeat until a pass finds
// nothing, then recreate the folders the firmware logs into.
void wipeCard() {
    if (!s_mounted) {
        Serial.println("SD wipe: no card");
        return;
    }
    uint32_t files = 0, dirs = 0;
    for (int pass = 0; pass < 5; pass++) {
        const uint32_t before = files + dirs;
        wipeDir("/", files, dirs, 0);
        if (files + dirs == before) break;
    }
    SD.mkdir("/logs");
    SD.mkdir("/logs/sensors");
    Serial.printf("SD wipe: removed %lu files, %lu folders\n", (unsigned long)files, (unsigned long)dirs);
    s_statsDirty = true;
    hmi::setFileView(hmi::FileView{});
    listDir("/");
    printTree("/", 0);
}

void saveHistory(bool sd, bool lfs) {
    if (sd && s_mounted) {
        SD.mkdir("/data");
        const uint32_t t0 = millis();
        if (datalog::save(SD, kHistorySd, false))
            Serial.printf("history: saved to SD in %lu ms\n", (unsigned long)(millis() - t0));
    }
    if (lfs && s_lfs) {
        const uint32_t t0 = millis();
        if (datalog::save(LittleFS, kHistoryLfs, true))
            Serial.printf("history: saved to flash in %lu ms\n", (unsigned long)(millis() - t0));
    }
    s_statsDirty = true;
}

// Sensor CSVs are one file per day: drop those older than s_logDays.
void enforceRetention() {
    time_t now = time(nullptr);
    if (now < 1700000000 || !s_mounted) return;  // needs the real date
    File d = SD.open("/logs/sensors");
    if (!d || !d.isDirectory()) return;
    char victims[16][32];
    uint8_t count = 0;
    for (File f = d.openNextFile(); f && count < 16; f = d.openNextFile()) {
        struct tm t = {};
        if (sscanf(f.name(), "%4d-%2d-%2d.csv", &t.tm_year, &t.tm_mon, &t.tm_mday) == 3) {
            t.tm_year -= 1900;
            t.tm_mon -= 1;
            t.tm_hour = 12;
            if (difftime(now, mktime(&t)) > s_logDays * 86400.0) strlcpy(victims[count++], f.name(), sizeof(victims[0]));
        }
        f.close();
    }
    d.close();
    for (uint8_t i = 0; i < count; i++) {
        char path[48];
        snprintf(path, sizeof(path), "/logs/sensors/%s", victims[i]);
        SD.remove(path);
    }
    if (count) {
        char line[80];
        snprintf(line, sizeof(line), "Retention: removed %u sensor logs older than %u days", count, s_logDays);
        writeEvent(line);
        s_statsDirty = true;
    }
}

void periodic(uint32_t now) {
    if (now - s_lastHistorySd >= kHistorySdEveryMs) {
        s_lastHistorySd = now;
        saveHistory(true, false);
    }
    if (now - s_lastHistoryLfs >= kHistoryLfsEveryMs) {
        s_lastHistoryLfs = now;
        saveHistory(false, true);
    }
    if (now - s_lastRetention >= kRetentionEveryMs || !s_lastRetention) {
        if (time(nullptr) > 1700000000) {  // first run once the clock is set
            s_lastRetention = now;
            enforceRetention();
        }
    }
}

void refreshStats(uint32_t now) {
    if (!s_statsDirty && now - s_lastStats < kStatsEveryMs) return;
    s_statsDirty = false;
    s_lastStats = now;
    hmi::StorageInfo info;
    info.present = s_mounted;
    info.configLoaded = s_configLoaded;
    strlcpy(info.cfgRoom, s_cfgRoom, sizeof(info.cfgRoom));
    info.cfgWifi = s_cfgWifi;
    strlcpy(info.cfgMqtt, s_cfgMqtt, sizeof(info.cfgMqtt));
    info.cfgTasmota = s_cfgTasmota;
    if (s_mounted) {
        const uint8_t type = SD.cardType();
        strlcpy(info.cardType, type == CARD_SDHC ? "SDHC" : type == CARD_SD ? "SD" : type == CARD_MMC ? "MMC" : "?",
                sizeof(info.cardType));
        info.spiKHz = s_mountHz / 1000;
    }
    if (s_mounted) {
        const uint32_t t0 = millis();
        info.totalMB = SD.totalBytes() / (1024 * 1024);
        info.usedMB = SD.usedBytes() / (1024 * 1024);  // walks the FAT: can take seconds
        const uint32_t took = millis() - t0;
        if (took > 200) Serial.printf("SD: free-space scan took %lu ms\n", (unsigned long)took);
        if (SD.exists(kFirmware)) {
            File f = SD.open(kFirmware, FILE_READ);
            info.firmwareKB = f ? f.size() / 1024 : 0;
            if (f) f.close();
        }
        if (SD.exists(kBackupFw)) {
            File f = SD.open(kBackupFw, FILE_READ);
            info.backupKB = f ? f.size() / 1024 : 0;
            if (f) f.close();
            File v = SD.open(kBackupVer, FILE_READ);
            if (v) {
                const size_t n = v.read(reinterpret_cast<uint8_t*>(info.backupVer), sizeof(info.backupVer) - 1);
                info.backupVer[n] = 0;
                v.close();
            }
        }
    }
    info.configSource = s_configSource;
    info.logDays = s_logDays;
    if (s_lfs) {
        info.lfsTotalKB = LittleFS.totalBytes() / 1024;
        info.lfsUsedKB = LittleFS.usedBytes() / 1024;
    }
    hmi::setStorage(info);
}

void checkCard(uint32_t now) {
    if (now - s_lastCheck < 5000) return;
    s_lastCheck = now;
    if (s_mounted) {
        File root = SD.open("/");  // fails once the card is pulled
        if (root) {
            root.close();
            return;
        }
        SD.end();
        s_mounted = false;
        s_statsDirty = true;
        Serial.println("SD: card removed");
    } else if (mount()) {
        s_mounted = true;
        s_statsDirty = true;
        writeEvent("SD card inserted");
    }
}

void storageTask(void*) {
    Item it;
    for (;;) {
        const bool got = xQueueReceive(s_queue, &it, pdMS_TO_TICKS(1000)) == pdTRUE;
        // Browser requests are always answered (with an error when there is no
        // card) so the UI never waits forever; the rest needs a mounted card.
        if (got && (s_mounted || it.type == LIST || it.type == READ)) {
            switch (it.type) {
                case EVENT: writeEvent(it.text); break;
                case SENSORS: writeSensors(it.values); break;
                case FW_UPDATE: installFirmware(); break;
                case SCREENSHOT: saveScreenshot(); break;
                case LIST: listDir(it.text); break;
                case READ: readFile(it.text); break;
                case REMOVE: removePath(it.text); break;
                case TREE:
                    if (s_mounted) printTree("/", 0);
                    else Serial.println("SD: no card");
                    break;
                case WIPE: wipeCard(); break;
                case RESTORE: restoreFirmware(); break;
                case SAVE_HISTORY: saveHistory(true, true); break;
            }
        } else if (got && it.type == SAVE_HISTORY) {
            saveHistory(false, true);  // no card: flash copy only
        }
        const uint32_t now = millis();
        checkCard(now);
        periodic(now);
        refreshStats(now);
    }
}

}  // namespace

// Last lines of /logs/events.log into the PSRAM event ring (oldest first).
void preloadEvents() {
    File f = SD.open(kEventLog, FILE_READ);
    if (!f) return;
    constexpr size_t kTail = 24 * 1024;
    char* buf = static_cast<char*>(heap_caps_malloc(kTail + 1, MALLOC_CAP_SPIRAM));
    if (buf) {
        if (f.size() > kTail) f.seek(f.size() - kTail);
        const size_t n = f.read(reinterpret_cast<uint8_t*>(buf), kTail);
        buf[n] = 0;
        char* line = f.size() > kTail ? strchr(buf, '\n') : buf;  // skip a cut first line
        while (line && *line) {
            if (*line == '\n') line++;
            char* end = strchr(line, '\n');
            if (end) *end = 0;
            if (end && end > line && end[-1] == '\r') end[-1] = 0;
            if (*line) datalog::addEvent(line);
            line = end ? end + 1 : nullptr;
        }
        heap_caps_free(buf);
    }
    f.close();
}

bool begin() {
    s_mounted = mount();
    if (!s_mounted) Serial.println("SD: no card");
    s_lastCheck = millis();
    // LittleFS on the "spiffs" partition; formatted on first use.
    s_lfs = LittleFS.begin(true, "/lfs", 10, "spiffs");
    Serial.printf("LittleFS: %s", s_lfs ? "mounted" : "unavailable\n");
    if (s_lfs) Serial.printf(", %u / %u KB used\n", (unsigned)(LittleFS.usedBytes() / 1024), (unsigned)(LittleFS.totalBytes() / 1024));
    s_text = static_cast<char*>(heap_caps_malloc(kTextCap + 1, MALLOC_CAP_SPIRAM));
    // History: whichever copy is newer (each load() only accepts a higher seq).
    bool restored = s_mounted && datalog::load(SD, kHistorySd);
    restored = (s_lfs && datalog::load(LittleFS, kHistoryLfs)) || restored;
    if (restored) Serial.printf("history: restored (seq %lu)\n", (unsigned long)datalog::seq());
    if (s_mounted) preloadEvents();
    return s_mounted;
}

// Keep a copy of a valid SD /config.json in flash (only when it changed), so
// the panel still knows its Wi-Fi and devices without a card.
void backupConfig(const String& json) {
    if (!s_lfs) return;
    File old = LittleFS.open("/config.json", FILE_READ);
    const bool same = old && old.readString() == json;
    if (old) old.close();
    if (same) return;
    File f = LittleFS.open("/config.json", FILE_WRITE);
    if (!f) return;
    f.print(json);
    f.close();
    logEvent("config.json copied to flash (%u bytes)", json.length());
}

bool loadConfig(Config& out) {
    out = Config{};
    String json;
    if (s_mounted && SD.exists("/config.json")) {  // exists(): avoids a VFS error log
        File f = SD.open("/config.json", FILE_READ);
        if (f) {
            json = f.readString();
            f.close();
            s_configSource = 1;
        }
    }
    if (!s_configSource && s_lfs && LittleFS.exists("/config.json")) {
        File f = LittleFS.open("/config.json", FILE_READ);
        if (f) {
            json = f.readString();
            f.close();
            s_configSource = 2;
        }
    }
    if (!s_configSource) return false;
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, json);
    if (err) {
        logEvent("config.json invalid: %s", err.c_str());
        s_configSource = 0;
        return false;
    }
    if (s_configSource == 1) backupConfig(json);
    s_logDays = constrain(doc["logDays"] | 90, 1, 3650);
    for (JsonObject w : doc["wifi"].as<JsonArray>()) {
        if (out.wifiCount >= kMaxWifi) break;
        const char* ssid = w["ssid"] | "";
        if (!ssid[0]) continue;
        strlcpy(out.wifiSsid[out.wifiCount], ssid, sizeof(out.wifiSsid[0]));
        strlcpy(out.wifiPass[out.wifiCount], w["password"] | "", sizeof(out.wifiPass[0]));
        out.wifiCount++;
    }
    strlcpy(out.mqttHost, doc["mqtt"]["host"] | "", sizeof(out.mqttHost));
    out.mqttPort = doc["mqtt"]["port"] | 0;
    strlcpy(out.mqttUser, doc["mqtt"]["user"] | "", sizeof(out.mqttUser));
    strlcpy(out.mqttPass, doc["mqtt"]["password"] | "", sizeof(out.mqttPass));
    strlcpy(out.room, doc["room"] | "", sizeof(out.room));
    static const char* const kDefaultKeys[] = {"primary", "secondary", "third", "fourth"};
    for (JsonObject t : doc["tasmota"].as<JsonArray>()) {
        if (out.tasmotaCount >= hmi::kMaxDevices) break;
        const char* key = t["key"] | kDefaultKeys[out.tasmotaCount];  // gateway device name
        hmi::DeviceInfo& d = out.tasmota[out.tasmotaCount++];
        strlcpy(d.name, t["name"] | key, sizeof(d.name));
        strlcpy(d.key, key, sizeof(d.key));
        d.channels = constrain(t["channels"] | 1, 1, (int)hmi::kMaxChannels);
        uint8_t c = 0;
        for (const char* l : t["labels"].as<JsonArray>()) {
            if (c >= hmi::kMaxChannels) break;
            strlcpy(d.labels[c++], l ? l : "", sizeof(d.labels[0]));
        }
    }
    out.loaded = s_configLoaded = true;
    strlcpy(s_cfgRoom, out.room, sizeof(s_cfgRoom));
    s_cfgWifi = out.wifiCount;
    if (out.mqttHost[0]) snprintf(s_cfgMqtt, sizeof(s_cfgMqtt), "%s:%u", out.mqttHost, out.mqttPort ? out.mqttPort : 1883);
    s_cfgTasmota = out.tasmotaCount;
    if (!doc["tasmotaApi"].isNull()) {
        out.hasTasmotaApi = true;
        strlcpy(out.tasmotaApi, doc["tasmotaApi"] | "", sizeof(out.tasmotaApi));
    }
    logEvent("config.json loaded from %s: %u Wi-Fi, mqtt=%s", s_configSource == 1 ? "SD" : "flash", out.wifiCount,
             out.mqttHost[0] ? out.mqttHost : "-");
    return true;
}

void start() {
    if (s_queue) return;
    s_queue = xQueueCreate(24, sizeof(Item));
    // 12 KB: recursive tree/wipe, FAT + LittleFS, JPEG/PNG decoders (pools on the heap).
    xTaskCreatePinnedToCore(storageTask, "storage", 12 * 1024, nullptr, 1, nullptr, 0);
}

void logEvent(const char* fmt, ...) {
    Item it{EVENT, {}, {}};
    char ts[24];
    timestamp(ts, sizeof(ts));
    const int n = snprintf(it.text, sizeof(it.text), "%s ", ts);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(it.text + n, sizeof(it.text) - n, fmt, ap);
    va_end(ap);
    Serial.printf("[event] %s\n", it.text + n);
    datalog::addEvent(it.text);
    if (s_queue) enqueue(it);
    else if (s_mounted) writeEvent(it.text);  // during setup, before the task exists
}

void logSensors(float primary, float metric1, float metric2) {
    Item it{SENSORS, {primary, metric1, metric2}, {}};
    enqueue(it);
}

void requestFirmwareUpdate() {
    Item it{FW_UPDATE, {}, {}};
    enqueue(it);
}

void requestList(const char* dir) {
    Item it{LIST, {}, {}};
    strlcpy(it.text, dir, sizeof(it.text));
    enqueue(it);
}

void requestRead(const char* path) {
    Item it{READ, {}, {}};
    strlcpy(it.text, path, sizeof(it.text));
    enqueue(it);
}

void requestDelete(const char* path) {
    Item it{REMOVE, {}, {}};
    strlcpy(it.text, path, sizeof(it.text));
    enqueue(it);
}

void requestScreenshot() {
    Item it{SCREENSHOT, {}, {}};
    enqueue(it);
}

void requestRestore() {
    Item it{RESTORE, {}, {}};
    enqueue(it);
}

void requestHistorySave() {
    Item it{SAVE_HISTORY, {}, {}};
    enqueue(it);
}

void requestTree() {
    Item it{TREE, {}, {}};
    enqueue(it);
}

void requestWipe() {
    Item it{WIPE, {}, {}};
    enqueue(it);
}

}  // namespace storage
