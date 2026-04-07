#include "AppNetwork.h"
#include "config.h"
// Подключаем ProcessEngine, чтобы брать из него данные
#include "ProcessEngine.h" 
#include "preferences.h"
#include "SDLogger.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
// Подключаем внешний объект логера
extern SDLogger logger;
// Внешний мьютекс для SD карты (создаётся в .ino)
extern SemaphoreHandle_t sdMutex;
// Внешняя очередь команд (создаётся в .ino)
extern QueueHandle_t commandQueue;

// === ИНИЦИАЛИЗАЦИЯ SD КАРТЫ (отдельная функция) ===
bool AppNetwork::initSD() {
    SPI.begin(SD_SPI_SCK, SD_SPI_MISO, SD_SPI_MOSI, SD_SPI_CS);
    Serial.println("[NetMgr] Mounting SD Card...");

    if(!SD.begin(SD_SPI_CS)) {
        Serial.println("[NetMgr] SD Card Mount FAILED!");
        sdInitialized = false;
        return false;
    }

    Serial.println("[NetMgr] SD Card Mounted OK.");
    sdInitialized = true;
    return true;
}

// === РАННИЙ ЗАПУСК WEBSERVER (В AP РЕЖИМЕ) ===
// Вызывается сразу после initSD() в setup()
// WebServer доступен по 192.168.4.1 даже до подключения к WiFi
void AppNetwork::startWebServerEarly() {
    if (webServerStarted) return;  // Уже запущен
    
    // Запускаем AP режим для раннего доступа к Web
    Serial.println("[NetMgr] Starting AP mode for early Web access...");
    
    IPAddress local_IP(192, 168, 4, 1);
    IPAddress gateway(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);
    
    if (!WiFi.softAPConfig(local_IP, gateway, subnet)) {
        Serial.println("[NetMgr] AP Config FAILED!");
        return;
    }
    
    if (!WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL)) {
        Serial.println("[NetMgr] AP Start FAILED!");
        return;
    }
    
    // Запускаем DNS сервер (Captive Portal)
    if (dnsServer) {
        dnsServer->stop();
        delete dnsServer;
    }
    dnsServer = new DNSServer();
    dnsServer->start(53, "*", WiFi.softAPIP());
    
    // Создаём и настраиваем WebServer
    if (server) {
        server->stop();
        delete server;
    }
    server = new WebServer(80);
    
    // Регистрируем обработчики API
    server->on("/api/status", [this]() { handleApiStatus(); });
    server->on("/api/command", HTTP_POST, [this]() { handleApiCommand(); });
    server->on("/api/settings", HTTP_POST, [this]() { handleApiSettings(); });
    server->on("/api/calcvalve", HTTP_POST, [this]() { handleCalcValve(); });
    server->on("/api/saveprofile", HTTP_POST, [this]() { handleSaveProfile(); });
    server->on("/api/listprofiles", HTTP_GET, [this]() { handleListProfiles(); });
    server->on("/api/loadprofile", HTTP_POST, [this]() { handleLoadProfile(); });
    server->on("/api/logs", HTTP_GET, [this]() {
        String logContent = logger.readLastLog();
        server->send(200, "text/plain", logContent);
    });

    // Главная страница
    server->on("/", HTTP_GET, [this]() {
        if (sdMutex) xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000));
        
        // SD.open с retry
        File file;
        for (int retry = 0; retry < 3; retry++) {
            file = SD.open("/www/index.html", "r");
            if (file) break;
            delay(10);
        }
        
        if (file) {
            server->streamFile(file, "text/html");
            file.close();
        } else {
            server->send(404, "text/plain", "File Not Found: index.html");
        }
        
        if (sdMutex) xSemaphoreGive(sdMutex);
    });

    // Статика с кешированием
    server->serveStatic("/", SD, "/www/", "max-age=86400");

    server->onNotFound([this]() {
        server->send(404, "text/plain", "Not Found");
    });
    
    server->begin();
    webServerStarted = true;
    networkMode = NetworkMode::AP_MODE;
    
    Serial.println("[NetMgr] WebServer started (AP mode, IP: 192.168.4.1)");
    Serial.println("[NetMgr] Connect to WiFi: " + String(AP_SSID));
    logger.log("WebServer: AP mode started");
}

// === ОСНОВНАЯ ИНИЦИАЛИЗАЦИЯ (БЕЗ БЛОКИРОВКИ) ===
void AppNetwork::begin(int checkIntervalMinutes) {
    // Защита от нулевого интервала (минимум 1 минута)
    if (checkIntervalMinutes < 1) checkIntervalMinutes = 1;
    checkIntervalMs = checkIntervalMinutes * 60000;
    Serial.printf("[NetMgr] Check interval: %d min (%d ms)\n", checkIntervalMinutes, checkIntervalMs);

    // 1. SD карта уже инициализирована (initSD вызван раньше)
    if (!sdInitialized) {
        Serial.println("[NetMgr] SD not initialized! Cannot load WiFi config.");
        return;
    }

    // 2. Чтение конфигурации WiFi
    if (!loadConfigFromSD()) {
        Serial.println("[NetMgr] WiFi config not found. AP mode only.");
        return;
    }
    
    // 3. WebServer должен быть уже запущен через startWebServerEarly()
    if (!webServerStarted) {
        Serial.println("[NetMgr] WARNING: WebServer not started! Call startWebServerEarly() first.");
        startWebServerEarly();
    }
    
    // 4. Пробуем подключиться к WiFi (с коротким таймаутом)
    Serial.println("[NetMgr] Trying to connect to WiFi...");
    
    // === ВАЖНО: Не блокируем надолго! ===
    // Пробуем только один раз с коротким таймаутом
    WiFi.begin(ssid1.c_str(), pass1.c_str());
    
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 20) {  // 2 сек максимум
        delay(100);
        if (server && systemReady) server->handleClient();  // Не обрабатываем до полной инициализации
        yield();
        tries++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        networkMode = NetworkMode::STA_MODE;
        
        // === ВАЖНО: НЕ отключаем AP режим! ===
        // Двойной режим (AP+STA) обеспечивает无缝 переход
        // 192.168.4.1 всегда доступен, 192.168.1.x когда WiFi подключен
        // Это решает проблему долгой загрузки при переключении
        Serial.println("[NetMgr] WiFi connected! (AP+STA dual mode)");
        
        Serial.print("[NetMgr] STA IP: "); Serial.println(WiFi.localIP());
        Serial.print("[NetMgr] AP IP: "); Serial.println(WiFi.softAPIP());
        logger.log("WiFi: Connected to " + ssid1 + " (dual mode)");
        
        // Проверяем интернет (неблокирующе)
        online = checkInternet();
        
        if (online) {
            // Telegram ОТКЛЮЧЁН — не инициализируем
            // client.setCACert(TELEGRAM_CERTIFICATE_ROOT);
            // if (tgToken.length() > 0) { bot = new UniversalTelegramBot(tgToken, client); }
            
            // NTP синхронизация (неблокирующе)
            syncNTP();
            
            Serial.println("[NetMgr] Internet OK. NTP enabled.");
            
            // Telegram disabled
            // String msg = "System " + String(FIRMWARE_VERSION) + " started. IP: " + WiFi.localIP().toString();
            // queueMessage(msg);
        } else {
            Serial.println("[NetMgr] No internet. Web only mode.");
        }
        
        logger.log("Network: STA mode, IP: " + WiFi.localIP().toString());
        
    } else {
        // WiFi не подключился - остаёмся в AP режиме (Web уже работает!)
        Serial.println("[NetMgr] WiFi connection failed. Staying in AP mode.");
        Serial.println("[NetMgr] Web interface: http://192.168.4.1");
        logger.log("WiFi: Connection failed, AP mode");
        // WebServer уже работает в AP режиме!
    }
    
    networkInitialized = true;
}

