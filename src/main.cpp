#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <LovyanGFX.hpp>
#include <OneButton.h>
#include <ArduinoOTA.h>
#include <Update.h>

#include "config.h"
#include "touch_gt911.h"
#include "lgfx_4848s040_reference.h"

LGFX display;
// Draw directly to the RGB panel.  The panel driver already owns a PSRAM-backed
// framebuffer; allocating a second 480x480 Sprite can fail and leave the LCD black.
LGFX& canvas = display;

// ===== Global Objects =====
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
#if BUTTONS_ENABLED
OneButton btn1(BUTTON_PIN_1, true);
OneButton btn2(BUTTON_PIN_2, true);
OneButton btn3(BUTTON_PIN_3, true);
OneButton btn4(BUTTON_PIN_4, true);
#endif
GT911Touch touch;

// ===== Device State =====
struct RelayState {
    bool state[RELAY_COUNT] = {false, false, false};
    uint32_t lastChange[RELAY_COUNT] = {0, 0, 0};
} relayState;

struct SystemInfo {
    uint32_t bootTime = 0;
    uint32_t lastMqttReconnect = 0;
    uint32_t lastWifiReconnect = 0;
    uint32_t mqttReconnectCount = 0;
    uint32_t wifiReconnectCount = 0;
    uint32_t freeHeap = 0;
    uint32_t minFreeHeap = UINT32_MAX;
    float cpuTemp = 0;
} sysInfo;

// ===== Forward Declarations =====
void setupDisplay();
void setupWiFi();
void setupMQTT();
void setupButtons();
void setupRelays();
void setupTouch();
void setupOTA();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void publishState();
void publishAvailability(bool available);
void handleRelayCommand(int index, bool state);
void handleRelayToggle(int index);
void updateDisplay();
void drawUI();
void checkConnections();
void printSystemInfo();
void handleTouch();

// ===== Button Callbacks =====
void onBtn1Click() { handleRelayToggle(0); }
void onBtn2Click() { handleRelayToggle(1); }
void onBtn3Click() { handleRelayToggle(2); }
void onBtn4Click() { handleRelayToggle(3); }

void setup() {
    Serial.begin(SERIAL_BAUDRATE);
    delay(100);
    
    Serial.println("\n\n=== ESP32-4848S040 Smart Home Panel ===");
    Serial.printf("Firmware: %s\n", FIRMWARE_VERSION);
    Serial.printf("Hardware: %s\n", HARDWARE_VERSION);
    Serial.printf("Device: %s\n", DEVICE_NAME);
    Serial.flush();
    
    sysInfo.bootTime = millis();
    sysInfo.freeHeap = ESP.getFreeHeap();
    sysInfo.minFreeHeap = sysInfo.freeHeap;
    
    setupDisplay();
#if TOUCH_TEST_MODE
    display.fillScreen(TFT_GREEN);
    display.setTextColor(TFT_BLACK, TFT_GREEN);
    display.setTextSize(3);
    display.drawCenterString("TOUCH TEST", DISPLAY_WIDTH / 2, 210);
    display.drawCenterString("Tap LCD", DISPLAY_WIDTH / 2, 255);
    Serial.println("Touch test mode: LCD GREEN, touch coordinates enabled");
    Serial.flush();
    return;
#endif
#if MINIMAL_LCD_UI
    Serial.println("Minimal LCD UI mode: skipping relay/touch/network startup");
    return;
#endif
    setupRelays();
    setupTouch();
    setupButtons();
    setupWiFi();
    setupMQTT();
    setupOTA();
    
    publishAvailability(true);
    publishState();
    
    Serial.println("=== Setup Complete ===");
    printSystemInfo();
}

