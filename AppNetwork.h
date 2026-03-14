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
#include "ProcessEngine.h"  // Для полного определения NotifyCategory

// Предварительное объявление классов
class ProcessEngine;
class ConfigManager;

// Для Telegram
#include <UniversalTelegramBot.h>

// === НАСТРОЙКИ ОЧЕРЕДИ СООБЩЕНИЙ ===
#define TG_QUEUE_SIZE 10        // Макс. сообщений в очереди
#define TG_SEND_TIMEOUT 5000    // Таймаут отправки (мс)
#define TG_RETRY_DELAY 30000    // Пауза после неудачи (мс)

// === VK API ===
#define VK_QUEUE_SIZE 5         // Макс. сообщений VK в очереди
#define VK_API_HOST "api.vk.com"
#define VK_API_PORT 443

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
    
    // === УВЕДОМЛЕНИЯ (Telegram + VK) ===
    void sendMessage(const String& text);  // Отправка в Telegram (существующий)
    void sendVKMessage(const String& text); // Отправка в VK
    void sendNotification(NotifyCategory category, const String& text); // Универсальный метод
    
    bool isOnline(); 
    NetworkMode getNetworkMode();  // Получить текущий режим сети
    char getNetworkSymbol();        // Символ для LCD (W/A/X)
    String getTimeStr();

private:
    // --- Настройки из файла ---
    String ssid1, pass1;
    String ssid2, pass2;
    String tgToken, tgChatId;
    String vkToken, vkPeerId;  // VK API
    
    // --- Флаги включения мессенджеров ---
    bool tgEnabled = true;
    bool vkEnabled = true;
    
    // NOTE: Настройки категорий (notifySystem, notifyDistillation, etc.)
    // теперь хранятся в SystemConfig (Preferences), а не здесь!
    
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
    DNSServer* dnsServer = nullptr;  // DNS сервер для AP режима

    // === АСИНХРОННАЯ ОЧЕРЕДЬ СООБЩЕНИЙ TELEGRAM ===
    struct TgMessage {
        String text;
        unsigned long timestamp;
    };
    TgMessage tgQueue[TG_QUEUE_SIZE];
    int tgQueueHead = 0;          // Индекс для добавления
    int tgQueueTail = 0;          // Индекс для извлечения
    int tgQueueCount = 0;         // Текущее кол-во сообщений
    bool tgSending = false;       // Флаг: идёт отправка
    unsigned long lastTgSendTime = 0;      // Время последней успешной отправки
    unsigned long lastTgFailTime = 0;      // Время последней неудачи
    int tgConsecutiveFails = 0;            // Счётчик подряд неудач
    
    // === АСИНХРОННАЯ ОЧЕРЕДЬ СООБЩЕНИЙ VK ===
    struct VkMessage {
        String text;
        unsigned long timestamp;
    };
    VkMessage vkQueue[VK_QUEUE_SIZE];
    int vkQueueHead = 0;
    int vkQueueTail = 0;
    int vkQueueCount = 0;
    bool vkSending = false;
    unsigned long lastVkSendTime = 0;
    unsigned long lastVkFailTime = 0;
    int vkConsecutiveFails = 0;
    
    // --- Методы очереди Telegram ---
    void queueMessage(const String& text);  // Добавить в очередь
    void processMessageQueue();              // Обработать очередь (неблокирующая)
    bool isTelegramReady();                  // Проверка готовности к отправке
    bool sendTelegramNow(const String& text); // Непосредственная отправка
    
    // --- Методы очереди VK ---
    void queueVKMessage(const String& text);  // Добавить в очередь VK
    void processVKQueue();                    // Обработать очередь VK
    bool sendVKNow(const String& text);       // Непосредственная отправка VK
    bool shouldSendCategory(NotifyCategory category); // Проверка категории
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