// === АСИНХРОННАЯ ПЕРЕПОДКЛЮЧЕННАЯ К WIFI ===
// Вызывается из update() при потере связи
bool AppNetwork::connectToWiFi() {
    // Пробуем первую сеть (до 3 раз, 15 итераций × 100мс = 1.5 сек на попытку)
    for (int i = 0; i < 3; i++) {
        Serial.printf("[NetMgr] Try %s (Attempt %d)\n", ssid1.c_str(), i+1);
        WiFi.begin(ssid1.c_str(), pass1.c_str());
        
        int tries = 0;
        while (WiFi.status() != WL_CONNECTED && tries < 15) {
            delay(100);
            // handleClient каждые 5 итераций — WebServer не подвисает
            if (tries % 5 == 0 && server && systemReady) server->handleClient();
            yield();
            tries++;
        }
        
        if (WiFi.status() == WL_CONNECTED) return true;
        if (server && systemReady) server->handleClient();
        yield();
    }
    
    // Пробуем вторую сеть
    if (ssid2.length() > 0) {
        for (int i = 0; i < 3; i++) {
            Serial.printf("[NetMgr] Try %s (Attempt %d)\n", ssid2.c_str(), i+1);
            WiFi.begin(ssid2.c_str(), pass2.c_str());
            
            int tries = 0;
            while (WiFi.status() != WL_CONNECTED && tries < 15) {
                delay(100);
                if (tries % 5 == 0 && server && systemReady) server->handleClient();
                yield();
                tries++;
            }
            
            if (WiFi.status() == WL_CONNECTED) return true;
            if (server && systemReady) server->handleClient();
            yield();
        }
    }
    
    return false;
}

// === ЗАПУСК ТОЧКИ ДОСТУПА (AP MODE) ===
bool AppNetwork::startAPMode() {
    Serial.println("[NetMgr] Starting AP Mode...");
    Serial.print("[NetMgr] SSID: "); Serial.println(AP_SSID);
    Serial.print("[NetMgr] IP: "); Serial.println(AP_IP_ADDR);
    
    // Настройка IP адреса
    IPAddress local_IP(192, 168, 4, 1);
    IPAddress gateway(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);
    
    if (!WiFi.softAPConfig(local_IP, gateway, subnet)) {
        Serial.println("[NetMgr] AP Config FAILED!");
        return false;
    }
    
    // Запуск точки доступа
    if (!WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL)) {
        Serial.println("[NetMgr] AP Start FAILED!");
        return false;
    }
    
    Serial.println("[NetMgr] AP Mode started successfully!");
    
    // DNS сервер (Captive Portal)
    if (dnsServer) {
        dnsServer->stop();
        delete dnsServer;
    }
    dnsServer = new DNSServer();
    dnsServer->start(53, "*", WiFi.softAPIP());
    Serial.println("[NetMgr] DNS Server started (Captive Portal)");
    
    // WebServer должен быть уже запущен через startWebServerEarly()
    if (!webServerStarted) {
        Serial.println("[NetMgr] ERROR: WebServer not started!");
        return false;
    }
    
    logger.log("Network: AP Mode");
    return true;
}

// === ПОЛУЧЕНИЕ РЕЖИМА СЕТИ ===
NetworkMode AppNetwork::getNetworkMode() {
    return networkMode;
}

// === СИМВОЛ СЕТИ ДЛЯ LCD ===
char AppNetwork::getNetworkSymbol() {
    switch (networkMode) {
        case NetworkMode::STA_MODE: return 'W';
        case NetworkMode::AP_MODE:  return 'A';
        default:                    return 'X';
    }
}


void AppNetwork::setEngine(ProcessEngine* engine, ConfigManager* cfgMgr) {
    this->processEngine = engine;
    this->configManager = cfgMgr;
}

void AppNetwork::setSystemReady(bool ready) {
    systemReady = ready;
    if (ready) {
        Serial.println("[NetMgr] System ready — API handlers enabled");
    }
}


void AppNetwork::update() {
    if (!server) return; 
    unsigned long now = millis();
    
    // === ЗАЩИТА: не обрабатываем HTTP до полной инициализации системы ===
    // Без этого processEngine->getSensorData() крашит (sensorAdapter = nullptr)
    // Время window: между startTask() и processEngine.begin() (~1-2 сек при загрузке)
    // =========================================================================
    if (!systemReady) {
        return;  // Браузер подождёт, запрос обработается после systemReady
    }
    
    // === ДИАГНОСТИКА ПАМЯТИ (каждые 5 минут) ===
    static unsigned long lastMemCheck = 0;
    if (now - lastMemCheck > 300000) {
        lastMemCheck = now;
        Serial.printf("[MEM] Free heap: %u bytes\n", ESP.getFreeHeap());
    }
    // ===========================================
    
    // === ПЕРВЫМ ДЕЛОМ: WebServer (самый приоритетный!) ===
    server->handleClient();
    
    // === DNS SERVER (всегда, если активен) ===
    // В dual mode (AP+STA) DNS нужен для AP интерфейса
    // 3 итерации вместо 10 — достаточно для отзывчивости, не перегружает CPU
    if (dnsServer) {
        for (int i = 0; i < 3; i++) {
            dnsServer->processNextRequest();
        }
    }
    
    // === ПЕРИОДИЧЕСКАЯ ПРОВЕРКА СЕТИ ===
    // Защита: минимальный интервал 30 секунд
    unsigned long safeInterval = (checkIntervalMs > 30000) ? checkIntervalMs : 30000;
    
    if (networkInitialized && (now - lastCheckTime > safeInterval || lastCheckTime == 0)) {
        lastCheckTime = now;
        
        if (networkMode == NetworkMode::STA_MODE) {
            // === Проверка WiFi ===
            if (WiFi.status() != WL_CONNECTED) {
                Serial.println("[NetMgr] WiFi lost. Reconnecting...");
                if (connectToWiFi()) {
                    Serial.println("[NetMgr] WiFi reconnected.");
                } else {
                    Serial.println("[NetMgr] WiFi reconnect failed. Switching to AP...");
                    if (startAPMode()) {
                        networkMode = NetworkMode::AP_MODE;
                    } else {
                        networkMode = NetworkMode::OFFLINE;
                    }
                    return;
                }
            }
            
            // === Проверка интернета ===
            bool hasInternet = checkInternet();
            if (hasInternet && !online) {
                online = true;
                syncNTP();
                // sendMessage("Connection restored."); // Telegram disabled
                Serial.println("[NetMgr] Internet restored.");
            } else if (!hasInternet && online) {
                online = false;
                Serial.println("[NetMgr] Internet lost. Web continues, Telegram disabled.");
            }
        }
    }

    // === TELEGRAM ОТКЛЮЧЁН ===
    // if (processEngine && online) {
    //     const SystemStatus& status = processEngine->getStatus();
    //     const SensorData& sensors = processEngine->getSensorData(); 
    //     if (status.safety == SafetyState::WARNING_TSA || status.safety == SafetyState::EMERGENCY) {
    //         if (now - lastAlarmTgTime > 60000) {
    //             lastAlarmTgTime = now;
    //             String msg = "🔥 АВАРИЯ! TSA: " + String(status.currentTsa, 1) + "C (Limit: " + String(configManager->getConfig().tsaLimit) + "C)";
    //             sendMessage(msg);
    //         }
    //     }
    //     else if (status.safety == SafetyState::WARNING_BOX) {
    //         if (now - lastAlarmTgTime > 60000) {
    //             lastAlarmTgTime = now;
    //             String msg = "⚠️ ВНИМАНИЕ! Перегрев бокса: " + String(sensors.boxTemp, 1) + "C";
    //             sendMessage(msg);
    //         }
    //     }
    //     else {
    //         lastAlarmTgTime = 0;
    //     }
    // }
    // processMessageQueue();
}

// ================== API HANDLERS ==================

