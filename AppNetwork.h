#ifndef APP_NETWORK_H
#define APP_NETWORK_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <SPI.h>
#include <SD.h>
#include "config.h"

// Предварительное объявление классов
class ProcessEngine;
class ConfigManager;

// Для Telegram
#include <UniversalTelegramBot.h>

class AppNetwork {
public:
    void begin(int checkIntervalMinutes);
    void update();
    void startTask();
    
    // Метод для связи с логикой
    void setEngine(ProcessEngine* engine, ConfigManager* cfgMgr);
    
    void sendMessage(const String& text);
    bool isOnline(); 
    String getTimeStr();

private:
    // --- Настройки из файла ---
    String ssid1, pass1;
    String ssid2, pass2;
    String tgToken, tgChatId;
    
    // --- Связь с системой ---
    ProcessEngine* processEngine = nullptr;
    ConfigManager* configManager = nullptr;
    
    // --- Состояние ---
    bool online = false;
    TaskHandle_t networkTaskHandle = nullptr;

    unsigned long lastCheckTime = 0;
    int checkIntervalMs = 300000; 
    unsigned long lastAlarmTgTime = 0;
    
    // --- Объекты ---
    WiFiClientSecure client;
    UniversalTelegramBot* bot = nullptr;
    WebServer* server = nullptr;

    // --- Внутренние методы ---
    bool loadConfigFromSD();
    bool connectToWiFi();
    void syncNTP();
    bool checkInternet();
    String parseLine(String line, String key);
    
    // --- Обработчики API ---
    void handleApiStatus();
    void handleApiCommand();
    void handleApiSettings();
    void handleCalcValve();
    void handleSaveProfile();
    void handleListProfiles();
    void handleLoadProfile();
    String buildCfgJson();
String transliterate(String input); // транслитерация для имени файла
};

#endif