void loop() {
#if TOUCH_TEST_MODE
    static bool wasDown = false;
    static uint32_t lastTouchLog = 0;
    uint16_t x = 0;
    uint16_t y = 0;
    bool down = display.getTouch(&x, &y);
    if (down && (!wasDown || millis() - lastTouchLog > 250)) {
        Serial.printf("Touch DOWN: X=%u Y=%u\n", x, y);
        Serial.flush();
        lastTouchLog = millis();
    } else if (!down && wasDown) {
        Serial.println("Touch UP");
        Serial.flush();
    }
    wasDown = down;
    delay(20);
    return;
#endif

#if MINIMAL_LCD_UI
    static uint32_t lastMinimalDraw = 0;
    if (millis() - lastMinimalDraw > 1000) {
        updateDisplay();
        lastMinimalDraw = millis();
    }
    delay(10);
    return;
#endif

#if DISPLAY_COLOR_TEST
    // Keep the diagnostic visible long enough to identify RGB/timing problems.
    static uint32_t lastColorChange = 0;
    static uint8_t colorIndex = 0;
    static const uint16_t colors[] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE};
    static const char* names[] = {"RED", "GREEN", "BLUE", "WHITE"};
    if (millis() - lastColorChange > 2500) {
        display.fillScreen(colors[colorIndex]);
        display.setTextColor(colorIndex == 3 ? TFT_BLACK : TFT_WHITE, colors[colorIndex]);
        display.setTextSize(4);
        display.drawCenterString(names[colorIndex], DISPLAY_WIDTH / 2, DISPLAY_HEIGHT / 2 - 20);
        colorIndex = (colorIndex + 1) % 4;
        lastColorChange = millis();
    }
    delay(10);
    return;
#endif

#if BUTTONS_ENABLED
    // Handle button events
    btn1.tick();
    btn2.tick();
    btn3.tick();
    btn4.tick();
#endif
    
    // Handle touch
    handleTouch();
    
    // Handle OTA
    ArduinoOTA.handle();
    
    // Maintain connections
    checkConnections();
    
    // MQTT loop
    mqttClient.loop();
    
    // Update display frequently for testing (every 500ms)
    static uint32_t lastDisplayUpdate = 0;
    if (millis() - lastDisplayUpdate > 500) {
        updateDisplay();
        lastDisplayUpdate = millis();
    }
    
    // Debug: print touch status every 5 seconds
    static uint32_t lastDebugPrint = 0;
    if (millis() - lastDebugPrint > 5000) {
        Serial.printf("Loop: heap=%dKB, WiFi=%s, MQTT=%s, TouchPts=%d\n", 
                      ESP.getFreeHeap() / 1024,
                      WiFi.status() == WL_CONNECTED ? "OK" : "FAIL",
                      mqttClient.connected() ? "OK" : "FAIL",
                      touch.getTouchCount());
        lastDebugPrint = millis();
    }
    
    // System monitoring
    static uint32_t lastHeapCheck = 0;
    if (millis() - lastHeapCheck > HEAP_MONITOR_INTERVAL) {
        sysInfo.freeHeap = ESP.getFreeHeap();
        if (sysInfo.freeHeap < sysInfo.minFreeHeap) {
            sysInfo.minFreeHeap = sysInfo.freeHeap;
        }
        lastHeapCheck = millis();
    }
    
    delay(10);
}