void AppNetwork::handleApiStatus() {
    if (!processEngine || !configManager) {
        server->send(500, "application/json", "{\"error\":\"Engine not linked\"}");
        return;
    }

    const SystemStatus& status = processEngine->getStatus();
    const SensorData& sensors = processEngine->getSensorData(); 
    SystemConfig& cfg = configManager->getConfig();

    // Формируем JSON вручную
    String json = "{";

    // --- Сеть и Время ---
    json += "\"online\":" + String(online ? "true" : "false") + ",";
    json += "\"network_mode\":\"" + String(getNetworkSymbol()) + "\",";
    json += "\"time\":" + String(status.processTimeSec) + ",";
    
    // --- Статус системы (Safety) ---
    int safetyCode = 0;
    String safetyText = "НОРМА";
    String safetyDetail = "";
    
    if (status.safety == SafetyState::WARNING_BOX) { 
        safetyCode = 1; 
        safetyText = "ВНИМАНИЕ: Перегрев"; 
        safetyDetail = "Температура корпуса: " + String(sensors.boxTemp, 1) + "C";
    }
    else if (status.safety == SafetyState::WARNING_TSA) { 
        safetyCode = 2; 
        safetyText = "АВАРИЯ: TSA"; 
        safetyDetail = "TSA: " + String(status.currentTsa, 1) + "C (лимит: " + String(configManager->getConfig().tsaLimit, 0) + "C)";
    }
    else if (status.safety == SafetyState::EMERGENCY) { 
        safetyCode = 3; 
        safetyText = "АВАРИЯ: VREAC"; 
        safetyDetail = "Температура: " + String(status.currentTsa, 1) + "C";
    }
    
    json += "\"safety_code\":" + String(safetyCode) + ",";
    json += "\"safety_text\":\"" + safetyText + "\",";
    json += "\"safety_detail\":\"" + safetyDetail + "\",";

    // --- Процесс ---
    json += "\"process\":" + String(processEngine->getActiveProcessType()) + ",";
    json += "\"stage\":\"" + status.stageName + "\",";

    // --- Датчики ---
    json += "\"tsa\":" + String(status.currentTsa) + ",";
    json += "\"tsar\":" + String(status.currentTsar) + ",";
    json += "\"aqua\":" + String(status.currentAqua) + ",";
    json += "\"tank\":" + String(status.currentTank) + ",";
    
    // --- Крепость ---
    json += "\"str_bak\":" + String(status.currentStrengthBak) + ",";
    json += "\"str_out\":" + String(status.currentStrength) + ",";
    json += "\"str_bak_valid\":" + String(status.strengthBakValid ? "true" : "false") + ",";
    json += "\"str_out_valid\":" + String(status.strengthOutValid ? "true" : "false") + ",";
    json += "\"pressure\":" + String(sensors.pressure * 0.75) + ",";
    json += "\"box_temp\":" + String(sensors.boxTemp) + ",";
    json += "\"humidity\":" + String(sensors.humidity, 1) + ",";
    
    // --- Калибровка датчиков ---
    json += "\"webCalibStatus\":" + String(status.webCalibStatus) + ",";
    json += "\"webCalibSensorName\":\"" + status.webCalibSensorName + "\",";

    // === RECT INFO ===
    json += "\"rectMethodName\":\"" + status.rectMethodName + "\",";
    json += "\"rectSubStage\":\"" + status.rectSubStage + "\",";
    json += "\"rectTimeRemaining\":" + String(status.rectTimeRemaining) + ",";
    json += "\"rectVolumeTarget\":" + String(status.rectVolumeTarget) + ",";
    json += "\"bodyMethodName\":\"" + status.bodyMethodName + "\",";
    json += "\"bodySpeed\":" + String(status.bodySpeed, 1) + ",";
    json += "\"bodyVolDone\":" + String(status.bodyVolDone, 1) + ",";
    json += "\"headsSpeed\":" + String(status.headsSpeed, 1) + ",";
    json += "\"headsSpeedCalc\":" + String(status.headsSpeedCalc, 1) + ",";
    json += "\"bodyCycle\":" + String(status.bodyCycle) + ",";
    
    // === Накопленный объём ===
    json += "\"headsVolDone\":" + String(status.headsVolDone, 0) + ",";
    json += "\"headsVolSub\":" + String(status.headsVolSub, 1) + ",";
    json += "\"headsVolTarget\":" + String(status.headsVolTarget) + ",";
    json += "\"finishingRemainSec\":" + String(status.finishingRemainSec) + ",";
    json += "\"stageTimeSec\":" + String(status.stageTimeSec) + ",";
    json += "\"alarmTimerSec\":" + String(status.alarmTimerSec) + ",";
    
    // === Статусы выходов ===
    json += "\"heaterOn\":" + String(processEngine->isHeaterOn() ? "true" : "false") + ",";
    json += "\"mixerOn\":" + String(processEngine->isMixerOn() ? "true" : "false") + ",";
    json += "\"waterValveOpen\":" + String(processEngine->isWaterValveOpen() ? "true" : "false") + ",";
    
    // === Референтные значения ===
    json += "\"rtsarM\":" + String(processEngine->getRtsarM(), 2) + ",";
    json += "\"adPressM\":" + String(processEngine->getAdPressM(), 1) + ",";
    
    // === Общий объём голов ===
    float headsTotalTarget = cfg.headsTypeKSS ? (cfg.asVolume * 0.20f) : (cfg.asVolume * 0.10f);
    json += "\"headsTotalTarget\":" + String((int)headsTotalTarget) + ",";

    // --- Настройки ---
    json += "\"power\":" + String(cfg.power) + ",";
    json += "\"heater\":" + String(cfg.heaterType) + ",";
    json += "\"cfg\": " + buildCfgJson();

    // --- Статус тестов клапанов ---
    json += ",\"headTest\":{";
    json += "\"active\":" + String(status.headTestActive ? "true" : "false") + ",";
    json += "\"remainingSec\":" + String(status.headTestRemainingSec) + ",";
    json += "\"totalSec\":" + String(status.headTestTotalSec);
    json += "}";
    json += ",\"bodyTest\":{";
    json += "\"active\":" + String(status.bodyTestActive ? "true" : "false") + ",";
    json += "\"remainingSec\":" + String(status.bodyTestRemainingSec) + ",";
    json += "\"totalSec\":" + String(status.bodyTestTotalSec);
    json += "}";
    json += ",\"testAwaitingInput\":" + String(status.testAwaitingInput ? "true" : "false");
    json += ",\"testAwaitingType\":\"" + status.testAwaitingType + "\"";

    // === СОСТОЯНИЕ МАСТЕРА КАЛИБРОВКИ ===
    json += ",\"calibWizard\":{";
    json += "\"valve\":" + String(status.calibWizard.valve) + ",";
    json += "\"step\":" + String(status.calibWizard.step) + ",";
    json += "\"isRunning\":" + String(status.calibWizard.isRunning ? "true" : "false") + ",";
    json += "\"remainingSec\":" + String(status.calibWizard.remainingSec) + ",";
    json += "\"totalSec\":" + String(status.calibWizard.totalSec) + ",";
    json += "\"volume\":" + String(status.calibWizard.volume, 1) + ",";
    json += "\"capacity\":" + String(status.calibWizard.capacity, 1);
    json += "}";
    
    json += "}";
    
    server->send(200, "application/json", json);
}

