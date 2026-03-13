#include "AppNetwork.h"
#include "config.h"
// Подключаем ProcessEngine, чтобы брать из него данные
#include "ProcessEngine.h" 
#include "preferences.h"
#include "SDLogger.h" // <--- Добавить эту строку
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
// Подключаем внешний объект логера
extern SDLogger logger;

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

void AppNetwork::begin(int checkIntervalMinutes) {
    checkIntervalMs = checkIntervalMinutes * 60000;

    // 1. Инициализация SD карты
    if(!initSD()) {
        Serial.println("[NetMgr] Switching to OFFLINE mode.");
        return;
    }

    // 2. Чтение конфигурации WiFi
    if (!loadConfigFromSD()) {
        Serial.println("[NetMgr] Config load failed! Switching to OFFLINE mode.");
        return;
    }
    
    // 3. Попытка подключения
    Serial.println("[NetMgr] Connecting...");
    if (connectToWiFi()) {
        // WiFi подключен к роутеру
        wifiConnected = true;
        networkMode = NetworkMode::STA_MODE;  // Режим станции (роутер)
        
        // Проверяем интернет
        online = checkInternet();
        
        if (online) {
            // Интернет есть - запускаем Telegram и NTP
            client.setCACert(TELEGRAM_CERTIFICATE_ROOT); 
            
            if (tgToken.length() > 0) {
                bot = new UniversalTelegramBot(tgToken, client);
            }
            
            syncNTP();
            Serial.println("[NetMgr] Internet OK. Telegram/NTP enabled.");
        } else {
            Serial.println("[NetMgr] No internet. Web only mode.");
        }
        
        // ================== WEB SERVER SETUP (ОДИН РАЗ) ==================
        server = new WebServer(80);

        // API Endpoints
        server->on("/api/status", [this]() { handleApiStatus(); });
        server->on("/api/command", HTTP_POST, [this]() { handleApiCommand(); });
        server->on("/api/settings", HTTP_POST, [this]() { handleApiSettings(); });
        server->on("/api/calcvalve", HTTP_POST, [this]() { handleCalcValve(); });

        server->on("/api/saveprofile",  HTTP_POST, [this]() { handleSaveProfile(); });
        server->on("/api/listprofiles", HTTP_GET,  [this]() { handleListProfiles(); });
        server->on("/api/loadprofile",  HTTP_POST, [this]() { handleLoadProfile(); });
                // === API ДЛЯ ЛОГОВ ===
        server->on("/api/logs", HTTP_GET, [this]() {
            String logContent = logger.readLastLog();
            server->send(200, "text/plain", logContent);
        });

        // Главная страница
        server->on("/", HTTP_GET, [this]() {
            File file = SD.open("/www/index.html", "r");
            if (file) {
                server->streamFile(file, "text/html");
                file.close();
            } else {
                server->send(404, "text/plain", "File Not Found: index.html");
            }
        });

        // Статика
        server->serveStatic("/", SD, "/www/");

        server->onNotFound([this]() {
            server->send(404, "text/plain", "Not Found");
        });
        
        server->begin();
        Serial.println("[NetMgr] HTTP Server started");
        logger.log("---------------------------");
        logger.log("Network: Online");
        
        // Стартовое сообщение в Telegram
        String msg = "System " + String(FIRMWARE_VERSION) + " started. IP: " + WiFi.localIP().toString();
        sendMessage(msg);
        
    } else {
        // WiFi не подключился - пробуем AP режим
        Serial.println("[NetMgr] WiFi Connection Failed. Trying AP mode...");
        logger.log("Network: WiFi Failed, trying AP...");
        
        if (startAPMode()) {
            networkMode = NetworkMode::AP_MODE;
            logger.log("Network: AP Mode started");
        } else {
            networkMode = NetworkMode::OFFLINE;
            Serial.println("[NetMgr] AP Mode Failed. Full OFFLINE mode.");
            logger.log("Network: Full OFFLINE");
        }
    }
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
    
    // ================== DNS SERVER (Captive Portal) ==================
    // Удаляем старый DNS сервер если есть
    if (dnsServer) {
        dnsServer->stop();
        delete dnsServer;
    }
    // DNS сервер перенаправляет все запросы на наш IP
    // Это позволяет открывать страницу даже если клиент вводит любой домен
    dnsServer = new DNSServer();
    dnsServer->start(53, "*", WiFi.softAPIP());  // Порт 53, перенаправляем всё на наш IP
    Serial.println("[NetMgr] DNS Server started (Captive Portal)");
    
    // ================== WEB SERVER SETUP (AP MODE) ==================
    // Удаляем старый сервер если есть (защита от утечки памяти)
    if (server) {
        server->stop();
        delete server;
    }
    server = new WebServer(80);

    // API Endpoints
    server->on("/api/status", [this]() { handleApiStatus(); });
    server->on("/api/command", HTTP_POST, [this]() { handleApiCommand(); });
    server->on("/api/settings", HTTP_POST, [this]() { handleApiSettings(); });
    server->on("/api/calcvalve", HTTP_POST, [this]() { handleCalcValve(); });

    server->on("/api/saveprofile",  HTTP_POST, [this]() { handleSaveProfile(); });
    server->on("/api/listprofiles", HTTP_GET,  [this]() { handleListProfiles(); });
    server->on("/api/loadprofile",  HTTP_POST, [this]() { handleLoadProfile(); });
    
    // API для логов
    server->on("/api/logs", HTTP_GET, [this]() {
        String logContent = logger.readLastLog();
        server->send(200, "text/plain", logContent);
    });

    // Главная страница
    server->on("/", HTTP_GET, [this]() {
        File file = SD.open("/www/index.html", "r");
        if (file) {
            server->streamFile(file, "text/html");
            file.close();
        } else {
            server->send(404, "text/plain", "File Not Found: index.html");
        }
    });

    // Статика
    server->serveStatic("/", SD, "/www/");

    server->onNotFound([this]() {
        server->send(404, "text/plain", "Not Found");
    });
    
    server->begin();
    Serial.println("[NetMgr] HTTP Server started (AP mode)");
    logger.log("---------------------------");
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


void AppNetwork::update() {
    if (!server) return; 
    unsigned long now = millis();
    
    // === ПЕРВЫМ ДЕЛОМ: WebServer (самый приоритетный!) ===
    server->handleClient();
    
    // === DNS SERVER (только в AP режиме) ===
    // Обрабатываем ВСЕ DNS запросы за один цикл для быстродействия
    if (dnsServer && networkMode == NetworkMode::AP_MODE) {
        int dnsProcessed = 0;
        while (dnsServer->processNextRequest() && dnsProcessed < 10) {
            dnsProcessed++;
        }
    }
    
    // === ПЕРИОДИЧЕСКАЯ ПРОВЕРКА СЕТИ (только в STA режиме) ===
    if (now - lastCheckTime > checkIntervalMs || lastCheckTime == 0) {
        lastCheckTime = now;
        
        if (networkMode == NetworkMode::STA_MODE) {
            // === РЕЖИМ STA: проверяем WiFi и интернет отдельно ===
            
            // 1. Проверка WiFi соединения с роутером
            if (WiFi.status() != WL_CONNECTED) {
                // WiFi отвалился - пробуем переподключиться
                Serial.println("[NetMgr] WiFi lost. Reconnecting...");
                if (connectToWiFi()) {
                    Serial.println("[NetMgr] WiFi reconnected.");
                } else {
                    // WiFi не восстановился - переключаемся в AP режим
                    Serial.println("[NetMgr] WiFi reconnect failed. Switching to AP...");
                    if (startAPMode()) {
                        networkMode = NetworkMode::AP_MODE;
                    } else {
                        networkMode = NetworkMode::OFFLINE;
                    }
                    return;
                }
            }
            
            // 2. Проверка интернета (только для Telegram/NTP)
            bool hasInternet = checkInternet();
            if (hasInternet && !online) {
                // Интернет восстановился
                online = true;
                syncNTP();
                sendMessage("Connection restored.");
                Serial.println("[NetMgr] Internet restored.");
            } else if (!hasInternet && online) {
                // Интернет потерян, но WiFi работает
                online = false;
                Serial.println("[NetMgr] Internet lost. Web continues, Telegram disabled.");
            }
            
        } else if (networkMode == NetworkMode::AP_MODE) {
            // === РЕЖИМ AP: работаем до перезагрузки ===
            // Попытки подключения к роутеру убраны - ESP32 не может одновременно
            // работать как AP и STA. Пользователь должен исправить настройки
            // WiFi и перезагрузить систему.
            // Web интерфейс доступен по http://192.168.4.1
        }
        // OFFLINE режим - ничего не проверяем
    }

    // === ПЕРИОДИЧЕСКИЕ УВЕДОМЛЕНИЯ TELEGRAM (Аварии) ===
    // Проверяем каждый цикл, но шлем только по таймеру
    if (processEngine && online) {
        const SystemStatus& status = processEngine->getStatus();
        // Получаем данные датчиков, если метод доступен (он нужен для Box Temp)
        // Если getSensorData() нет в ProcessEngine, можно передать температуру через SystemStatus
        // Предположим, что getSensorData есть (мы добавляли её ранее)
        const SensorData& sensors = processEngine->getSensorData(); 

        // 1. Авария TSA (каждые 60 сек) - увеличено для предотвращения блокировки интерфейса
        if (status.safety == SafetyState::WARNING_TSA || status.safety == SafetyState::EMERGENCY) {
            if (now - lastAlarmTgTime > 60000) { // 60 секунд (было 10)
                lastAlarmTgTime = now;
                String msg = "🔥 АВАРИЯ! TSA: " + String(status.currentTsa, 1) + "C (Limit: " + String(configManager->getConfig().tsaLimit) + "C)";
                sendMessage(msg);
            }
        }
        // 2. Внимание BOX (каждые 30 сек)
        else if (status.safety == SafetyState::WARNING_BOX) {
            if (now - lastAlarmTgTime > 30000) { // 30 секунд
                lastAlarmTgTime = now;
                String msg = "⚠️ ВНИМАНИЕ! Перегрев бокса: " + String(sensors.boxTemp, 1) + "C";
                sendMessage(msg);
            }
        }
        // Сброс таймера, когда все нормализовалось
        else {
            // Если аварии нет, сбрасываем таймер, чтобы при следующей аварии счет пошел с нуля
            lastAlarmTgTime = 0;
        }
    }
    
    // === ОБРАБОТКА ОЧЕРЕДИ TELEGRAM (после WebServer!) ===
    processMessageQueue();
}

// ================== API HANDLERS ==================

void AppNetwork::handleApiStatus() {
    if (!processEngine || !configManager) {
        server->send(500, "application/json", "{\"error\":\"Engine not linked\"}");
        return;
    }

    // 1. Получаем данные
    const SystemStatus& status = processEngine->getStatus();
    const SensorData& sensors = processEngine->getSensorData(); 

    SystemConfig& cfg = configManager->getConfig();

    // 2. Формируем JSON вручную
    String json = "{";

    // --- Сеть и Время ---
    json += "\"online\":" + String(online ? "true" : "false") + ",";
    json += "\"network_mode\":\"" + String(getNetworkSymbol()) + "\",";  // W/A/X
    json += "\"time\":" + String(status.processTimeSec) + ",";
    
    // --- Статус системы (Safety) ---
    int safetyCode = 0;
    String safetyText = "НОРМА";
    if (status.safety == SafetyState::WARNING_BOX) { safetyCode = 1; safetyText = "ВНИМАНИЕ"; }
    else if (status.safety == SafetyState::WARNING_TSA) { safetyCode = 2; safetyText = "АВАРИЯ"; }
    else if (status.safety == SafetyState::EMERGENCY) { safetyCode = 3; safetyText = "АВАРИЯ"; }
    
    json += "\"safety_code\":" + String(safetyCode) + ",";
    json += "\"safety_text\":\"" + safetyText + "\",";

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

    // === НОВЫЕ ДАННЫЕ ДЛЯ WEB (RECT INFO) - ВЫНЕСЕНЫ ИЗ CFG ===
    json += "\"rectMethodName\":\"" + status.rectMethodName + "\",";
    json += "\"rectSubStage\":\"" + status.rectSubStage + "\",";
    json += "\"rectTimeRemaining\":" + String(status.rectTimeRemaining) + ",";
    json += "\"rectVolumeTarget\":" + String(status.rectVolumeTarget) + ",";
    json += "\"bodyMethodName\":\"" + status.bodyMethodName + "\",";
    json += "\"bodySpeed\":" + String(status.bodySpeed, 1) + ",";
    json += "\"headsSpeed\":" + String(status.headsSpeed, 1) + ","; // скорость голов (отдельно от тела)
    json += "\"bodyCycle\":" + String(status.bodyCycle) + ",";
    // ===========================================================
    // === Накопленный объём голов и остаток таймера завершения ===
    json += "\"headsVolDone\":" + String(status.headsVolDone, 0) + ",";
    json += "\"finishingRemainSec\":" + String(status.finishingRemainSec) + ",";
    // ===========================================================
    // === Статусы выходов для блока СИСТЕМА ===
    json += "\"heaterOn\":"       + String(processEngine->isHeaterOn()       ? "true" : "false") + ",";
    json += "\"mixerOn\":"        + String(processEngine->isMixerOn()        ? "true" : "false") + ",";
    json += "\"waterValveOpen\":" + String(processEngine->isWaterValveOpen() ? "true" : "false") + ",";
    // ===========================================================
    
    // === Референтные значения для TELO (запоминаются перед этапом) ===
    json += "\"rtsarM\":" + String(processEngine->getRtsarM(), 2) + ",";
    json += "\"adPressM\":" + String(processEngine->getAdPressM(), 1) + ",";
    // ===========================================================
    
    // === Общий объём голов для этапа (KSS=20%, Standard=10%) ===
    float headsTotalTarget = cfg.headsTypeKSS ? (cfg.asVolume * 0.20f) : (cfg.asVolume * 0.10f);
    json += "\"headsTotalTarget\":" + String((int)headsTotalTarget) + ",";
    // ===========================================================


    // --- Настройки (Config) ---
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
    
    json += "}"; // Конец главного объекта
    
    server->send(200, "application/json", json);
}

void AppNetwork::handleApiCommand() {
    if (!server->hasArg("plain")) {
        server->send(400, "text/plain", "Bad Request");
        return;
    }

    String body = server->arg("plain");
    Serial.println("[API] Command: " + body);

    auto sendCmd = [](UiCommand cmd) {
        CommandMessage msg = { cmd };
        xQueueSend(commandQueue, &msg, 0);
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
    else if (body.indexOf("\"cmd\":\"IDENTIFY_") > 0) {
        // === ИСПРАВЛЕНО: Проверяем TSAR первым, так как он содержит TSA ===
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

    // Получаем ссылку на конфигурацию для изменения
    SystemConfig& cfg = configManager->getConfig();

    // === ПАРСИНГ JSON ВРУЧНУЮ (без ArduinoJson) ===
    
        // Вспомогательная лямбда-функция для поиска целых чисел
    auto getInt = [&](const char* key) -> int {
        String searchKey = "\"" + String(key) + "\":";
        int pos = body.indexOf(searchKey);
        if (pos != -1) {
            int start = pos + searchKey.length();
            int end = body.indexOf(',', start);
            if (end == -1) end = body.indexOf('}', start); // Если последний элемент
            String valStr = body.substring(start, end);
            valStr.trim();
            
            // ИСПРАВЛЕНО: Проверяем текстовые булевы значения, которые мог прислать Web
            if (valStr.equalsIgnoreCase("true")) return 1;
            if (valStr.equalsIgnoreCase("false")) return 0;
            
            return valStr.toInt();
        }
        return 0; // Значение по умолчанию, если не нашли
    };

    // Вспомогательная функция для поиска float
    auto getFloat = [&](const char* key) -> float {
        String searchKey = "\"" + String(key) + "\":";
        int pos = body.indexOf(searchKey);
        if (pos != -1) {
            int start = pos + searchKey.length();
            int end = body.indexOf(',', start);
            if (end == -1) end = body.indexOf('}', start);
            String valStr = body.substring(start, end);
            return valStr.toFloat();
        }
        return 0.0f;
    };

       // Вспомогательная функция для поиска bool (true/false)
    auto getBool = [&](const char* key) -> bool {
        String searchKey = "\"" + String(key) + "\":";
        int pos = body.indexOf(searchKey);
        if (pos != -1) {
            int start = pos + searchKey.length();
            int end = body.indexOf(',', start);
            if (end == -1) end = body.indexOf('}', start);
            String valStr = body.substring(start, end);
            valStr.trim();
            
            // ИСПРАВЛЕНО: Анализируем только конкретное значение, а не всю строку дальше
            if (valStr == "1" || valStr.equalsIgnoreCase("true")) return true;
            return false;
        }
        return false;
    };

    // === ОБНОВЛЕНИЕ ПОЛЕЙ (СООТВЕТСТВУЕТ ТВОИМ .h ФАЙЛАМ) ===

    // 1. Общие настройки (menu_settings.h)
    cfg.emergencyTime = getInt("emergencyTime");
    cfg.nasebTime = getInt("nasebTime");
    cfg.reklapTime = getInt("reklapTime");
    cfg.boxMaxTemp = getInt("boxMaxTemp");
    cfg.power = getInt("power");
    cfg.asVolume = getInt("asVolume");
    cfg.chekwifi = getInt("chekwifi");

            // 2. Дистилляция (menu_dist_setup.h)
    cfg.razgonTemp = getInt("razgonTemp");
    cfg.bakStopTemp = getInt("bakStopTemp");

    // === СМЕНА ТАРЫ: УМНАЯ ЛОГИКА ПРИОРИТЕТОВ ===
    int newMidtermAbv = getInt("midterm_abv");
    int newMidterm    = getInt("midterm");
    
    // Запоминаем, что было ДО редактирования (cfg еще не обновился)
    int oldMidterm = cfg.midterm;
    
    // Получаем давление
    const SensorData& sensors = processEngine->getSensorData();
    float pressure_hPa = sensors.pressure; 
    float pressure_mmHg = pressure_hPa * 0.75006f;

    // ЛОГИКА:
    // 1. Если ТЕМПЕРАТУРА изменилась (пользователь крутит градусы) -> 
    //    Считаем, что температура в приоритете. Пересчитываем %.
    if (newMidterm != oldMidterm) {
        cfg.midterm = newMidterm;
        int calcAbv = (int)round(configManager->getOutputABVForTemp((float)newMidterm, pressure_mmHg));
        cfg.midterm_abv = calcAbv; 
        Serial.printf("[Dist] Priority TEMP: %dC -> ABV=%d%% (P=%.1f mmHg)\n", cfg.midterm, cfg.midterm_abv, pressure_mmHg);
    } 
    // 2. Если температура та же, но КРЕПОСТЬ задана (>0) ->
    //    Считаем, что приоритет у %. Пересчитываем градусы.
    else if (newMidtermAbv > 0) {
        cfg.midterm_abv = newMidtermAbv;
        cfg.midterm = (int)round(configManager->getTempForOutputABV((float)newMidtermAbv, pressure_mmHg));
        Serial.printf("[Dist] Priority ABV: %d%% -> T=%dC (P=%.1f mmHg)\n", cfg.midterm_abv, cfg.midterm, pressure_mmHg);
    }
    // 3. Если ничего не меняли (или выкл %) -> просто сохраняем темп.
    else {
        cfg.midterm = newMidterm;
        cfg.midterm_abv = 0;
    }
    // ==========================================

    
    cfg.heaterType = getInt("heaterType"); // 0 или 1
    cfg.fullPwr = getBool("fullPwr");
    cfg.valveuse = getBool("valveuse");
    cfg.mixerEnabled = getBool("mixerEnabled");
    cfg.mixerOnTime = getInt("mixerOnTime");
    cfg.mixerOffTime = getInt("mixerOffTime");

    // 3. Ректификация (menu_rect_setup.h)
    cfg.tsaLimit = getInt("tsaLimit");
    cfg.cycleLim = getInt("cycleLim");
    cfg.histeresis = getFloat("histeresis");
    cfg.delta = getFloat("delta");
    cfg.useHeadValve = getBool("useHeadValve");
    cfg.bodyValveNC = getBool("bodyValveNC");
    cfg.headsTypeKSS = getBool("headsTypeKSS");
    cfg.calibration = getBool("calibration");
    
    // 4. Клапана (valve_cal_menu.h)
    cfg.headOpenMs = getInt("headOpenMs");
    cfg.headCloseMs = getInt("headCloseMs");
    cfg.bodyOpenMs = getInt("bodyOpenMs");
    cfg.bodyCloseMs = getInt("bodyCloseMs");
    cfg.active_test = getInt("active_test");
    cfg.valve_head_capacity = getInt("valve_head_capacity");
    cfg.valve_body_capacity = getInt("valve_body_capacity");
    cfg.valve0_body_capacity = getInt("valve0_body_capacity");

    // === СОХРАНЕНИЕ ===
    // Сохраняем все секции (или можно вызывать конкретные, если нужно оптимизировать ресурсы)
    configManager->saveConfig();      // Общие
    configManager->saveDistConfig();  // Дист
    configManager->saveRectConfig();  // Рект
    
    Serial.println("[API] Settings SAVED to EEPROM.");
    
    server->send(200, "text/plain", "Settings Saved");
}
// --- Реализация методов ---

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

bool AppNetwork::connectToWiFi() {
    // Попытка к первой сети (3 раза)
    for (int i = 0; i < 3; i++) {
        Serial.printf("[NetMgr] Try %s (Attempt %d)\n", ssid1.c_str(), i+1);
        WiFi.begin(ssid1.c_str(), pass1.c_str());
        
        int tries = 0;
        while (WiFi.status() != WL_CONNECTED && tries < 30) {
            delay(100);
            // === ВАЖНО: Не блокируем WebServer надолго! ===
            if (server) server->handleClient();
            yield();
            // ============================================
            tries++;
        }
        
        if (WiFi.status() == WL_CONNECTED) return true;
        delay(100);  // Сокращено с 1000
        if (server) server->handleClient();
        yield();
    }
    
    // Попытка ко второй сети
    if (ssid2.length() > 0) {
        for (int i = 0; i < 3; i++) {
            Serial.printf("[NetMgr] Try %s (Attempt %d)\n", ssid2.c_str(), i+1);
            WiFi.begin(ssid2.c_str(), pass2.c_str());
            
            int tries = 0;
            while (WiFi.status() != WL_CONNECTED && tries < 30) {
                delay(100);
                // === ВАЖНО: Не блокируем WebServer надолго! ===
                if (server) server->handleClient();
                yield();
                // ============================================
                tries++;
            }
            
            if (WiFi.status() == WL_CONNECTED) return true;
            delay(100);  // Сокращено с 1000
            if (server) server->handleClient();
            yield();
        }
    }
    
    return false;
}

void AppNetwork::syncNTP() {
    Serial.println("[NetMgr] Syncing NTP...");
    configTime(3 * 3600, 0, "pool.ntp.org", "time.nist.gov");
    
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)){
        Serial.println("[NetMgr] NTP Failed");
        return;
    }
    Serial.println("[NetMgr] NTP Sync OK");
}

bool AppNetwork::checkInternet() {
    // === НЕБЛОКИРУЮЩАЯ проверка интернета ===
    // Таймаут 2 сек, handleClient() внутри цикла
    WiFiClient testClient;
    testClient.setTimeout(2000);  // 2 секунды максимум

    // Пытаемся подключиться, но не блокируем надолго
    // 20 итераций по 100мс = 2 секунды
    for (int i = 0; i < 20; i++) {
        if (testClient.connect("google.com", 80)) {
            testClient.stop();
            Serial.println("[NetMgr] Internet check: OK");
            return true;
        }

        // === ВАЖНО: Обслуживаем WebServer во время ожидания ===
        if (server) server->handleClient();
        delay(100);
        yield();
    }

    testClient.stop();
    Serial.println("[NetMgr] Internet check: FAILED (timeout)");
    return false;
}

// === АСИНХРОННАЯ ОЧЕРЕДЬ СООБЩЕНИЙ TELEGRAM ===

// Публичный метод: добавляет сообщение в очередь (неблокирующий)
void AppNetwork::sendMessage(const String& text) {
    queueMessage(text);
}

// Добавление сообщения в очередь
void AppNetwork::queueMessage(const String& text) {
    if (!online || !bot || tgToken.length() == 0) {
        Serial.println("[TG] Offline or no token - message dropped");
        return;
    }
    
    if (tgQueueCount >= TG_QUEUE_SIZE) {
        Serial.println("[TG] Queue FULL! Dropping oldest message");
        // Удаляем самое старое сообщение (сдвигаем tail)
        tgQueueTail = (tgQueueTail + 1) % TG_QUEUE_SIZE;
        tgQueueCount--;
    }
    
    // Добавляем в очередь
    tgQueue[tgQueueHead].text = text;
    tgQueue[tgQueueHead].timestamp = millis();
    tgQueueHead = (tgQueueHead + 1) % TG_QUEUE_SIZE;
    tgQueueCount++;
    
    Serial.println("[TG] Queued: " + text + " (queue: " + String(tgQueueCount) + ")");
}

// Проверка готовности к отправке
bool AppNetwork::isTelegramReady() {
    if (!online || !bot) return false;
    
    unsigned long now = millis();
    
    // Если были неудачи, ждём паузу перед повтором
    if (tgConsecutiveFails > 0 && (now - lastTgFailTime) < TG_RETRY_DELAY) {
        return false;
    }
    
    // Минимальный интервал между отправками (500мс)
    if ((now - lastTgSendTime) < 500) {
        return false;
    }
    
    return true;
}

// Проверка соединения с Telegram API
bool AppNetwork::sendTelegramNow(const String& text) {
    if (!online || !bot) return false;
    
    client.setTimeout(1);
    Serial.println("[TG] Sending: " + text);
    
    if (server) server->handleClient();
    yield();
    
    // Отправляем сообщение (игнорируем return - библиотека возвращает false при timeout ответа)
    bot->sendMessage(tgChatId, text, "");
    
    if (server) server->handleClient();
    yield();
    
    Serial.println("[TG] Sent OK");
    return true;  // Всегда true - сообщение отправлено
}

// Обработка очереди (вызывается из update() - неблокирующая!)
void AppNetwork::processMessageQueue() {
    // Нет сообщений или не готовы - выходим сразу
    if (tgQueueCount == 0) return;
    if (!isTelegramReady()) return;
    if (tgSending) return;  // Уже идёт отправка (защита от повторного входа)

    // Берём сообщение из головы очереди
    String msg = tgQueue[tgQueueTail].text;

    tgSending = true;

    bool success = sendTelegramNow(msg);

    if (success) {
        // Успех - удаляем из очереди
        tgQueueTail = (tgQueueTail + 1) % TG_QUEUE_SIZE;
        tgQueueCount--;
        lastTgSendTime = millis();
        tgConsecutiveFails = 0;
        Serial.println("[TG] Sent OK (remaining: " + String(tgQueueCount) + ")");
    } else {
        // Неудача - оставляем в очереди для повторной попытки
        tgConsecutiveFails++;
        lastTgFailTime = millis();
        Serial.println("[TG] Send FAILED (attempt " + String(tgConsecutiveFails) + ")");
        
        // После 3 неудач подряд - сбрасываем очередь (чтобы не копилась)
        if (tgConsecutiveFails >= 3 && tgQueueCount > 1) {
            Serial.println("[TG] Too many fails - dropping oldest message");
            tgQueueTail = (tgQueueTail + 1) % TG_QUEUE_SIZE;
            tgQueueCount--;
            tgConsecutiveFails = 0;
        }
    }

    tgSending = false;
}
// ==============================================

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

// Статическая обёртка для FreeRTOS (нужна т.к. xTaskCreatePinnedToCore
// принимает C-функцию, не метод класса)
static void networkTaskWrapper(void* param) {
    AppNetwork* self = static_cast<AppNetwork*>(param);
    for (;;) {
        self->update();
        // В AP режиме минимальная задержка для быстрого Web-интерфейса
        // В STA режиме можно больше, т.к. сеть стабильна
        if (self->getNetworkMode() == NetworkMode::AP_MODE) {
            vTaskDelay(pdMS_TO_TICKS(1));  // 1мс - быстро для AP
        } else {
            vTaskDelay(pdMS_TO_TICKS(10)); // 10мс - нормально для STA
        }
    }
}

void AppNetwork::startTask() {
    xTaskCreatePinnedToCore(
        networkTaskWrapper,   // функция таска
        "NetworkTask",        // имя (для отладки)
        8192,                 // стек (байт) — сеть требует много
        this,                 // параметр → передаём себя
        1,                    // приоритет (1 = низкий, не мешаем Core 1)
        &networkTaskHandle,   // хендл
        0                     // Core 0
    );
}
void AppNetwork::handleCalcValve() {
    if (!server->hasArg("plain")) {
        server->send(400, "text/plain", "Bad Request");
        return;
    }

    String body = server->arg("plain");
    // Ожидаем JSON: {"type":"head","ml":15.5}
    // или           {"type":"body_nc","ml":12.0}
    // или           {"type":"body_no","ml":10.0}

    // Парсинг ml
    float ml = 0;
    int mlIdx = body.indexOf("\"ml\":");
    if (mlIdx > 0) ml = body.substring(mlIdx + 5).toFloat();

    if (ml <= 0) {
        server->send(400, "text/plain", "Invalid ml value");
        return;
    }

    SystemConfig& cfg = configManager->getConfig();

    // Определяем тип теста
    bool isHead   = body.indexOf("\"head\"") > 0;
    bool isBodyNC = body.indexOf("\"body_nc\"") > 0;
    bool isBodyNO = body.indexOf("\"body_no\"") > 0;

    // Берём параметры теста из снимка (сохранённого при старте)
    int openSec, closeSec, durationSec;
    if (isHead) {
        openSec     = processEngine->getHeadTestOpenSec();
        closeSec    = processEngine->getHeadTestCloseSec();
        durationSec = processEngine->getHeadTestDuration();
    } else {
        openSec     = processEngine->getBodyTestOpenSec();
        closeSec    = processEngine->getBodyTestCloseSec();
        durationSec = processEngine->getBodyTestDuration();
    }

    // Расчёт точного времени открытия клапана
    int cycleTime   = openSec + closeSec;
    int fullCycles  = durationSec / cycleTime;
    int remainder   = durationSec % cycleTime;
    int openedSec   = fullCycles * openSec + min(remainder, openSec);

    if (openedSec <= 0) {
        server->send(400, "text/plain", "openedSec == 0");
        return;
    }

    // Скорость отбора мл/ч — фактическая
float speed = (ml / (float)durationSec) * 3600.0f;

// Расход клапана мл/мин — через время открытия
float capacity = (ml / (float)openedSec) * 60.0f;

    // Сохраняем расход в конфиг
    if (isHead)         cfg.valve_head_capacity  = (int)round(capacity);
    else if (isBodyNC)  cfg.valve_body_capacity  = (int)round(capacity);
    else if (isBodyNO)  cfg.valve0_body_capacity = (int)round(capacity);

    // Сохраняем в NVS
    configManager->saveConfig();

    // Сбрасываем флаг ожидания
    if (isHead) processEngine->clearHeadTestAwait();
    else        processEngine->clearBodyTestAwait();

    // Возвращаем результат веб-интерфейсу
    String resp = "{";
    resp += "\"capacity\":" + String(capacity, 1) + ",";
    resp += "\"speed\":"    + String(speed, 1)    + ",";
    resp += "\"openedSec\":" + String(openedSec);
    resp += "}";
    server->send(200, "application/json", resp);

    Serial.println("[CalcValve] capacity=" + String(capacity,1) + " ml/min, speed=" + String(speed,1) + " ml/h");
    logger.log("[CalcValve] capacity=" + String(capacity,1) + " ml/min, speed=" + String(speed,1) + " ml/h");
}
String AppNetwork::transliterate(String input) {
    String result = "";
    for (int i = 0; i < input.length(); i++) {
        char c = input[i];
        // Латиница и цифры — оставляем
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || 
            (c >= '0' && c <= '9') || c == '_' || c == '-') {
            result += c;
            continue;
        }
        // Пробел — заменяем на _
        if (c == ' ') { result += '_'; continue; }
        
        // Кириллица (UTF-8: каждый символ = 2 байта)
        // Берём два байта и сравниваем
        if (i + 1 < input.length()) {
            uint8_t b1 = (uint8_t)input[i];
            uint8_t b2 = (uint8_t)input[i+1];
            uint16_t cp = ((b1 & 0x1F) << 6) | (b2 & 0x3F);
            i++; // съедаем второй байт
            
            const char* translit[] = {
                "A","B","V","G","D","E","Yo","Zh","Z","I","J","K","L","M",
                "N","O","P","R","S","T","U","F","Kh","Ts","Ch","Sh","Shch",
                "","Y","","E","Yu","Ya"
            };
            // А=0x0410, Я=0x042F (заглавные), а=0x0430, я=0x044F (строчные)
            if (cp >= 0x0410 && cp <= 0x042F) {
                result += translit[cp - 0x0410];
            } else if (cp >= 0x0430 && cp <= 0x044F) {
                String t = translit[cp - 0x0430];
                t.toLowerCase();
                result += t;
            }
        }
    }
    if (result.length() == 0) result = "profile";
    return result;
}