// ===== Setup Functions =====
void setupDisplay() {
    Serial.println("=== Display Setup Start ===");
    Serial.printf("Config: W=%d H=%d ROT=%d BL=%d\n", DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_ROTATION, DISPLAY_BRIGHTNESS);
    Serial.printf("Pins: PCLK=%d HSYNC=%d VSYNC=%d DE=%d CS=%d SCLK=%d MOSI=%d BL=%d\n",
                  PIN_DISP_PCLK, PIN_DISP_HSYNC, PIN_DISP_VSYNC, PIN_DISP_DE,
                  PIN_DISP_CS, PIN_DISP_SCLK, PIN_DISP_MOSI, PIN_DISP_BL);
    
    // Reset display hardware
    Serial.println("Resetting display hardware...");
    if (PIN_DISP_RST >= 0) {
        pinMode(PIN_DISP_RST, OUTPUT);
        digitalWrite(PIN_DISP_RST, LOW);
        delay(100);
        digitalWrite(PIN_DISP_RST, HIGH);
        delay(100);
    }
    Serial.println("Reset done");
    
    // Initialize display
    Serial.println("Calling display.begin()...");
    bool result = display.begin();
    Serial.printf("display.begin() returned: %s\n", result ? "true" : "false");
    
    if (!result) {
        Serial.println("!!! Display initialization FAILED !!!");
        Serial.printf("Display width: %d, height: %d\n", display.width(), display.height());
        return;
    }
    
    Serial.println("Display begin() succeeded");
    Serial.printf("Display size: %dx%d\n", display.width(), display.height());
    Serial.printf("Display rotation: %d\n", display.getRotation());
    
    // Keep the exact post-begin path from the known-good reference firmware.
    // Rotation 0 and the default PWM brightness are already configured by the panel.
    display.fillScreen(TFT_RED);  // Red screen = init working
    delay(500);
    display.fillScreen(TFT_GREEN); // Green screen = LCD test; keep visible
    delay(2000);
    display.fillScreen(TFT_BLUE);  // Blue screen = next step
    delay(500);
    display.fillScreen(TFT_BLACK);
    
    Serial.println("Direct LCD drawing enabled (no extra Sprite)");
    Serial.println("=== Display Setup Complete ===");
    drawUI();
}

void setupWiFi() {
    Serial.println("Connecting to WiFi...");
    WiFi.setHostname(WIFI_HOSTNAME);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
        delay(500);
        Serial.print(".");
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println();
        Serial.printf("WiFi connected: %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
    } else {
        Serial.println("\nWiFi connection failed!");
    }
}

void setupMQTT() {
    mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setBufferSize(2048);
    mqttClient.setKeepAlive(MQTT_KEEPALIVE);
}

void setupButtons() {
#if BUTTONS_ENABLED
    btn1.attachClick(onBtn1Click);
    btn2.attachClick(onBtn2Click);
    btn3.attachClick(onBtn3Click);
    btn4.attachClick(onBtn4Click);
    
    btn1.setDebounceMs(50);
    btn2.setDebounceMs(50);
    btn3.setDebounceMs(50);
    btn4.setDebounceMs(50);
#endif
    Serial.println("Buttons initialized");
}

void setupRelays() {
    const uint8_t relayPins[RELAY_COUNT] = {RELAY_PIN_1, RELAY_PIN_2, RELAY_PIN_3};
    
    for (int i = 0; i < RELAY_COUNT; i++) {
        pinMode(relayPins[i], OUTPUT);
        digitalWrite(relayPins[i], LOW); // Active HIGH relay, OFF = LOW
        relayState.state[i] = false;
        relayState.lastChange[i] = millis();
    }
    
    Serial.printf("Relays initialized (%d channels)\n", RELAY_COUNT);
}

// ===== MQTT Functions =====
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String message;
    for (unsigned int i = 0; i < length; i++) {
        message += (char)payload[i];
    }
    
    Serial.printf("MQTT Received [%s]: %s\n", topic, message.c_str());
    
    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, message);
    if (error) {
        Serial.printf("JSON parse failed: %s\n", error.c_str());
        return;
    }
    
    String cmdTopic = String(MQTT_TOPIC_COMMAND);
    if (String(topic) == cmdTopic) {
        // Handle relay commands
        if (doc.containsKey("relay")) {
            int index = doc["relay"];
            bool state = doc["state"] | false;
            handleRelayCommand(index, state);
        }
        // Handle all relays
        else if (doc.containsKey("relays")) {
            JsonArray relays = doc["relays"];
            for (int i = 0; i < relays.size() && i < RELAY_COUNT; i++) {
                handleRelayCommand(i, relays[i]);
            }
        }
        // Handle brightness
        else if (doc.containsKey("brightness")) {
            uint8_t brightness = doc["brightness"];
            ledcWrite(0, brightness);
        }
    }
}