String AppNetwork::buildTelemetryJson() {
    if (!processEngine || !configManager) {
        return "{}";
    }

    const SystemStatus& status = processEngine->getStatus();
    const SensorData& sensors = processEngine->getSensorData();
    SystemConfig& cfg = configManager->getConfig();

    float headsTotalTarget = cfg.headsTypeKSS ? (cfg.asVolume * 0.20f) : (cfg.asVolume * 0.10f);

    String json = "{";
    json += "\"time\":" + String(status.processTimeSec) + ",";
    json += "\"stage\":\"" + status.stageName + "\",";
    json += "\"process\":" + String(processEngine->getActiveProcessType()) + ",";
    json += "\"tsa\":" + String(status.currentTsa) + ",";
    json += "\"tsar\":" + String(status.currentTsar) + ",";
    json += "\"aqua\":" + String(status.currentAqua) + ",";
    json += "\"tank\":" + String(status.currentTank) + ",";
    json += "\"str_bak\":" + String(status.currentStrengthBak) + ",";
    json += "\"str_out\":" + String(status.currentStrength) + ",";
    json += "\"str_bak_valid\":" + String(status.strengthBakValid ? "true" : "false") + ",";
    json += "\"str_out_valid\":" + String(status.strengthOutValid ? "true" : "false") + ",";
    json += "\"safety\":" + String((int)status.safety) + ",";
    json += "\"pressure\":" + String(sensors.pressure * 0.75) + ",";
    json += "\"box_temp\":" + String(sensors.boxTemp) + ",";
    json += "\"humidity\":" + String(sensors.humidity, 1) + ",";
    json += "\"alarmTimerSec\":" + String(status.alarmTimerSec) + ",";
    json += "\"stageTimeSec\":" + String(status.stageTimeSec) + ",";
    json += "\"finishingRemainSec\":" + String(status.finishingRemainSec) + ",";
    json += "\"rectMethodName\":\"" + status.rectMethodName + "\",";
    json += "\"rectSubStage\":\"" + status.rectSubStage + "\",";
    json += "\"rectTimeRemaining\":" + String(status.rectTimeRemaining) + ",";
    json += "\"rectVolumeTarget\":" + String(status.rectVolumeTarget) + ",";
    json += "\"bodyMethodName\":\"" + status.bodyMethodName + "\",";
    json += "\"bodySpeed\":" + String(status.bodySpeed, 1) + ",";
    json += "\"bodyVolDone\":" + String(status.bodyVolDone, 1) + ",";
    json += "\"bodyCycle\":" + String(status.bodyCycle) + ",";
    json += "\"headsSpeed\":" + String(status.headsSpeed, 1) + ",";
    json += "\"headsSpeedCalc\":" + String(status.headsSpeedCalc, 1) + ",";
    json += "\"headsVolDone\":" + String(status.headsVolDone, 0) + ",";
    json += "\"headsVolSub\":" + String(status.headsVolSub, 1) + ",";
    json += "\"headsVolTarget\":" + String(status.headsVolTarget) + ",";
    json += "\"headsTotalTarget\":" + String((int)headsTotalTarget) + ",";
    json += "\"heaterOn\":" + String(processEngine->isHeaterOn() ? "true" : "false") + ",";
    json += "\"mixerOn\":" + String(processEngine->isMixerOn() ? "true" : "false") + ",";
    json += "\"waterValveOpen\":" + String(processEngine->isWaterValveOpen() ? "true" : "false") + ",";
    json += "\"running\":" + String(processEngine->isProcessRunning() ? "true" : "false") + ",";
    json += "\"rtsarM\":" + String(processEngine->getRtsarM(), 2) + ",";
    json += "\"adPressM\":" + String(processEngine->getAdPressM(), 1) + ",";
    json += "\"useHeadValve\":" + String(cfg.useHeadValve ? "true" : "false") + ",";
    json += "\"bodyValveNC\":" + String(cfg.bodyValveNC ? "true" : "false") + ",";
    json += "\"headsTypeKSS\":" + String(cfg.headsTypeKSS ? "true" : "false") + ",";
    json += "\"heaterType\":" + String(cfg.heaterType) + ",";
    json += "\"power\":" + String(cfg.power) + ",";
    json += "\"asVolume\":" + String(cfg.asVolume) + ",";
    json += "\"headOpenMs\":" + String(cfg.headOpenMs) + ",";
    json += "\"headCloseMs\":" + String(cfg.headCloseMs) + ",";
    json += "\"bodyOpenMs\":" + String(cfg.bodyOpenMs) + ",";
    json += "\"bodyCloseMs\":" + String(cfg.bodyCloseMs) + ",";
    json += "\"valve_head_capacity\":" + String(cfg.valve_head_capacity) + ",";
    json += "\"valve_body_capacity\":" + String(cfg.valve_body_capacity) + ",";
    json += "\"minOpenTime\":" + String(cfg.minOpenTime) + ",";
    json += "\"speedHeadCorr\":" + String(cfg.speedHeadCorr) + ",";
    json += "\"speedBodyCorr\":" + String(cfg.speedBodyCorr);
    json += ",";
    // Используем готовый buildCfgJson()
    json += "\"cfg\": " + buildCfgJson();
    
    // === ОТПРАВКА НОВЫХ ЛОГОВ В ОБЛАКО ===
    String newLogs = logger.readNewLog(lastLogSize);
    if (newLogs.length() > 0) {
        newLogs.replace("\\", "\\\\");
        newLogs.replace("\"", "\\\"");
        newLogs.replace("\n", "\\n");
        newLogs.replace("\r", "");
        json += ",\"new_logs\":\"" + newLogs + "\"";
    }
    // =========================================
    
    json += "}";
    
    return json;
}

void AppNetwork::handleApiCommand() {
    if (!server->hasArg("plain")) {
        server->send(400, "text/plain", "Bad Request");
        return;
    }

    String body = server->arg("plain");
    Serial.println("[API] Command: " + body);

    auto sendCmd = [](UiCommand cmd, int param = 0) {
        CommandMessage msg = { cmd, param };
        xQueueSend(commandQueue, &msg, 0);
    };
    
    auto getValveParam = [&body]() -> int {
        int idx = body.indexOf("\"valve\":");
        if (idx > 0) {
            int start = idx + 8;
            int end = body.indexOf(',', start);
            if (end == -1) end = body.indexOf('}', start);
            String valStr = body.substring(start, end);
            valStr.trim();
            return valStr.toInt();
        }
        return 0;
    };

    if      (body.indexOf("\"cmd\":\"START_DIST\"") > 0)        sendCmd(UiCommand::START_DIST);
    else if (body.indexOf("\"cmd\":\"START_RECT\"") > 0)        sendCmd(UiCommand::START_RECT);
    else if (body.indexOf("\"cmd\":\"STOP\"") > 0)              sendCmd(UiCommand::STOP_PROCESS);
    else if (body.indexOf("\"cmd\":\"STOP_TEST\"") > 0)         sendCmd(UiCommand::STOP_TEST);
    else if (body.indexOf("\"cmd\":\"FINISH_CALIBRATION\"") > 0) sendCmd(UiCommand::FINISH_CALIBRATION);
    else if (body.indexOf("\"cmd\":\"DIALOG_YES\"") > 0)        sendCmd(UiCommand::DIALOG_YES);
    else if (body.indexOf("\"cmd\":\"DIALOG_NO\"") > 0)         sendCmd(UiCommand::DIALOG_NO);
    else if (body.indexOf("\"cmd\":\"NEXT_STAGE\"") > 0)        sendCmd(UiCommand::NEXT_STAGE);
    else if (body.indexOf("\"cmd\":\"TEST_HEAD\"") > 0)         sendCmd(UiCommand::TEST_HEAD);
    else if (body.indexOf("\"cmd\":\"TEST_BODY\"") > 0)         sendCmd(UiCommand::TEST_BODY);
    else if (body.indexOf("\"cmd\":\"CALIB_START_DRY\"") > 0)   sendCmd(UiCommand::CALIB_START_DRY, getValveParam());
    else if (body.indexOf("\"cmd\":\"CALIB_START_CAP\"") > 0)   sendCmd(UiCommand::CALIB_START_CAPACITY, getValveParam());
    else if (body.indexOf("\"cmd\":\"CALIB_CANCEL\"") > 0)      sendCmd(UiCommand::CALIB_CANCEL);
    else if (body.indexOf("\"cmd\":\"IDENTIFY_") > 0) {
        if      (body.indexOf("TSAR") > 0) sendCmd(UiCommand::IDENTIFY_TSAR);
        else if (body.indexOf("TANK") > 0) sendCmd(UiCommand::IDENTIFY_TANK);
        else if (body.indexOf("AQUA") > 0) sendCmd(UiCommand::IDENTIFY_AQUA);
        else if (body.indexOf("TSA") > 0)  sendCmd(UiCommand::IDENTIFY_TSA);
    }
    else {
        server->send(400, "text/plain", "Unknown Command");
        return;
    }

    server->send(200, "text/plain", "OK");
}