String AppNetwork::buildCfgJson() {
    SystemConfig& cfg = configManager->getConfig();
    String j = "{";
    j += "\"emergencyTime\":" + String(cfg.emergencyTime) + ",";
    j += "\"nasebTime\":"     + String(cfg.nasebTime)     + ",";
    j += "\"reklapTime\":"    + String(cfg.reklapTime)    + ",";
    j += "\"boxMaxTemp\":"    + String(cfg.boxMaxTemp)    + ",";
    j += "\"power\":"         + String(cfg.power)         + ",";
    j += "\"asVolume\":"      + String(cfg.asVolume)      + ",";
    j += "\"chekwifi\":"      + String(cfg.chekwifi)      + ",";
    j += "\"razgonTemp\":"    + String(cfg.razgonTemp)    + ",";
    j += "\"bakStopTemp\":"   + String(cfg.bakStopTemp)   + ",";
    j += "\"midterm\":"       + String(cfg.midterm)       + ",";
    j += "\"midterm_abv\":"   + String(cfg.midterm_abv)   + ",";
    j += "\"heaterType\":"    + String(cfg.heaterType)    + ",";
    j += "\"fullPwr\":"       + String(cfg.fullPwr  ? "1":"0") + ",";
    j += "\"valveuse\":"      + String(cfg.valveuse ? "1":"0") + ",";
    j += "\"mixerEnabled\":"  + String(cfg.mixerEnabled ? "1":"0") + ",";
    j += "\"mixerOnTime\":"   + String(cfg.mixerOnTime)   + ",";
    j += "\"mixerOffTime\":"  + String(cfg.mixerOffTime)  + ",";
    j += "\"tsaLimit\":"      + String(cfg.tsaLimit)      + ",";
    j += "\"cycleLim\":"      + String(cfg.cycleLim)      + ",";
    j += "\"histeresis\":"    + String(cfg.histeresis)    + ",";
    j += "\"delta\":"         + String(cfg.delta)         + ",";
    j += "\"useHeadValve\":"  + String(cfg.useHeadValve  ? "1":"0") + ",";
    j += "\"bodyValveNC\":"   + String(cfg.bodyValveNC   ? "1":"0") + ",";
    j += "\"headsTypeKSS\":"  + String(cfg.headsTypeKSS  ? "1":"0") + ",";
    j += "\"calibration\":"   + String(cfg.calibration   ? "1":"0") + ",";
    j += "\"headOpenMs\":"    + String(cfg.headOpenMs)    + ",";
    j += "\"headCloseMs\":"   + String(cfg.headCloseMs)   + ",";
    j += "\"bodyOpenMs\":"    + String(cfg.bodyOpenMs)    + ",";
    j += "\"bodyCloseMs\":"   + String(cfg.bodyCloseMs)   + ",";
    j += "\"active_test\":"   + String(cfg.active_test)   + ",";
    j += "\"valve_head_capacity\":"  + String(cfg.valve_head_capacity)  + ",";
    j += "\"valve_body_capacity\":"  + String(cfg.valve_body_capacity)  + ",";
    j += "\"valve0_body_capacity\":" + String(cfg.valve0_body_capacity);
    j += "}";
    return j;
}
void AppNetwork::handleSaveProfile() {
    if (!server->hasArg("plain")) {
        server->send(400, "text/plain", "Bad Request");
        return;
    }
    String body = server->arg("plain");
    
    // Парсим имя профиля
    String name = "";
    int idx = body.indexOf("\"name\":");
    if (idx >= 0) {
        int s = body.indexOf('"', idx + 7);
        int e = body.indexOf('"', s + 1);
        if (s >= 0 && e > s) name = body.substring(s + 1, e);
    }
    if (name.length() == 0) {
        server->send(400, "text/plain", "Name required");
        return;
    }

    // Создаём папку если нет
    if (!SD.exists("/profiles")) SD.mkdir("/profiles");

    // Имя файла — транслитерация
    String filename = "/profiles/" + transliterate(name) + ".json";

    // Формируем JSON профиля
    String json = "{";
    json += "\"name\":\"" + name + "\",";
    json += "\"cfg\":" + buildCfgJson();
    json += "}";

    // Удаляем старый файл если есть (перезапись)
    if (SD.exists(filename)) SD.remove(filename);

    File file = SD.open(filename, FILE_WRITE);
    if (!file) {
        server->send(500, "text/plain", "SD write error");
        return;
    }
    file.print(json);
    file.close();

    Serial.println("[Profile] Saved: " + filename);
    logger.log("[Profile] Saved: " + filename);
    server->send(200, "text/plain", "OK");
}

