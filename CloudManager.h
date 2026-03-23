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
    unsigned long telemetryIntervalMs = 1000;
    unsigned long commandCheckIntervalMs = 1000;
    unsigned long settingsCheckIntervalMs = 30000;
    unsigned long settingsLastUpdate = 0;
    bool skipCertCheck = true;
    
    typedef void (*CommandCallback)(const String& command, const String& params);
    CommandCallback onCommand = nullptr;
    
    typedef void (*SettingsCallback)(const String& settingsJson);
    SettingsCallback onSettings = nullptr;
    
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
    
    bool sendTelemetry(const String& jsonData) {
        if (!isConfigured()) return false;
        
        HTTPClient http;
        WiFiClientSecure client;
        
        if (skipCertCheck) {
            client.setInsecure();
        }
        
        String url = serverUrl + "?telemetry=1";
        
        http.begin(client, url);
        http.setTimeout(5000);
        http.addHeader("Content-Type", "application/json");
        String authHeader = "Bearer " + apiKey;
        http.addHeader("Authorization", authHeader.c_str());
        
        int httpCode = http.POST(jsonData);
        
        if (httpCode == 200) {
            lastTelemetryMs = millis();
            http.end();
            return true;
        } else {
            Serial.printf("[Cloud] Telemetry failed: %d\n", httpCode);
            http.end();
            return false;
        }
    }
    
    bool checkCommands() {
        if (!isConfigured()) return false;
        
        HTTPClient http;
        WiFiClientSecure client;
        
        if (skipCertCheck) {
            client.setInsecure();
        }
        
        String url = serverUrl + "?commands=1";
        
        http.begin(client, url);
        http.setTimeout(5000);
        String authHeader = "Bearer " + apiKey;
        http.addHeader("Authorization", authHeader.c_str());
        
        int httpCode = http.GET();
        
        if (httpCode == 200) {
            String response = http.getString();
            http.end();
            
            if (response.indexOf("\"commands\"") != -1) {
                parseCommands(response);
                return true;
            }
        } else {
            Serial.printf("[Cloud] Command check failed: %d\n", httpCode);
            http.end();
        }
        return false;
    }
    
    bool checkSettings() {
        if (!isConfigured() || onSettings == nullptr) return false;
        
        HTTPClient http;
        WiFiClientSecure client;
        
        if (skipCertCheck) {
            client.setInsecure();
        }
        
        String url = serverUrl + "?settings=1";
        
        http.begin(client, url);
        http.setTimeout(5000);
        String authHeader = "Bearer " + apiKey;
        http.addHeader("Authorization", authHeader.c_str());
        
        int httpCode = http.GET();
        
        if (httpCode == 200) {
            String response = http.getString();
            http.end();
            
            if (response.indexOf("\"settings\"") != -1) {
                int tsPos = response.indexOf("\"settings_last_update\":");
                unsigned long cloudTs = 0;
                if (tsPos != -1) {
                    int start = tsPos + 22;
                    int end = response.indexOf(",", start);
                    if (end == -1) end = response.indexOf("}", start);
                    String tsStr = response.substring(start, end);
                    cloudTs = tsStr.toInt();
                }
                
                if (cloudTs > settingsLastUpdate) {
                    settingsLastUpdate = cloudTs;
                    Serial.printf("[Cloud] Settings updated from cloud (ts: %lu)\n", cloudTs);
                    
                    int settingsStart = response.indexOf("{\"settings\":");
                    if (settingsStart != -1) {
                        int jsonStart = response.indexOf("{", settingsStart);
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
                    return true;
                }
            }
        } else {
            Serial.printf("[Cloud] Settings check failed: %d\n", httpCode);
            http.end();
        }
        return false;
    }
    
    void setSettingsLastUpdate(unsigned long ts) {
        settingsLastUpdate = ts;
    }
    
    void update(const String& telemetryJson) {
        if (!isConfigured()) return;
        
        unsigned long now = millis();
        
        if (now - lastTelemetryMs >= telemetryIntervalMs) {
            sendTelemetry(telemetryJson);
        }
        
        if (now - lastCommandCheckMs >= commandCheckIntervalMs) {
            checkCommands();
        }
        
        if (now - lastSettingsCheckMs >= settingsCheckIntervalMs) {
            checkSettings();
        }
    }
    
    bool isConnected() {
        return WiFi.status() == WL_CONNECTED && isConfigured();
    }
    
private:
    void parseCommands(const String& response) {
        int cmdsStart = response.indexOf("\"commands\"");
        if (cmdsStart == -1) return;
        
        int arrStart = response.indexOf("[", cmdsStart);
        if (arrStart == -1) return;
        
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
        
        int cmdPos = 0;
        while (cmdPos < commands.length()) {
            int cmdStart = commands.indexOf("{\"command\":", cmdPos);
            if (cmdStart == -1) break;
            
            int nameStart = commands.indexOf("\"", cmdStart + 11) + 1;
            int nameEnd = commands.indexOf("\"", nameStart);
            if (nameEnd == -1 || nameEnd <= nameStart) break;
            
            String cmdName = commands.substring(nameStart, nameEnd);
            
            if (onCommand) {
                onCommand(cmdName, "{}");
                Serial.printf("[Cloud] Command parsed: %s\n", cmdName.c_str());
            }
            
            cmdPos = nameEnd + 1;
        }
    }
};

#endif
