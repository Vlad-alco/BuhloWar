#ifndef APP_NETWORK_H
#define APP_NETWORK_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <DNSServer.h>   // DNS сервер для AP режима (captive portal)
#include <SPI.h>
#include <SD.h>
#include "config.h"
#include "CloudManager.h"

// Предварительное объявление классов
class ProcessEngine;
class ConfigManager;

// Telegram ОТКЛЮЧЁН — библиотека не подключается
// #include <UniversalTelegramBot.h>

// === НАСТРОЙКИ ОЧЕРЕДИ СООБЩЕНИЙ ===
#define TG_QUEUE_SIZE 10        // Макс. сообщений в очереди
#define TG_SEND_TIMEOUT 5000    // Таймаут отправки (мс)
#define TG_RETRY_DELAY 30000    // Пауза после неудачи (мс)

// === РЕЖИМЫ СЕТИ ===
enum class NetworkMode {
    OFFLINE,    // X - нет WiFi, нет AP (только LCD)
    AP_MODE,    // A - точка доступа, Web работает
    STA_MODE    // W - подключено к роутеру, интернет есть
};

class AppNetwork {
public:
    void begin(int checkIntervalMinutes);
    void beginNetwork();  // Асинхронная инициализация сети (WiFi, Telegram, NTP)
    void update();
    void startTask();
    bool initSD();  // Публичный метод для ранней инициализации SD
    bool startAPMode();  // Запуск точки доступа
    void startWebServerEarly();  // Запуск WebServer сразу (в AP режиме)
    
    // Метод для связи с логикой
    void setEngine(ProcessEngine* engine, ConfigManager* cfgMgr);
    
    void sendMessage(const String& text);
    bool isOnline(); 
    NetworkMode getNetworkMode();  // Получить текущий режим сети
    char getNetworkSymbol();        // Символ для LCD (W/A/X)
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
    bool online = false;           // Интернет доступен (для Telegram/NTP)
    bool wifiConnected = false;    // WiFi подключен к роутеру
    bool sdInitialized = false;    // Флаг инициализации SD
    bool webServerStarted = false; // Флаг: WebServer уже запущен
    bool networkInitialized = false; // Флаг: сеть инициализирована
    NetworkMode networkMode = NetworkMode::OFFLINE;  // Текущий режим сети
    unsigned long lastLogSize = 0;  // Позиция лога для отправки в облако
    TaskHandle_t networkTaskHandle = nullptr;

    unsigned long lastCheckTime = 0;
    int checkIntervalMs = 300000; 
    unsigned long lastAlarmTgTime = 0;
    
    // --- Объекты ---
    WiFiClientSecure client;
    // UniversalTelegramBot* bot = nullptr;  // Telegram ОТКЛЮЧЁН
    WebServer* server = nullptr;
    DNSServer* dnsServer = nullptr;  // DNS сервер для AP режима

    // === TELEGRAM ОТКЛЮЧЁН ===
    // Очередь, bot и методы Telegram удалены для экономии RAM
    // =============================================

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

public:
    String buildTelemetryJson();
};

#endif