void AppNetwork::handleApiSettings() {
    if (!server->hasArg("plain")) {
        server->send(400, "text/plain", "Bad Request");
        return;
    }
    
    String body = server->arg("plain");
    Serial.println("[API] Settings Save Request received.");

    SystemConfig& cfg = configManager->getConfig();

    auto getInt = [&](const char* key, int defaultVal) -> int {
        String searchKey = "\"" + String(key) + "\":";
        int pos = body.indexOf(searchKey);
        if (pos != -1) {
            int start = pos + searchKey.length();
            int end = body.indexOf(',', start);
            if (end == -1) end = body.indexOf('}', start);
            String valStr = body.substring(start, end);
            valStr.trim();
            if (valStr.equalsIgnoreCase("true")) return 1;
            if (valStr.equalsIgnoreCase("false")) return 0;
            return valStr.toInt();
        }
        return defaultVal;
    };

    auto getFloat = [&](const char* key, float defaultVal) -> float {
        String searchKey = "\"" + String(key) + "\":";
        int pos = body.indexOf(searchKey);
        if (pos != -1) {
            int start = pos + searchKey.length();
            int end = body.indexOf(',', start);
            if (end == -1) end = body.indexOf('}', start);
            String valStr = body.substring(start, end);
            return valStr.toFloat();
        }
        return defaultVal;
    };

    auto getBool = [&](const char* key, bool defaultVal) -> bool {
        String searchKey = "\"" + String(key) + "\":";
        int pos = body.indexOf(searchKey);
        if (pos != -1) {
            int start = pos + searchKey.length();
            int end = body.indexOf(',', start);
            if (end == -1) end = body.indexOf('}', start);
            String valStr = body.substring(start, end);
            valStr.trim();
            if (valStr == "1" || valStr.equalsIgnoreCase("true")) return true;
            return false;
        }
        return defaultVal;
    };

    // Обновляем настройки (если ключ отсутствует в JSON — сохраняем текущее значение)
    cfg.emergencyTime = getInt("emergencyTime", cfg.emergencyTime);
    cfg.nasebTime = getInt("nasebTime", cfg.nasebTime);
    cfg.reklapTime = getInt("reklapTime", cfg.reklapTime);
    cfg.boxMaxTemp = getInt("boxMaxTemp", cfg.boxMaxTemp);
    cfg.power = getInt("power", cfg.power);
    cfg.asVolume = getInt("asVolume", cfg.asVolume);
    cfg.chekwifi = getInt("chekwifi", cfg.chekwifi);

    cfg.razgonTemp = getInt("razgonTemp", cfg.razgonTemp);
    cfg.bakStopTemp = getInt("bakStopTemp", cfg.bakStopTemp);

    // Midterm логика — используем -1 как маркер "не передано" вместо 0
    int newMidtermAbv = getInt("midterm_abv", -1);
    int newMidterm = getInt("midterm", -1);
    
    const SensorData& sensors = processEngine->getSensorData();
    float pressure_mmHg = sensors.pressure * 0.75006f;

    if (newMidterm != -1 && newMidterm != cfg.midterm) {
        cfg.midterm = newMidterm;
        int calcAbv = (int)round(configManager->getOutputABVForTemp((float)newMidterm, pressure_mmHg));
        cfg.midterm_abv = calcAbv; 
    } 
    else if (newMidtermAbv != -1 && newMidtermAbv > 0) {
        cfg.midterm_abv = newMidtermAbv;
        cfg.midterm = (int)round(configManager->getTempForOutputABV((float)newMidtermAbv, pressure_mmHg));
    }
    // Если ключи не переданы — НЕ меняем cfg.midterm и cfg.midterm_abv

    cfg.heaterType = getInt("heaterType", cfg.heaterType);
    cfg.fullPwr = getBool("fullPwr", cfg.fullPwr);
    cfg.valveuse = getBool("valveuse", cfg.valveuse);
    cfg.mixerEnabled = getBool("mixerEnabled", cfg.mixerEnabled);
    cfg.mixerOnTime = getInt("mixerOnTime", cfg.mixerOnTime);
    cfg.mixerOffTime = getInt("mixerOffTime", cfg.mixerOffTime);

    cfg.tsaLimit = getInt("tsaLimit", cfg.tsaLimit);
    cfg.cycleLim = getInt("cycleLim", cfg.cycleLim);
    cfg.histeresis = getFloat("histeresis", cfg.histeresis);
    cfg.delta = getFloat("delta", cfg.delta);
    cfg.useHeadValve = getBool("useHeadValve", cfg.useHeadValve);
    cfg.bodyValveNC = getBool("bodyValveNC", cfg.bodyValveNC);
    cfg.headsTypeKSS = getBool("headsTypeKSS", cfg.headsTypeKSS);
    cfg.calibration = getBool("calibration", cfg.calibration);
    
    cfg.headOpenMs = getInt("headOpenMs", cfg.headOpenMs);
    cfg.headCloseMs = getInt("headCloseMs", cfg.headCloseMs);
    cfg.bodyOpenMs = getInt("bodyOpenMs", cfg.bodyOpenMs);
    cfg.bodyCloseMs = getInt("bodyCloseMs", cfg.bodyCloseMs);
    cfg.active_test = getInt("active_test", cfg.active_test);
    cfg.valve_head_capacity = getInt("valve_head_capacity", cfg.valve_head_capacity);
    cfg.valve_body_capacity = getInt("valve_body_capacity", cfg.valve_body_capacity);
    cfg.valve0_body_capacity = getInt("valve0_body_capacity", cfg.valve0_body_capacity);
    cfg.minOpenTime = getInt("minOpenTime", cfg.minOpenTime);
    cfg.speedHeadCorr = getInt("speedHeadCorr", cfg.speedHeadCorr);
    cfg.speedBodyCorr = getInt("speedBodyCorr", cfg.speedBodyCorr);

    // === ОБЛАЧНЫЕ НАСТРОЙКИ ===
    auto getString = [&](const char* key, char* dest, size_t maxLen) {
        String searchKey = "\"" + String(key) + "\":\"";
        int pos = body.indexOf(searchKey);
        if (pos != -1) {
            int start = pos + searchKey.length();
            int end = body.indexOf("\"", start);
            if (end == -1) end = body.indexOf("}", start);
            String val = body.substring(start, end);
            val.trim();
            val.toCharArray(dest, maxLen);
        }
    };
    
    getString("cloudUrl", cfg.cloudUrl, sizeof(cfg.cloudUrl));
    getString("cloudApiKey", cfg.cloudApiKey, sizeof(cfg.cloudApiKey));
    // ============================

    // === ИНЖЕНЕРНОЕ МЕНЮ ===
    cfg.speedGolovyBase = getInt("speedGolovyBase", cfg.speedGolovyBase);
    cfg.speedTeloBase = getInt("speedTeloBase", cfg.speedTeloBase);
    cfg.distHeadsSpeed = getInt("distHeadsSpeed", cfg.distHeadsSpeed);
    cfg.shporaCorr = getFloat("shporaCorr", cfg.shporaCorr);
    cfg.shporaStabMs = getInt("shporaStabMs", cfg.shporaStabMs);
    cfg.minBodySpeed = getInt("minBodySpeed", cfg.minBodySpeed);
    cfg.headsShareStd = getFloat("headsShareStd", cfg.headsShareStd);
    cfg.headsShareKssSpit = getFloat("headsShareKssSpit", cfg.headsShareKssSpit);
    cfg.headsShareKssStd = getFloat("headsShareKssStd", cfg.headsShareKssStd);
    cfg.headsShareKssAkatelo = getFloat("headsShareKssAkatelo", cfg.headsShareKssAkatelo);
    cfg.pressureCoeff = getFloat("pressureCoeff", cfg.pressureCoeff);
    cfg.coolingDurationSec = getInt("coolingDurationSec", cfg.coolingDurationSec);
    cfg.bakstopDelaySec = getInt("bakstopDelaySec", cfg.bakstopDelaySec);
    cfg.calibDrySec = getInt("calibDrySec", cfg.calibDrySec);
    cfg.calibCapacitySec = getInt("calibCapacitySec", cfg.calibCapacitySec);
    // =========================

    Serial.println("[API] Saving config...");
    configManager->saveConfig();
    configManager->saveDistConfig();
    configManager->saveRectConfig();
    
    // Перезагружаем конфиг в память чтобы ProcessEngine увидел изменения
    configManager->loadConfig();
    
    Serial.println("[API] Settings SAVED to EEPROM.");
    server->send(200, "text/plain", "Settings Saved");
}