void publishState() {
    StaticJsonDocument<512> doc;
    doc["device"] = DEVICE_NAME;
    doc["type"] = DEVICE_TYPE;
    doc["fw_version"] = FIRMWARE_VERSION;
    doc["hw_version"] = HARDWARE_VERSION;
    doc["uptime"] = (millis() - sysInfo.bootTime) / 1000;
    doc["free_heap"] = sysInfo.freeHeap;
    doc["min_free_heap"] = sysInfo.minFreeHeap;
    doc["wifi_rssi"] = WiFi.RSSI();
    doc["ip"] = WiFi.localIP().toString();
    
    JsonArray relays = doc.createNestedArray("relays");
    for (int i = 0; i < RELAY_COUNT; i++) {
        JsonObject r = relays.createNestedObject();
        r["index"] = i;
        r["state"] = relayState.state[i];
        r["last_change"] = (millis() - relayState.lastChange[i]) / 1000;
    }
    
    String output;
    serializeJson(doc, output);
    mqttClient.publish(MQTT_TOPIC_STATE, output.c_str(), true);
}

void publishAvailability(bool available) {
    mqttClient.publish(MQTT_TOPIC_AVAILABILITY, available ? "online" : "offline", true);
}

// ===== Relay Control =====
void handleRelayCommand(int index, bool state) {
    if (index < 0 || index >= RELAY_COUNT) return;
    
    const uint8_t relayPins[RELAY_COUNT] = {RELAY_PIN_1, RELAY_PIN_2, RELAY_PIN_3};
    
    if (relayState.state[index] != state) {
        relayState.state[index] = state;
        relayState.lastChange[index] = millis();
        digitalWrite(relayPins[index], state ? HIGH : LOW);
        
        Serial.printf("Relay %d: %s\n", index + 1, state ? "ON" : "OFF");
        publishState();
    }
}

void handleRelayToggle(int index) {
    if (index < 0 || index >= RELAY_COUNT) return;
    handleRelayCommand(index, !relayState.state[index]);
}

// ===== Display Functions =====
void updateDisplay() {
    drawUI();
}