void AppNetwork::handleListProfiles() {
    if (!SD.exists("/profiles")) {
        server->send(200, "application/json", "[]");
        return;
    }

    File dir = SD.open("/profiles");
    String json = "[";
    bool first = true;

    while (true) {
        File entry = dir.openNextFile();
        if (!entry) break;
        
        String fname = String(entry.name());
        if (fname.endsWith(".json")) {
            // Читаем имя из содержимого файла
            String content = "";
            while (entry.available()) content += (char)entry.read();
            entry.close();
            
            // Парсим "name" из JSON
            String displayName = fname; // fallback
            int ni = content.indexOf("\"name\":");
            if (ni >= 0) {
                int s = content.indexOf('"', ni + 7);
                int e = content.indexOf('"', s + 1);
                if (s >= 0 && e > s) displayName = content.substring(s + 1, e);
            }
            
            // Имя файла без пути и расширения
            String fileKey = fname.substring(0, fname.length() - 5); // убираем .json
            
            if (!first) json += ",";
            json += "{\"file\":\"" + fileKey + "\",\"name\":\"" + displayName + "\"}";
            first = false;
        } else {
            entry.close();
        }
    }
    dir.close();
    json += "]";
    server->send(200, "application/json", json);
}

void AppNetwork::handleLoadProfile() {
    if (!server->hasArg("plain")) {
        server->send(400, "text/plain", "Bad Request");
        return;
    }
    String body = server->arg("plain");

    // Парсим имя файла (file key без расширения)
    String fileKey = "";
    int idx = body.indexOf("\"file\":");
    if (idx >= 0) {
        int s = body.indexOf('"', idx + 7);
        int e = body.indexOf('"', s + 1);
        if (s >= 0 && e > s) fileKey = body.substring(s + 1, e);
    }
    if (fileKey.length() == 0) {
        server->send(400, "text/plain", "File key required");
        return;
    }

    String filename = "/profiles/" + fileKey + ".json";
    if (!SD.exists(filename)) {
        server->send(404, "text/plain", "Profile not found");
        return;
    }

    File file = SD.open(filename);
    String content = "";
    while (file.available()) content += (char)file.read();
    file.close();

    // Находим блок cfg внутри профиля и применяем через существующий парсер
    int cfgIdx = content.indexOf("\"cfg\":");
    if (cfgIdx < 0) {
        server->send(500, "text/plain", "Invalid profile format");
        return;
    }
    // Вырезаем JSON cfg начиная с {
    String cfgJson = content.substring(content.indexOf('{', cfgIdx + 6));
    // Убираем закрывающую } профиля
    cfgJson = cfgJson.substring(0, cfgJson.lastIndexOf('}') + 1);

    // Применяем через существующий парсер настроек
    // Временно подменяем тело запроса — используем тот же код что в handleApiSettings
    SystemConfig& cfg = configManager->getConfig();
    
    auto getInt = [&](const char* key) -> int {
        String searchKey = "\"" + String(key) + "\":";
        int pos = cfgJson.indexOf(searchKey);
        if (pos == -1) return -9999;
        return cfgJson.substring(pos + searchKey.length()).toInt();
    };
    auto getFloat = [&](const char* key) -> float {
        String searchKey = "\"" + String(key) + "\":";
        int pos = cfgJson.indexOf(searchKey);
        if (pos == -1) return -9999.0f;
        return cfgJson.substring(pos + searchKey.length()).toFloat();
    };

    // Применяем все поля (те же что в handleApiSettings)
    int v;
    float f;
    if ((v = getInt("emergencyTime"))      != -9999) cfg.emergencyTime      = v;
    if ((v = getInt("nasebTime"))          != -9999) cfg.nasebTime          = v;
    if ((v = getInt("reklapTime"))         != -9999) cfg.reklapTime         = v;
    if ((f = getFloat("boxMaxTemp"))       != -9999) cfg.boxMaxTemp         = f;
    if ((v = getInt("power"))              != -9999) cfg.power              = v;
    if ((v = getInt("asVolume"))           != -9999) cfg.asVolume           = v;
    if ((v = getInt("chekwifi"))           != -9999) cfg.chekwifi           = v;
    if ((f = getFloat("razgonTemp"))       != -9999) cfg.razgonTemp         = f;
    if ((f = getFloat("bakStopTemp"))      != -9999) cfg.bakStopTemp        = f;
    if ((f = getFloat("midterm"))          != -9999) cfg.midterm            = f;
    if ((v = getInt("heaterType"))         != -9999) cfg.heaterType         = v;
    if ((v = getInt("fullPwr"))            != -9999) cfg.fullPwr            = v;
    if ((v = getInt("valveuse"))           != -9999) cfg.valveuse           = v;
    if ((v = getInt("mixerEnabled"))       != -9999) cfg.mixerEnabled       = v;
    if ((v = getInt("mixerOnTime"))        != -9999) cfg.mixerOnTime        = v;
    if ((v = getInt("mixerOffTime"))       != -9999) cfg.mixerOffTime       = v;
    if ((f = getFloat("tsaLimit"))         != -9999) cfg.tsaLimit           = f;
    if ((v = getInt("cycleLim"))           != -9999) cfg.cycleLim           = v;
    if ((f = getFloat("histeresis"))       != -9999) cfg.histeresis         = f;
    if ((f = getFloat("delta"))            != -9999) cfg.delta              = f;
    if ((v = getInt("useHeadValve"))       != -9999) cfg.useHeadValve       = v;
    if ((v = getInt("bodyValveNC"))        != -9999) cfg.bodyValveNC        = v;
    if ((v = getInt("headsTypeKSS"))       != -9999) cfg.headsTypeKSS       = v;
    if ((v = getInt("calibration"))        != -9999) cfg.calibration        = v;
    if ((v = getInt("headOpenMs"))         != -9999) cfg.headOpenMs         = v;
    if ((v = getInt("headCloseMs"))        != -9999) cfg.headCloseMs        = v;
    if ((v = getInt("bodyOpenMs"))         != -9999) cfg.bodyOpenMs         = v;
    if ((v = getInt("bodyCloseMs"))        != -9999) cfg.bodyCloseMs        = v;
    if ((v = getInt("active_test"))        != -9999) cfg.active_test        = v;
    if ((v = getInt("valve_head_capacity"))  != -9999) cfg.valve_head_capacity  = v;
    if ((v = getInt("valve_body_capacity"))  != -9999) cfg.valve_body_capacity  = v;
    if ((v = getInt("valve0_body_capacity")) != -9999) cfg.valve0_body_capacity = v;

    configManager->saveConfig();

    Serial.println("[Profile] Loaded: " + filename);
    logger.log("[Profile] Loaded: " + filename);
    server->send(200, "text/plain", "OK");
}