// === Загрузка конфигурации WiFi с SD ===
bool AppNetwork::loadConfigFromSD() {
    File file = SD.open("/wifi_config.txt");
    if (!file) {
        Serial.println("[NetMgr] wifi_config.txt not found");
        return false;
    }
    
    while(file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        
        if (line.startsWith("ssid1=")) ssid1 = parseLine(line, "ssid1");
        else if (line.startsWith("pass1=")) pass1 = parseLine(line, "pass1");
        else if (line.startsWith("ssid2=")) ssid2 = parseLine(line, "ssid2");
        else if (line.startsWith("pass2=")) pass2 = parseLine(line, "pass2");
        else if (line.startsWith("tg_token=")) tgToken = parseLine(line, "tg_token");
        else if (line.startsWith("tg_chat=")) tgChatId = parseLine(line, "tg_chat");
    }
    file.close();
    
    if (ssid1.length() == 0) return false;
    return true;
}

String AppNetwork::parseLine(String line, String key) {
    int idx = line.indexOf('=');
    if (idx == -1) return "";
    String val = line.substring(idx + 1);
    val.trim();
    return val;
}

void AppNetwork::syncNTP() {
    Serial.println("[NetMgr] Syncing NTP...");
    int tzOffset = configManager ? configManager->getConfig().timezoneOffset : 3;
    configTime(tzOffset * 3600, 0, "pool.ntp.org", "time.nist.gov");
    
    struct tm timeinfo;
    // Таймаут 2000 мс вместо дефолтного 5000 мс — не блокируем надолго
    if (!getLocalTime(&timeinfo, 2000)) {
        Serial.println("[NetMgr] NTP Failed");
        return;
    }
    Serial.println("[NetMgr] NTP Sync OK");
}

bool AppNetwork::checkInternet() {
    WiFiClient testClient;
    testClient.setTimeout(1);  // 1 сек вместо 2 — быстрее обнаруживаем отсутствие интернета

    // 3 попытки вместо 20: максимальная задержка 3 сек вместо потенциальных 40 сек
    for (int i = 0; i < 3; i++) {
        if (testClient.connect("google.com", 80)) {
            testClient.stop();
            Serial.println("[NetMgr] Internet check: OK");
            return true;
        }
        if (server && systemReady) server->handleClient();  // не блокируем WebServer
        yield();
    }

    testClient.stop();
    Serial.println("[NetMgr] Internet check: FAILED");
    return false;
}

// === TELEGRAM ОТКЛЮЧЁН ===

void AppNetwork::sendMessage(const String& text) {
    // Telegram disabled - do nothing
}

bool AppNetwork::isOnline() {
    return online;
}

String AppNetwork::getTimeStr() {
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)){
        return "??:??";
    }
    char buf[6];
    strftime(buf, sizeof(buf), "%H:%M", &timeinfo);
    return String(buf);
}