void drawUI() {
    constexpr int MARGIN = 12;
    constexpr int GAP = 6;
    constexpr int CARD_Y = 52;
    constexpr int CARD_W = (DISPLAY_WIDTH - (MARGIN * 2) - GAP * 2) / 3;
    constexpr int CARD_H = 115;
    const uint16_t BG = TFT_BLACK;
    const uint16_t CARD_OFF = 0x2104;
    const uint16_t CARD_ON = TFT_GREEN;
    const uint16_t TEXT = TFT_WHITE;
    const uint16_t MUTED = 0x8410;

    canvas.fillScreen(BG);

    // Compact EspControl-inspired clock/status header.
    canvas.setTextSize(2);
    canvas.setTextColor(TEXT);
    canvas.drawString("Smart Home", MARGIN, 10);
    canvas.setTextSize(1);
    canvas.setTextColor(MUTED);
    canvas.drawString(WiFi.status() == WL_CONNECTED ? "ONLINE" : "OFFLINE", MARGIN, 34);
    canvas.fillCircle(112, 38, 4, WiFi.status() == WL_CONNECTED ? CARD_ON : TFT_RED);
    canvas.drawString("WiFi", 120, 34);
    canvas.fillCircle(170, 38, 4, mqttClient.connected() ? CARD_ON : TFT_RED);
    canvas.drawString("MQTT", 178, 34);
    canvas.setTextColor(MUTED);
    canvas.drawRightString("12:00", DISPLAY_WIDTH - MARGIN, 16);

    auto drawTile = [&](int x, int y, const char* title, const char* value, uint16_t fill, uint16_t titleColor) {
        canvas.fillRoundRect(x, y, CARD_W, CARD_H, 14, fill);
        if (fill == CARD_OFF || fill == 0x2104) canvas.drawRoundRect(x, y, CARD_W, CARD_H, 14, 0x4208);
        canvas.setTextColor(titleColor);
        canvas.setTextSize(1);
        canvas.drawString(title, x + 10, y + 10);
        canvas.setTextSize(2);
        canvas.drawCenterString(value, x + CARD_W / 2, y + 50);
    };

    auto drawRelayCard = [&](int index, int x, int y, const char* label, const char* icon) {
        bool on = relayState.state[index];
        uint16_t fill = on ? CARD_ON : CARD_OFF;
        canvas.fillRoundRect(x, y, CARD_W, CARD_H, 14, fill);
        if (!on) canvas.drawRoundRect(x, y, CARD_W, CARD_H, 14, 0x4208);
        canvas.setTextColor(on ? TFT_BLACK : TEXT);
        canvas.setTextSize(2);
        canvas.drawCenterString(icon, x + CARD_W / 2, y + 28);
        canvas.setTextSize(1);
        canvas.drawCenterString(label, x + CARD_W / 2, y + 63);
        canvas.setTextSize(1);
        canvas.drawCenterString(on ? "ON" : "OFF", x + CARD_W / 2, y + 88);
    };

    const int x0 = MARGIN;
    const int x1 = MARGIN + CARD_W + GAP;
    const int x2 = MARGIN + (CARD_W + GAP) * 2;
    const int y0 = CARD_Y;
    const int y1 = CARD_Y + CARD_H + GAP;
    const int y2 = CARD_Y + (CARD_H + GAP) * 2;

    drawRelayCard(0, x0, y0, "Living Room", "L1");
    drawRelayCard(1, x1, y0, "HVAC Unit", "L2");
    drawRelayCard(2, x2, y0, "Wall Plugs", "L3");
    drawTile(x0, y1, "SYSTEM", WiFi.status() == WL_CONNECTED ? "ONLINE" : "OFFLINE", 0x2104, TEXT);
    drawTile(x1, y1, "NETWORK", WiFi.localIP().toString().c_str(), 0x2104, TEXT);
    drawTile(x2, y1, "TOUCH", "GT911", 0x2104, TEXT);
    drawTile(x0, y2, "ACTIVE", String((relayState.state[0] ? 1 : 0) + (relayState.state[1] ? 1 : 0) + (relayState.state[2] ? 1 : 0)).c_str(), 0x2104, TEXT);
    drawTile(x1, y2, "HEAP", (String(ESP.getFreeHeap() / 1024) + " KB").c_str(), 0x2104, TEXT);
    drawTile(x2, y2, "FIRMWARE", FIRMWARE_VERSION, 0x2104, TEXT);

    canvas.setTextColor(MUTED);
    canvas.setTextSize(1);
    canvas.drawString("Tap relay cards to control", MARGIN, DISPLAY_HEIGHT - 8);
}

void checkConnections() {
    // WiFi reconnection
    if (WiFi.status() != WL_CONNECTED) {
        if (millis() - sysInfo.lastWifiReconnect > WIFI_RECONNECT_INTERVAL) {
            Serial.println("WiFi disconnected, reconnecting...");
            WiFi.reconnect();
            sysInfo.lastWifiReconnect = millis();
            sysInfo.wifiReconnectCount++;
        }
        return;
    }
    
    // MQTT reconnection
    if (!mqttClient.connected()) {
        if (millis() - sysInfo.lastMqttReconnect > MQTT_RECONNECT_INTERVAL) {
            Serial.println("MQTT disconnected, reconnecting...");
            String clientId = String(MQTT_CLIENT_ID) + "-" + String(random(0xffff), HEX);
            
            if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD,
                                   MQTT_TOPIC_AVAILABILITY, 1, true, "offline")) {
                Serial.println("MQTT connected");
                mqttClient.subscribe(MQTT_TOPIC_COMMAND);
                mqttClient.subscribe(MQTT_TOPIC_CONFIG);
                publishAvailability(true);
                publishState();
            } else {
                Serial.printf("MQTT connect failed, rc=%d\n", mqttClient.state());
            }
            sysInfo.lastMqttReconnect = millis();
            sysInfo.mqttReconnectCount++;
        }
    }
}

