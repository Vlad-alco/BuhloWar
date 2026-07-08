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
    void setSystemReady(bool ready);  // Разрешить API handlers (после полной инициализации)
    
    void sendMessage(const String& text);
    bool isOnline(); 
    NetworkMode getNetworkMode();  // Получить текущий режим сети
    char getNetworkSymbol();        // Символ для LCD (W/A/X)
    bool didSwitchToSTA();          // true если произошёл переход AP→STA (сбрасывается после чтения)
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
    bool systemReady = false;         // Флаг: система полностью инициализирована (processEngine.begin() вызван)
    bool switchedToSTA = false;       // Флаг: переход AP→STA произошёл в фоне (для инициализации CloudManager)
    NetworkMode networkMode = NetworkMode::OFFLINE;  // Текущий режим сети
    unsigned long lastLogSize = 0;  // Позиция лога для отправки в облако
    TaskHandle_t networkTaskHandle = nullptr;

    // --- Состояние пошагового реконнекта (Причина №2) ---
    // Вместо единого блокирующего connectToWiFi() (до 9 сек),
    // реконнект разбит на 1 попытку за цикл update().
    int reconnectAttempt = 0;         // Текущий номер попытки (0-2 для каждой SSID)
    int reconnectSSID = 1;            // Какую сеть пробуем: 1 = ssid1, 2 = ssid2
    bool reconnecting = false;        // true — процесс реконнекта активен
    unsigned long wifiLostTime = 0;   // Когда обнаружена потеря WiFi (для 3-сек задержки)

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
    bool connectToWiFi();  // Используется только в beginNetwork() для начального подключения
    bool tryReconnectOnce();  // Одна попытка реконнекта (до 1.5 сек) для пошагового update()
    void startReconnect();  // Инициализация пошагового реконнекта
    void resetReconnect();   // Сброс состояния реконнекта при успехе/отказе
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