// === FREERTOS TASK ===
static void networkTaskWrapper(void* param) {
    AppNetwork* self = static_cast<AppNetwork*>(param);
    for (;;) {
        self->update();
        // 10мс вместо 2мс - меньше нагрузка на CPU
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void AppNetwork::startTask() {
    xTaskCreatePinnedToCore(
        networkTaskWrapper,
        "NetworkTask",
        8192,
        this,
        1,
        &networkTaskHandle,
        0
    );
}

// === ВСПОМОГАТЕЛЬНЫЕ ===

String AppNetwork::buildCfgJson() {
    if (!configManager) return "{}";
    SystemConfig& cfg = configManager->getConfig();
    
    String json = "{";
    json += "\"emergencyTime\":" + String(cfg.emergencyTime) + ",";
    json += "\"nasebTime\":" + String(cfg.nasebTime) + ",";
    json += "\"reklapTime\":" + String(cfg.reklapTime) + ",";
    json += "\"boxMaxTemp\":" + String(cfg.boxMaxTemp) + ",";
    json += "\"power\":" + String(cfg.power) + ",";
    json += "\"asVolume\":" + String(cfg.asVolume) + ",";
    json += "\"chekwifi\":" + String(cfg.chekwifi) + ",";
    json += "\"razgonTemp\":" + String(cfg.razgonTemp) + ",";
    json += "\"bakStopTemp\":" + String(cfg.bakStopTemp) + ",";
    json += "\"midterm\":" + String(cfg.midterm) + ",";
    json += "\"midterm_abv\":" + String(cfg.midterm_abv) + ",";
    json += "\"heaterType\":" + String(cfg.heaterType) + ",";
    json += "\"fullPwr\":" + String(cfg.fullPwr ? "true" : "false") + ",";
    json += "\"valveuse\":" + String(cfg.valveuse ? "true" : "false") + ",";
    json += "\"mixerEnabled\":" + String(cfg.mixerEnabled ? "true" : "false") + ",";
    json += "\"mixerOnTime\":" + String(cfg.mixerOnTime) + ",";
    json += "\"mixerOffTime\":" + String(cfg.mixerOffTime) + ",";
    json += "\"tsaLimit\":" + String(cfg.tsaLimit) + ",";
    json += "\"cycleLim\":" + String(cfg.cycleLim) + ",";
    json += "\"histeresis\":" + String(cfg.histeresis, 2) + ",";
    json += "\"delta\":" + String(cfg.delta, 2) + ",";
    json += "\"useHeadValve\":" + String(cfg.useHeadValve ? "true" : "false") + ",";
    json += "\"bodyValveNC\":" + String(cfg.bodyValveNC ? "true" : "false") + ",";
    json += "\"headsTypeKSS\":" + String(cfg.headsTypeKSS ? "true" : "false") + ",";
    json += "\"calibration\":" + String(cfg.calibration ? "true" : "false") + ",";
    json += "\"headOpenMs\":" + String(cfg.headOpenMs) + ",";
    json += "\"headCloseMs\":" + String(cfg.headCloseMs) + ",";
    json += "\"bodyOpenMs\":" + String(cfg.bodyOpenMs) + ",";
    json += "\"bodyCloseMs\":" + String(cfg.bodyCloseMs) + ",";
    json += "\"active_test\":" + String(cfg.active_test) + ",";
    json += "\"valve_head_capacity\":" + String(cfg.valve_head_capacity) + ",";
    json += "\"valve_body_capacity\":" + String(cfg.valve_body_capacity) + ",";
    json += "\"valve0_body_capacity\":" + String(cfg.valve0_body_capacity) + ",";
    json += "\"minOpenTime\":" + String(cfg.minOpenTime) + ",";
    json += "\"speedHeadCorr\":" + String(cfg.speedHeadCorr) + ",";
    json += "\"speedBodyCorr\":" + String(cfg.speedBodyCorr) + ",";
    json += "\"cloudUrl\":\"" + String(cfg.cloudUrl) + "\",";
    json += "\"cloudApiKey\":\"" + String(cfg.cloudApiKey) + "\"";
    json += ",\"eng\":{";
    json += "\"speedGolovyBase\":" + String(cfg.speedGolovyBase) + ",";
    json += "\"speedTeloBase\":" + String(cfg.speedTeloBase) + ",";
    json += "\"distHeadsSpeed\":" + String(cfg.distHeadsSpeed) + ",";
    json += "\"shporaCorr\":" + String(cfg.shporaCorr, 2) + ",";
    json += "\"shporaStabMs\":" + String(cfg.shporaStabMs) + ",";
    json += "\"minBodySpeed\":" + String(cfg.minBodySpeed) + ",";
    json += "\"headsShareStd\":" + String(cfg.headsShareStd, 2) + ",";
    json += "\"headsShareKssSpit\":" + String(cfg.headsShareKssSpit, 2) + ",";
    json += "\"headsShareKssStd\":" + String(cfg.headsShareKssStd, 2) + ",";
    json += "\"headsShareKssAkatelo\":" + String(cfg.headsShareKssAkatelo, 2) + ",";
    json += "\"pressureCoeff\":" + String(cfg.pressureCoeff, 4) + ",";
    json += "\"coolingDurationSec\":" + String(cfg.coolingDurationSec) + ",";
    json += "\"bakstopDelaySec\":" + String(cfg.bakstopDelaySec) + ",";
    json += "\"calibDrySec\":" + String(cfg.calibDrySec) + ",";
    json += "\"calibCapacitySec\":" + String(cfg.calibCapacitySec);
    json += "}";
    json += "}";
    
    return json;
}

String AppNetwork::transliterate(String input) {
    String result = "";
    for (unsigned int i = 0; i < input.length(); i++) {
        char c = input.charAt(i);
        switch (c) {
            case 'а': result += 'a'; break;
            case 'б': result += 'b'; break;
            case 'в': result += 'v'; break;
            case 'г': result += 'g'; break;
            case 'д': result += 'd'; break;
            case 'е': result += 'e'; break;
            case 'ё': result += 'e'; break;
            case 'ж': result += 'zh'; break;
            case 'з': result += 'z'; break;
            case 'и': result += 'i'; break;
            case 'й': result += 'y'; break;
            case 'к': result += 'k'; break;
            case 'л': result += 'l'; break;
            case 'м': result += 'm'; break;
            case 'н': result += 'n'; break;
            case 'о': result += 'o'; break;
            case 'п': result += 'p'; break;
            case 'р': result += 'r'; break;
            case 'с': result += 's'; break;
            case 'т': result += 't'; break;
            case 'у': result += 'u'; break;
            case 'ф': result += 'f'; break;
            case 'х': result += 'h'; break;
            case 'ц': result += 'ts'; break;
            case 'ч': result += 'ch'; break;
            case 'ш': result += 'sh'; break;
            case 'щ': result += 'sch'; break;
            case 'ъ': result += ""; break;
            case 'ы': result += 'y'; break;
            case 'ь': result += ""; break;
            case 'э': result += 'e'; break;
            case 'ю': result += 'yu'; break;
            case 'я': result += 'ya'; break;
            default: result += c; break;
        }
    }
    return result;
}

// === PROFILE HANDLERS ===

void AppNetwork::handleSaveProfile() {
    if (!server->hasArg("plain")) {
        server->send(400, "text/plain", "Bad Request");
        return;
    }
    
    String body = server->arg("plain");
    int nameIdx = body.indexOf("\"name\":\"");
    if (nameIdx < 0) {
        server->send(400, "text/plain", "Missing name");
        return;
    }
    
    int nameStart = nameIdx + 8;
    int nameEnd = body.indexOf("\"", nameStart);
    if (nameEnd < 0) {
        server->send(400, "text/plain", "Invalid name");
        return;
    }
    
    String name = body.substring(nameStart, nameEnd);
    name.trim();
    if (name.length() == 0) {
        server->send(400, "text/plain", "Empty name");
        return;
    }
    
    // Создаём имя файла
    String filename = transliterate(name);
    filename.replace(" ", "_");
    filename.toLowerCase();
    filename = "/profiles/" + filename + ".json";
    
    // Сохраняем текущий конфиг
    SystemConfig& cfg = configManager->getConfig();
    
    String json = "{";
    json += "\"name\":\"" + name + "\",";
    json += "\"emergencyTime\":" + String(cfg.emergencyTime) + ",";
    json += "\"nasebTime\":" + String(cfg.nasebTime) + ",";
    json += "\"reklapTime\":" + String(cfg.reklapTime) + ",";
    json += "\"boxMaxTemp\":" + String(cfg.boxMaxTemp) + ",";
    json += "\"power\":" + String(cfg.power) + ",";
    json += "\"asVolume\":" + String(cfg.asVolume) + ",";
    json += "\"razgonTemp\":" + String(cfg.razgonTemp) + ",";
    json += "\"bakStopTemp\":" + String(cfg.bakStopTemp) + ",";
    json += "\"midterm\":" + String(cfg.midterm) + ",";
    json += "\"midterm_abv\":" + String(cfg.midterm_abv) + ",";
    json += "\"heaterType\":" + String(cfg.heaterType) + ",";
    json += "\"fullPwr\":" + String(cfg.fullPwr ? "true" : "false") + ",";
    json += "\"valveuse\":" + String(cfg.valveuse ? "true" : "false") + ",";
    json += "\"mixerEnabled\":" + String(cfg.mixerEnabled ? "true" : "false") + ",";
    json += "\"mixerOnTime\":" + String(cfg.mixerOnTime) + ",";
    json += "\"mixerOffTime\":" + String(cfg.mixerOffTime) + ",";
    json += "\"tsaLimit\":" + String(cfg.tsaLimit) + ",";
    json += "\"cycleLim\":" + String(cfg.cycleLim) + ",";
    json += "\"histeresis\":" + String(cfg.histeresis, 2) + ",";
    json += "\"delta\":" + String(cfg.delta, 2) + ",";
    json += "\"useHeadValve\":" + String(cfg.useHeadValve ? "true" : "false") + ",";
    json += "\"bodyValveNC\":" + String(cfg.bodyValveNC ? "true" : "false") + ",";
    json += "\"headsTypeKSS\":" + String(cfg.headsTypeKSS ? "true" : "false") + ",";
    json += "\"calibration\":" + String(cfg.calibration ? "true" : "false") + ",";
    json += "\"headOpenMs\":" + String(cfg.headOpenMs) + ",";
    json += "\"headCloseMs\":" + String(cfg.headCloseMs) + ",";
    json += "\"bodyOpenMs\":" + String(cfg.bodyOpenMs) + ",";
    json += "\"bodyCloseMs\":" + String(cfg.bodyCloseMs) + ",";
    json += "\"active_test\":" + String(cfg.active_test) + ",";
    json += "\"valve_head_capacity\":" + String(cfg.valve_head_capacity) + ",";
    json += "\"valve_body_capacity\":" + String(cfg.valve_body_capacity) + ",";
    json += "\"valve0_body_capacity\":" + String(cfg.valve0_body_capacity) + ",";
    json += "\"minOpenTime\":" + String(cfg.minOpenTime) + ",";
    json += "\"speedHeadCorr\":" + String(cfg.speedHeadCorr) + ",";
    json += "\"speedBodyCorr\":" + String(cfg.speedBodyCorr);
    json += "}";
    
    // Сохраняем файл
    SDScopeLock lock;
    File file = SD.open(filename.c_str(), FILE_WRITE);
    if (!file) {
        server->send(500, "text/plain", "Failed to create file");
        return;
    }
    file.print(json);
    file.close();
    
    Serial.println("[Profile] Saved: " + filename);
    logger.log("[Profile] Saved: " + name);
    
    server->send(200, "text/plain", "Profile saved: " + name);
}

void AppNetwork::handleListProfiles() {
    String json = "{\"profiles\":[";
    
    SDScopeLock lock;
    
    File profilesDir = SD.open("/profiles");
    if (!profilesDir || !profilesDir.isDirectory()) {
        json += "]}";
        server->send(200, "application/json", json);
        return;
    }
    
    bool first = true;
    File file = profilesDir.openNextFile();
    while (file) {
        if (!file.isDirectory() && String(file.name()).endsWith(".json")) {
            // Читаем имя профиля из файла
            String filename = String(file.name());
            
            // Ищем "name" в файле
            String name = filename.substring(0, filename.length() - 5); // убираем .json
            
            // Простой парсинг имени
            String content = "";
            while (file.available()) {
                content += (char)file.read();
            }
            
            int nameIdx = content.indexOf("\"name\":\"");
            if (nameIdx >= 0) {
                int nameStart = nameIdx + 8;
                int nameEnd = content.indexOf("\"", nameStart);
                if (nameEnd > nameStart) {
                    name = content.substring(nameStart, nameEnd);
                }
            }
            
            if (!first) json += ",";
            json += "{\"file\":\"" + filename + "\",\"name\":\"" + name + "\"}";
            first = false;
        }
        file = profilesDir.openNextFile();
    }
    
    json += "]}";
    server->send(200, "application/json", json);
}

void AppNetwork::handleLoadProfile() {
    if (!server->hasArg("plain")) {
        server->send(400, "text/plain", "Bad Request");
        return;
    }
    
    String body = server->arg("plain");
    int fileIdx = body.indexOf("\"file\":\"");
    if (fileIdx < 0) {
        server->send(400, "text/plain", "Missing file");
        return;
    }
    
    int fileStart = fileIdx + 8;
    int fileEnd = body.indexOf("\"", fileStart);
    if (fileEnd < 0) {
        server->send(400, "text/plain", "Invalid file");
        return;
    }
    
    String filename = body.substring(fileStart, fileEnd);
    filename.trim();
    if (!filename.endsWith(".json")) {
        filename += ".json";
    }
    filename = "/profiles/" + filename;
    
    SDScopeLock lock;
    File file = SD.open(filename.c_str());
    if (!file) {
        server->send(404, "text/plain", "Profile not found");
        return;
    }
    
    String content = "";
    while (file.available()) {
        content += (char)file.read();
    }
    file.close();
    
    // Парсим и применяем настройки
    SystemConfig& cfg = configManager->getConfig();
    
    auto getInt = [&](const char* key, int defaultVal) -> int {
        String searchKey = "\"" + String(key) + "\":";
        int pos = content.indexOf(searchKey);
        if (pos != -1) {
            int start = pos + searchKey.length();
            int end = content.indexOf(',', start);
            if (end == -1) end = content.indexOf('}', start);
            String valStr = content.substring(start, end);
            valStr.trim();
            if (valStr.equalsIgnoreCase("true")) return 1;
            if (valStr.equalsIgnoreCase("false")) return 0;
            return valStr.toInt();
        }
        return defaultVal;
    };
    
    auto getFloat = [&](const char* key, float defaultVal) -> float {
        String searchKey = "\"" + String(key) + "\":";
        int pos = content.indexOf(searchKey);
        if (pos != -1) {
            int start = pos + searchKey.length();
            int end = content.indexOf(',', start);
            if (end == -1) end = content.indexOf('}', start);
            String valStr = content.substring(start, end);
            return valStr.toFloat();
        }
        return defaultVal;
    };
    
    auto getBool = [&](const char* key, bool defaultVal) -> bool {
        String searchKey = "\"" + String(key) + "\":";
        int pos = content.indexOf(searchKey);
        if (pos != -1) {
            int start = pos + searchKey.length();
            int end = content.indexOf(',', start);
            if (end == -1) end = content.indexOf('}', start);
            String valStr = content.substring(start, end);
            valStr.trim();
            return valStr == "1" || valStr.equalsIgnoreCase("true");
        }
        return defaultVal;
    };
    
    cfg.emergencyTime = getInt("emergencyTime", cfg.emergencyTime);
    cfg.nasebTime = getInt("nasebTime", cfg.nasebTime);
    cfg.reklapTime = getInt("reklapTime", cfg.reklapTime);
    cfg.boxMaxTemp = getInt("boxMaxTemp", cfg.boxMaxTemp);
    cfg.power = getInt("power", cfg.power);
    cfg.asVolume = getInt("asVolume", cfg.asVolume);
    cfg.razgonTemp = getInt("razgonTemp", cfg.razgonTemp);
    cfg.bakStopTemp = getInt("bakStopTemp", cfg.bakStopTemp);
    cfg.midterm = getInt("midterm", cfg.midterm);
    cfg.midterm_abv = getInt("midterm_abv", cfg.midterm_abv);
    cfg.heaterType = getInt("heaterType", cfg.heaterType);
    cfg.fullPwr = getBool("fullPwr", cfg.fullPwr);
    cfg.valveuse = getBool("valveuse", cfg.valveuse);
    cfg.mixerEnabled = getBool("mixerEnabled", cfg.mixerEnabled);
    cfg.mixerOnTime = getInt("mixerOnTime", cfg.mixerOnTime);
    cfg.mixerOffTime = getInt("mixerOffTime", cfg.mixerOffTime);
    cfg.tsaLimit = getInt("tsaLimit", cfg.tsaLimit);
    cfg.cycleLim = getInt("cycleLim", cfg.cycleLim);
    cfg.histeresis = getFloat("histeresis", cfg.histeresis);
    cfg.delta = getFloat("delta", cfg.delta);
    cfg.useHeadValve = getBool("useHeadValve", cfg.useHeadValve);
    cfg.bodyValveNC = getBool("bodyValveNC", cfg.bodyValveNC);
    cfg.headsTypeKSS = getBool("headsTypeKSS", cfg.headsTypeKSS);
    cfg.calibration = getBool("calibration", cfg.calibration);
    cfg.headOpenMs = getInt("headOpenMs", cfg.headOpenMs);
    cfg.headCloseMs = getInt("headCloseMs", cfg.headCloseMs);
    cfg.bodyOpenMs = getInt("bodyOpenMs", cfg.bodyOpenMs);
    cfg.bodyCloseMs = getInt("bodyCloseMs", cfg.bodyCloseMs);
    cfg.active_test = getInt("active_test", cfg.active_test);
    cfg.valve_head_capacity = getInt("valve_head_capacity", cfg.valve_head_capacity);
    cfg.valve_body_capacity = getInt("valve_body_capacity", cfg.valve_body_capacity);
    cfg.valve0_body_capacity = getInt("valve0_body_capacity", cfg.valve0_body_capacity);
    cfg.minOpenTime = getInt("minOpenTime", cfg.minOpenTime);
    cfg.speedHeadCorr = getInt("speedHeadCorr", cfg.speedHeadCorr);
    cfg.speedBodyCorr = getInt("speedBodyCorr", cfg.speedBodyCorr);
    
    configManager->saveConfig();
    configManager->saveDistConfig();
    configManager->saveRectConfig();
    
    // Перезагружаем конфиг в память чтобы ProcessEngine увидел изменения
    configManager->loadConfig();
    
    Serial.println("[Profile] Loaded: " + filename);
    logger.log("[Profile] Loaded: " + filename);
    
    server->send(200, "text/plain", "Profile loaded");
}

void AppNetwork::handleCalcValve() {
    if (!server->hasArg("plain")) {
        server->send(400, "text/plain", "Bad Request");
        return;
    }
    
    String body = server->arg("plain");
    
    // Парсим ml и type
    int mlIdx = body.indexOf("\"ml\":");
    int typeIdx = body.indexOf("\"type\":\"");
    
    if (mlIdx < 0 || typeIdx < 0) {
        server->send(400, "text/plain", "Missing ml or type");
        return;
    }
    
    int mlStart = mlIdx + 5;
    int mlEnd = body.indexOf(',', mlStart);
    if (mlEnd < 0) mlEnd = body.indexOf('}', mlStart);
    String mlStr = body.substring(mlStart, mlEnd);
    mlStr.trim();
    float ml = mlStr.toFloat();
    
    int typeStart = typeIdx + 8;
    int typeEnd = body.indexOf("\"", typeStart);
    String type = body.substring(typeStart, typeEnd);
    
    // Расчёт capacity
    int testDuration = configManager->getConfig().active_test;
    float capacity = ml / (testDuration / 60.0f);  // мл/мин
    
    // Сохраняем в конфиг
    SystemConfig& cfg = configManager->getConfig();
    if (type == "head") {
        cfg.valve_head_capacity = (int)capacity;
    } else {
        cfg.valve_body_capacity = (int)capacity;
    }
    configManager->saveRectConfig();
    
    Serial.printf("[CalcValve] type=%s, ml=%.1f, capacity=%.1f ml/min\n", type.c_str(), ml, capacity);
    logger.log("[CalcValve] " + type + ": " + String(ml, 1) + "ml -> " + String(capacity, 1) + " ml/min");
    
    String json = "{\"capacity\":" + String(capacity, 1) + "}";
    server->send(200, "application/json", json);
}