void printSystemInfo() {
    Serial.printf("Free Heap: %d bytes\n", ESP.getFreeHeap());
    Serial.printf("Min Free Heap: %d bytes\n", ESP.getMinFreeHeap());
    Serial.printf("Chip Model: %s Rev %d\n", ESP.getChipModel(), ESP.getChipRevision());
    Serial.printf("CPU Freq: %d MHz\n", ESP.getCpuFreqMHz());
    Serial.printf("Flash Size: %d bytes\n", ESP.getFlashChipSize());
    Serial.printf("PSRAM Size: %d bytes\n", ESP.getPsramSize());
}

void setupTouch() {
    Serial.println("=== Touch Setup Start ===");
    Serial.printf("Touch pins: SDA=%d SCL=%d\n", PIN_TOUCH_SDA, PIN_TOUCH_SCL);
    
    Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL);
    Serial.println("I2C started");
    
    // Scan I2C bus
    Serial.println("Scanning I2C bus...");
    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.printf("  Found device at 0x%02X\n", addr);
        }
    }
    
    if (touch.begin()) {
        Serial.println("Touch initialized successfully");
    } else {
        Serial.println("!!! Touch initialization FAILED !!!");
    }
    Serial.println("=== Touch Setup Complete ===");
}

void setupOTA() {
    Serial.println("Starting OTA...");
    ArduinoOTA.setHostname(WIFI_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.setPort(OTA_PORT);
    
    ArduinoOTA.onStart([]() {
        String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
        Serial.println("OTA Start: " + type);
    });
    
    ArduinoOTA.onEnd([]() {
        Serial.println("\nOTA End");
    });
    
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("OTA Progress: %u%%\r", (progress / (total / 100)));
    });
    
    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("OTA Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
        else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });
    
    ArduinoOTA.begin();
    Serial.println("OTA ready");
}

void handleTouch() {
    static uint32_t lastTouchRead = 0;
    static bool touchWasDown = false;
    if (millis() - lastTouchRead < 50) return;  // 20Hz max
    lastTouchRead = millis();
    
    bool down = touch.read();
    if (!down) {
        touchWasDown = false;
        return;
    }

    // Trigger once on touch-down, not continuously while the finger remains down.
    if (touchWasDown) return;
    int16_t x, y;
    if (!touch.getFirstTouch(x, y)) return;
    touchWasDown = true;

    constexpr int MARGIN = 12;
    constexpr int GAP = 6;
    constexpr int CARD_Y = 52;
    constexpr int CARD_W = (DISPLAY_WIDTH - (MARGIN * 2) - GAP * 2) / 3;
    constexpr int CARD_H = 115;
    const int cardX[3] = {MARGIN, MARGIN + CARD_W + GAP, MARGIN + (CARD_W + GAP) * 2};
    const int cardY[3] = {CARD_Y, CARD_Y, CARD_Y};
    for (int i = 0; i < RELAY_COUNT; i++) {
        if (x >= cardX[i] && x < cardX[i] + CARD_W && y >= cardY[i] && y < cardY[i] + CARD_H) {
            handleRelayToggle(i);
            Serial.printf("Touch: X=%d Y=%d -> Relay %d\n", x, y, i + 1);
            return;
        }
    }
    Serial.printf("Touch: X=%d Y=%d -> status tile\n", x, y);
}
