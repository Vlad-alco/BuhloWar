#ifndef CLOUD_MANAGER_H
#define CLOUD_MANAGER_H

#include <WiFiClientSecure.h>
#include <HTTPClient.h>

class CloudManager {
private:
    String serverUrl;
    String apiKey;
    unsigned long lastTelemetryMs = 0;
    unsigned long lastCommandCheckMs = 0;
    unsigned long lastSettingsCheckMs = 0;
    unsigned long telemetryIntervalMs = 2000;  // Send telemetry every 2 sec
    unsigned long commandCheckIntervalMs = 2000;  // Check commands every 2 sec
    unsigned long settingsCheckIntervalMs = 60000;  // Check settings every 60 sec (увеличено с 30)
    unsigned long settingsLastUpdate = 0;  // Timestamp of last settings update from cloud
    
    // === РОТАЦИЯ ЗАПРОСОВ: только один запрос за вызов ===
    // 0 = telemetry, 1 = commands, 2 = settings
    int nextRequestType = 0;
    // ====================================================
    
    // === ПЕРЕИСПОЛЬЗУЕМЫЙ HTTPS клиент (предотвращает фрагментацию кучи) ===
    WiFiClientSecure _client;
    bool _clientInitialized = false;
    // ========================================================================
    
    // Callback for processing commands from cloud
    typedef void (*CommandCallback)(const String& command, const String& params);
    CommandCallback onCommand = nullptr;
    
    // Callback for processing settings from cloud
    typedef void (*SettingsCallback)(const String& settingsJson);
    SettingsCallback onSettings = nullptr;
    
    void ensureClient() {
        if (!_clientInitialized) {
            _client.setInsecure();
            _clientInitialized = true;
        }
    }
    
public:
    CloudManager() : serverUrl(""), apiKey("") {}
    
    void begin(const String& url, const String& key) {
        serverUrl = url;
        apiKey = key;
        Serial.println("[Cloud] Initialized: " + serverUrl);
    }
    
    void onCommandReceived(CommandCallback callback) {
        onCommand = callback;
    }
    
    void onSettingsReceived(SettingsCallback callback) {
        onSettings = callback;
    }
    
    void setTelemetryInterval(unsigned long ms) {
        telemetryIntervalMs = ms;
    }
    
    void setCommandCheckInterval(unsigned long ms) {
        commandCheckIntervalMs = ms;
    }
    
    bool isConfigured() {
        return serverUrl.length() > 0 && apiKey.length() > 0;
    }
    
    // Send telemetry data to cloud
    bool sendTelemetry(const String& jsonData) {
        if (!isConfigured()) return false;
        
        ensureClient();
        HTTPClient http;
        
        String url = serverUrl + "?telemetry=1";
        
        http.begin(_client, url);
        http.setTimeout(2000);  // 2 сек таймаут (уменьшено с 5 сек)
        http.addHeader("Content-Type", "application/json");
        String authHeader = "Bearer " + apiKey;
        http.addHeader("Authorization", authHeader.c_str());
        
        int httpCode = http.POST(jsonData);
        
        if (httpCode == 200) {
            String response = http.getString();
            lastTelemetryMs = millis();
            http.end();
            return true;
        } else {
            Serial.printf("[Cloud] Telemetry failed: %d\n", httpCode);
            http.end();
            return false;
        }
    }
    
    // Check for pending commands from cloud
    bool checkCommands() {
        if (!isConfigured()) return false;
        
        ensureClient();
        HTTPClient http;
        
        String url = serverUrl + "?commands=1";
        
        http.begin(_client, url);
        http.setTimeout(2000);  // 2 сек таймаут
        String authHeader = "Bearer " + apiKey;
        http.addHeader("Authorization", authHeader.c_str());
        
        int httpCode = http.GET();
        
        if (httpCode == 200) {
            String response = http.getString();
            http.end();
            
            // Parse response if contains commands
            // Expected: {"commands": [{"command": "NAME", "params": {...}}, ...]}
            if (response.indexOf("\"commands\"") != -1) {
                parseCommands(response);
            }
            // Возвращаем true при любом успешном запросе (даже без команд)
            return true;
        } else {
            if (httpCode == -1) {
                Serial.println("[Cloud] Command check failed: Timeout/Connection error");
            } else {
                Serial.printf("[Cloud] Command check failed: %d\n", httpCode);
            }
            http.end();
        }
        return false;
    }
    
