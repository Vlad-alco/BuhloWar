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

// === НАСТРОЙКИ ОЧЕРЕДИ СООБЩЕНИЙ ===
#define TG_QUEUE_SIZE 10        // Макс. сообщений в очереди
#define TG_SEND_TIMEOUT 5000    // Таймаут отправки (мс)
#define TG_RETRY_DELAY 30000    // Пауза после неудачи (мс)
#define TG_MAX_RETRIES 3        // Макс. попыток на одно сообщение

// === РЕЖИМЫ СЕТИ ===
enum class NetworkMode {
    OFFLINE,    // X - нет WiFi, нет AP (только LCD)
    AP_MODE,    // A - точка доступа, Web работает
    STA_MODE    // W - подключено к роутеру, интернет есть
};

class AppNetwork {
public:
    void begin(int checkIntervalMinutes);
    void update();
    void startTask();
    bool initSD();  // Публичный метод для ранней инициализации SD
    bool startAPMode();  // Запуск точки доступа
    
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
    NetworkMode networkMode = NetworkMode::OFFLINE;  // Текущий режим сети
    TaskHandle_t networkTaskHandle = nullptr;

    unsigned long lastCheckTime = 0;
    int checkIntervalMs = 300000; 
    unsigned long lastAlarmTgTime = 0;
    
    // --- Объекты ---
    WiFiClientSecure client;
    UniversalTelegramBot* bot = nullptr;
    WebServer* server = nullptr;

    // === АСИНХРОННАЯ ОЧЕРЕДЬ СООБЩЕНИЙ TELEGRAM ===
    struct TgMessage {
        String text;
        unsigned long timestamp;
        int retryCount = 0;  // Счётчик попыток отправки
    };
    TgMessage tgQueue[TG_QUEUE_SIZE];
    int tgQueueHead = 0;          // Индекс для добавления
    int tgQueueTail = 0;          // Индекс для извлечения
    int tgQueueCount = 0;         // Текущее кол-во сообщений
    bool tgSending = false;       // Флаг: идёт отправка
    unsigned long lastTgSendTime = 0;      // Время последней успешной отправки
    unsigned long lastTgFailTime = 0;      // Время последней неудачи
    int tgConsecutiveFails = 0;            // Счётчик подряд неудач
    
    // --- Методы очереди ---
    void queueMessage(const String& text);  // Добавить в очередь
    void processMessageQueue();              // Обработать очередь (неблокирующая)
    bool isTelegramReady();                  // Проверка готовности к отправке
    bool sendTelegramNow(const String& text); // Непосредственная отправка
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
};

#endif