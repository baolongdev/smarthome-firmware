#pragma once

#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <Update.h>

#include "config.h"

class WebServerManager {
public:
    WebServerManager() : server(80) {}
    
    void begin() {
        setupRoutes();
        server.begin();
        Serial.println("Web server started on port 80");
    }
    
    void handleOTAUpdate(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
        if (!index) {
            Serial.printf("OTA Update Start: %s\n", filename.c_str());
            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                Update.printError(Serial);
            }
        }
        
        if (Update.write(data, len) != len) {
            Update.printError(Serial);
        }
        
        if (final) {
            if (Update.end(true)) {
                Serial.printf("OTA Update Success: %u bytes\n", index + len);
                request->send(200, "text/plain", "OTA Update Successful! Rebooting...");
                ESP.restart();
            } else {
                Update.printError(Serial);
                request->send(500, "text/plain", "OTA Update Failed!");
            }
        }
    }

private:
    AsyncWebServer server;
    
    void setupRoutes() {
        // Root - device info
        server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
            String html = generateInfoPage();
            request->send(200, "text/html", html);
        });
        
        // OTA update page
        server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request) {
            String html = generateOTAPage();
            request->send(200, "text/html", html);
        });
        
        // OTA upload handler
        server.on("/update", HTTP_POST, 
            [](AsyncWebServerRequest *request) {}, 
            [this](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
                handleOTAUpdate(request, filename, index, data, len, final);
            });
        
        // Restart
        server.on("/restart", HTTP_POST, [](AsyncWebServerRequest *request) {
            request->send(200, "text/plain", "Restarting...");
            delay(100);
            ESP.restart();
        });
        
        // Heap info
        server.on("/heap", HTTP_GET, [](AsyncWebServerRequest *request) {
            String json = "{\"free\":";
            json += ESP.getFreeHeap();
            json += ",\"min\":";
            json += ESP.getMinFreeHeap();
            json += ",\"total\":";
            json += ESP.getHeapSize();
            json += "}";
            request->send(200, "application/json", json);
        });
        
        // WiFi scan
        server.on("/scan", HTTP_GET, [](AsyncWebServerRequest *request) {
            int n = WiFi.scanComplete();
            if (n == -2) {
                WiFi.scanNetworks(true);
                request->send(202, "text/plain", "Scanning...");
                return;
            }
            
            String json = "[";
            for (int i = 0; i < n; ++i) {
                if (i) json += ",";
                json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",";
                json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
                json += "\"encryption\":" + String(WiFi.encryptionType(i)) + "}";
            }
            json += "]";
            WiFi.scanDelete();
            request->send(200, "application/json", json);
        });
        
        // 404
        server.onNotFound([](AsyncWebServerRequest *request) {
            request->send(404, "text/plain", "Not Found");
        });
    }
    
    String generateInfoPage() {
        String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32-4848S040 Panel</title>
    <style>
        body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; max-width: 600px; margin: 2rem auto; padding: 1rem; background: #f5f5f5; }
        .card { background: white; border-radius: 12px; padding: 1.5rem; margin-bottom: 1rem; box-shadow: 0 2px 8px rgba(0,0,0,0.1); }
        h1 { color: #333; font-size: 1.5rem; margin-bottom: 1rem; }
        h2 { color: #666; font-size: 1.1rem; margin-top: 0; }
        .info-row { display: flex; justify-content: space-between; padding: 0.5rem 0; border-bottom: 1px solid #eee; }
        .info-label { color: #888; }
        .info-value { font-weight: 500; color: #333; }
        .status-ok { color: #22c55e; }
        .status-error { color: #ef4444; }
        .btn { display: inline-block; padding: 0.75rem 1.5rem; background: #3b82f6; color: white; border: none; border-radius: 8px; cursor: pointer; font-size: 1rem; text-decoration: none; margin: 0.5rem 0; }
        .btn:hover { background: #2563eb; }
        .btn-danger { background: #ef4444; }
        .btn-danger:hover { background: #dc2626; }
        .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(120px, 1fr)); gap: 1rem; }
    </style>
</head>
<body>
    <div class="card">
        <h1>ESP32-4848S040 Smart Panel</h1>
        <div class="info-row"><span class="info-label">Firmware</span><span class="info-value">)rawliteral";
        html += FIRMWARE_VERSION;
        html += R"rawliteral(</span></div>
        <div class="info-row"><span class="info-label">Device</span><span class="info-value">)rawliteral";
        html += DEVICE_NAME;
        html += R"rawliteral(</span></div>
        <div class="info-row"><span class="info-label">Uptime</span><span class="info-value" id="uptime">-</span></div>
        <div class="info-row"><span class="info-label">Free Heap</span><span class="info-value" id="heap">-</span></div>
        <div class="info-row"><span class="info-label">WiFi</span><span class="info-value" id="wifi">-</span></div>
        <div class="info-row"><span class="info-label">MQTT</span><span class="info-value" id="mqtt">-</span></div>
        <div class="info-row"><span class="info-label">IP Address</span><span class="info-value" id="ip">-</span></div>
    </div>
    
    <div class="card">
        <h2>Relay Status</h2>
        <div class="grid" id="relays">Loading...</div>
    </div>
    
    <div class="card">
        <h2>Actions</h2>
        <a href="/update" class="btn">OTA Update</a>
        <button class="btn btn-danger" onclick="restart()">Restart Device</button>
    </div>
    
    <script>
        async function fetchStatus() {
            try {
                const [info, heap] = await Promise.all([
                    fetch('/info').then(r => r.json()),
                    fetch('/heap').then(r => r.json())
                ]);
                
                document.getElementById('uptime').textContent = formatUptime(info.uptime);
                document.getElementById('heap').textContent = (heap.free / 1024).toFixed(1) + ' KB';
                document.getElementById('wifi').innerHTML = info.wifi_connected ? 
                    '<span class="status-ok">Connected (' + info.wifi_rssi + ' dBm)</span>' : 
                    '<span class="status-error">Disconnected</span>';
                document.getElementById('mqtt').innerHTML = info.mqtt_connected ? 
                    '<span class="status-ok">Connected</span>' : 
                    '<span class="status-error">Disconnected</span>';
                document.getElementById('ip').textContent = info.ip;
                
                const relaysDiv = document.getElementById('relays');
                relaysDiv.innerHTML = info.relays.map((r, i) => 
                    '<div style="padding: 0.5rem; background: ' + (r.state ? '#dcfce7' : '#fee2e2') + '; border-radius: 8px; text-align: center;">' +
                    '<strong>Relay ' + (i+1) + '</strong><br>' +
                    '<span style="font-size: 1.5rem;">' + (r.state ? 'ON' : 'OFF') + '</span>' +
                    '</div>'
                ).join('');
            } catch (e) {
                console.error('Status fetch failed:', e);
            }
        }
        
        function formatUptime(seconds) {
            const h = Math.floor(seconds / 3600);
            const m = Math.floor((seconds % 3600) / 60);
            const s = seconds % 60;
            return h + 'h ' + m + 'm ' + s + 's';
        }
        
        function restart() {
            if (confirm('Restart device?')) {
                fetch('/restart', { method: 'POST' });
            }
        }
        
        fetchStatus();
        setInterval(fetchStatus, 5000);
    </script>
</body>
</html>
        )rawliteral";
        return html;
    }
    
    String generateOTAPage() {
        return R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>OTA Update</title>
    <style>
        body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; max-width: 500px; margin: 3rem auto; padding: 1rem; background: #f5f5f5; }
        .card { background: white; border-radius: 12px; padding: 2rem; box-shadow: 0 2px 8px rgba(0,0,0,0.1); text-align: center; }
        h1 { color: #333; margin-bottom: 0.5rem; }
        p { color: #666; margin-bottom: 2rem; }
        .drop-zone { border: 2px dashed #3b82f6; border-radius: 12px; padding: 3rem; margin: 1rem 0; cursor: pointer; transition: all 0.2s; }
        .drop-zone:hover, .drop-zone.active { background: #eff6ff; border-color: #2563eb; }
        .drop-zone input { display: none; }
        .progress { height: 8px; background: #e5e7eb; border-radius: 4px; overflow: hidden; margin: 1rem 0; display: none; }
        .progress-bar { height: 100%; background: #3b82f6; width: 0%; transition: width 0.3s; }
        .status { margin-top: 1rem; min-height: 1.5rem; }
        .error { color: #ef4444; }
        .success { color: #22c55e; }
        a { color: #3b82f6; text-decoration: none; }
    </style>
</head>
<body>
    <div class="card">
        <h1>Firmware Update</h1>
        <p>Upload a compiled <code>.bin</code> file to update the device firmware.</p>
        
        <div class="drop-zone" id="dropZone">
            <input type="file" id="fileInput" accept=".bin">
            <p>Drag & drop or click to select <code>.bin</code> file</p>
            <p style="font-size: 0.875rem; color: #888;">Max size: 4MB</p>
        </div>
        
        <div class="progress" id="progress"><div class="progress-bar" id="progressBar"></div></div>
        <div class="status" id="status"></div>
        
        <p><a href="/">← Back to Info</a></p>
    </div>
    
    <script>
        const dropZone = document.getElementById('dropZone');
        const fileInput = document.getElementById('fileInput');
        const progress = document.getElementById('progress');
        const progressBar = document.getElementById('progressBar');
        const status = document.getElementById('status');
        
        dropZone.addEventListener('click', () => fileInput.click());
        dropZone.addEventListener('dragover', e => { e.preventDefault(); dropZone.classList.add('active'); });
        dropZone.addEventListener('dragleave', () => dropZone.classList.remove('active'));
        dropZone.addEventListener('drop', e => { e.preventDefault(); dropZone.classList.remove('active'); handleFile(e.dataTransfer.files[0]); });
        fileInput.addEventListener('change', e => handleFile(e.target.files[0]));
        
        function handleFile(file) {
            if (!file || !file.name.endsWith('.bin')) {
                showStatus('Please select a .bin file', true);
                return;
            }
            
            uploadFile(file);
        }
        
        async function uploadFile(file) {
            progress.style.display = 'block';
            showStatus('Uploading...');
            
            const xhr = new XMLHttpRequest();
            xhr.open('POST', '/update', true);
            
            xhr.upload.onprogress = e => {
                if (e.lengthComputable) {
                    const percent = (e.loaded / e.total * 100).toFixed(1);
                    progressBar.style.width = percent + '%';
                }
            };
            
            xhr.onload = () => {
                if (xhr.status === 200) {
                    showStatus('Update successful! Device rebooting...', false);
                    setTimeout(() => window.location.href = '/', 3000);
                } else {
                    showStatus('Update failed: ' + xhr.responseText, true);
                }
                progress.style.display = 'none';
            };
            
            xhr.onerror = () => {
                showStatus('Upload error', true);
                progress.style.display = 'none';
            };
            
            const formData = new FormData();
            formData.append('firmware', file);
            xhr.send(formData);
        }
        
        function showStatus(msg, isError) {
            status.textContent = msg;
            status.className = 'status ' + (isError ? 'error' : 'success');
        }
    </script>
</body>
</html>
        )rawliteral";
    }
};

#endif // WEB_SERVER_H