    // Check for settings update from cloud
    bool checkSettings() {
        if (!isConfigured() || onSettings == nullptr) return false;
        
        ensureClient();
        HTTPClient http;
        
        String url = serverUrl + "?settings=1";
        
        http.begin(_client, url);
        http.setTimeout(2000);  // 2 сек таймаут
        String authHeader = "Bearer " + apiKey;
        http.addHeader("Authorization", authHeader.c_str());
        
        int httpCode = http.GET();
        
        if (httpCode == 200) {
            String response = http.getString();
            http.end();
            
            // Parse settings from cloud response
            // Expected: {"settings": {...}, "settings_last_update": timestamp}
            if (response.indexOf("\"settings\"") != -1) {
                // Extract settings timestamp
                int tsPos = response.indexOf("\"settings_last_update\":");
                unsigned long cloudTs = 0;
                if (tsPos != -1) {
                    int start = tsPos + 22;
                    int end = response.indexOf(",", start);
                    if (end == -1) end = response.indexOf("}", start);
                    String tsStr = response.substring(start, end);
                    cloudTs = tsStr.toInt();
                }
                
                // Only update if settings are newer
                if (cloudTs > settingsLastUpdate) {
                    settingsLastUpdate = cloudTs;
                    Serial.printf("[Cloud] Settings updated from cloud (ts: %lu)\n", cloudTs);
                    
                    // Extract settings JSON and call callback
                    int settingsStart = response.indexOf("{\"settings\":");
                    if (settingsStart != -1) {
                        int jsonStart = response.indexOf("{", settingsStart);
                        // Find the settings object
                        int depth = 0;
                        int jsonEnd = jsonStart;
                        for (int i = jsonStart; i < response.length(); i++) {
                            if (response.charAt(i) == '{') depth++;
                            else if (response.charAt(i) == '}') {
                                depth--;
                                if (depth == 0) {
                                    jsonEnd = i + 1;
                                    break;
                                }
                            }
                        }
                        String settingsJson = response.substring(jsonStart, jsonEnd);
                        onSettings(settingsJson);
                    }
                }
            }
            // Возвращаем true при любом успешном запросе
            return true;
        } else {
            Serial.printf("[Cloud] Settings check failed: %d\n", httpCode);
            http.end();
        }
        return false;
    }
    
    void setSettingsLastUpdate(unsigned long ts) {
        settingsLastUpdate = ts;
    }
    
    // Call this periodically from main loop
    // ВАЖНО: Только ОДИН запрос за вызов для предотвращения долгих блокировок loop()
    // Максимальная блокировка: 2 сек (вместо 6 сек при трёх запросах одновременно)
    void update(const String& telemetryJson) {
        if (!isConfigured()) return;
        
        unsigned long now = millis();
        
        // Ротация запросов: проверяем только один тип за вызов
        switch (nextRequestType) {
            case 0: // Telemetry
                if (now - lastTelemetryMs >= telemetryIntervalMs) {
                    sendTelemetry(telemetryJson);
                    // lastTelemetryMs обновляется внутри sendTelemetry при успехе
                }
                nextRequestType = 1;
                break;
                
            case 1: // Commands
                if (now - lastCommandCheckMs >= commandCheckIntervalMs) {
                    if (checkCommands()) {
                        lastCommandCheckMs = now;
                    }
                }
                nextRequestType = 2;
                break;
                
            case 2: // Settings
                if (now - lastSettingsCheckMs >= settingsCheckIntervalMs) {
                    if (checkSettings()) {
                        lastSettingsCheckMs = now;
                    }
                }
                nextRequestType = 0; // Возвращаемся к telemetry
                break;
        }
    }
    
    // Get server connection status
    bool isConnected() {
        return WiFi.status() == WL_CONNECTED && isConfigured();
    }
    
private:
    void parseCommands(const String& response) {
        // Simple JSON parsing for commands
        // Looking for: {"commands":[{"command":"NAME","params":{...}},...]}
        
        int cmdsStart = response.indexOf("\"commands\"");
        if (cmdsStart == -1) return;
        
        int arrStart = response.indexOf("[", cmdsStart);
        if (arrStart == -1) return;
        
        // Find matching ] by counting brackets depth
        int depth = 1;
        int pos = arrStart + 1;
        while (pos < response.length() && depth > 0) {
            char c = response.charAt(pos);
            if (c == '[' || c == '{') depth++;
            else if (c == ']' || c == '}') depth--;
            pos++;
        }
        int arrEnd = pos - 1;
        
        String commands = response.substring(arrStart + 1, arrEnd);
        
        // Extract each command - find all {"command": patterns
        int cmdPos = 0;
        while (cmdPos < commands.length()) {
            int cmdStart = commands.indexOf("{\"command\":", cmdPos);
            if (cmdStart == -1) break;
            
            // Find command name
            int nameStart = commands.indexOf("\"", cmdStart + 11) + 1;
            int nameEnd = commands.indexOf("\"", nameStart);
            if (nameEnd == -1 || nameEnd <= nameStart) break;
            
            String cmdName = commands.substring(nameStart, nameEnd);
            
            // Call callback for each command
            if (onCommand) {
                onCommand(cmdName, "{}");
                Serial.printf("[Cloud] Command parsed: %s\n", cmdName.c_str());
            }
            
            cmdPos = nameEnd + 1;
        }
    }
};

#endif
