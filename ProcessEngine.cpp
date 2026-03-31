#include "ProcessEngine.h"
#include <math.h>
#include "SDLogger.h" 
// === КОНСТАНТЫ ===
const float CORRELATION_COEFF = 1.13f; // Коэффициент снижения скорости для метода Шпора
const unsigned long SHPORA_STABILIZATION_MS = 60000; // 60 секунд на стабилизацию колонны
const float MAX_CYCLE_TIME_MS = 10000.0f; // Максимальное время цикла (мс)

// ==================== ИНИЦИАЛИЗАЦИЯ ====================

void ProcessEngine::begin(LiquidCrystal_I2C* lcd, SensorAdapter* sensors, OutputManager* outputs, ConfigManager* cfgMgr) {
    this->sensorAdapter = sensors;
    this->outputManager = outputs;
    this->configManager = cfgMgr;

    // +++ ПЕРЕДАЕМ ПАРАМЕТР ТИПА КЛАПАНА +++
    // getConfig() возвращает ссылку на SystemConfig
    outputManager->begin(configManager->getConfig().bodyValveNC); 

    valveCalMenu = new ValveCalMenu(lcd, configManager, outputs);
    setPwAsMenu = new SetPwAsMenu(lcd, configManager);
    
  
    
    currentStatus.activeProcess = PROCESS_NONE;
    currentStatus.isRunning = false;
    currentStatus.stageName = "IDLE";
    Serial.println("[ProcessEngine] Initialized");
    logger.log("[ProcessEngine] Initialized");
    printStartupInfo();
}

void ProcessEngine::syncConfig() {}
void ProcessEngine::updateNetworkStatus(char networkSymbol) {
    currentStatus.networkSymbol = String(networkSymbol);
}
void ProcessEngine::handleUiUp() {
    if (currentStage == Stage::VALVE_CAL) valveCalMenu->handleUpButton();
    else if (currentStage == Stage::SET_PW_AS) setPwAsMenu->handleUpButton();
}
void ProcessEngine::handleUiDown() {
    if (currentStage == Stage::VALVE_CAL) valveCalMenu->handleDownButton();
    else if (currentStage == Stage::SET_PW_AS) setPwAsMenu->handleDownButton();
}
void ProcessEngine::handleUiSet() {
    if (currentStage == Stage::VALVE_CAL) valveCalMenu->handleSetButton();
    else if (currentStage == Stage::SET_PW_AS) setPwAsMenu->handleSetButton();
    
    else if (currentStage == Stage::WATER_TEST) handleCommand(UiCommand::DIALOG_YES);
    else if (currentStage == Stage::REPLACEMENT) handleCommand(UiCommand::DIALOG_YES);
    else if (currentStage == Stage::GOLOVY_OK) handleCommand(UiCommand::DIALOG_YES);
    // =====================
}
void ProcessEngine::handleUiBack() {
    if (currentStage == Stage::VALVE_CAL) valveCalMenu->handleBackButton();
    else if (currentStage == Stage::SET_PW_AS) setPwAsMenu->handleBackButton();
    // === ДОБАВИТЬ ЭТО ===
    else if (currentStage == Stage::WATER_TEST) handleCommand(UiCommand::DIALOG_NO); // Это остановит процесс
    else if (currentStage == Stage::REPLACEMENT) handleCommand(UiCommand::DIALOG_NO); // Это перейдет в BAKSTOP
    // =====================
}

// ==================== ОСНОВНОЙ ЦИКЛ ====================

void ProcessEngine::update() {
    outputManager->update();
    checkSafety();
        // === ПРОВЕРКА СТАТУСА WEB КАЛИБРОВКИ ===
    if (currentStatus.webCalibStatus == 1) { // Если в режиме поиска
        // Определяем индекс датчика по имени (грубо, но работает)
        int idx = 0;
        if (currentStatus.webCalibSensorName == "AQUA") idx = 1;
        else if (currentStatus.webCalibSensorName == "TSAR") idx = 2;
        else if (currentStatus.webCalibSensorName == "TANK") idx = 3;

        DeviceAddress addr;
        // Проверяем, найден ли датчик (checkCalibration теперь не сохраняет)
                if (SensorManager::getInstance()->checkCalibration((SensorIndex)idx, addr)) {
            currentStatus.webCalibStatus = 2; // Найден
            Serial.println("[WebCalib] Sensor Found!");
            logger.log("[WebCalib] Sensor Found!");
        }
        
        // Таймаут (30 сек)
               // Простая проверка времени (менеджер сбросит флаг inProgress при таймауте)
if (!SensorManager::getInstance()->isCalibrating((SensorIndex)idx) && currentStatus.webCalibStatus == 1) {
     currentStatus.webCalibStatus = 3;// Ошибка/Не найден
             Serial.println("[WebCalib] Not Found / Timeout");
             logger.log("[WebCalib] Not Found / Timeout");
        }
    }
        // === ПРОВЕРКА ТАЙМЕРОВ ТЕСТОВ (РАЗДЕЛЬНАЯ) ===
    if (headTestStatus.active) {
        unsigned long elapsed = (millis() - headTestStatus.startTime) / 1000;
        if (elapsed >= headTestStatus.durationSec) {
    outputManager->stopHeadValveTest();
    headTestStatus.active = false;
    headTestStatus.awaitingInput = true;
    Serial.println("[Process] Head Test Finished - Awaiting ml input");
    logger.log("[Process] Head Test Finished - Awaiting ml input");
}
    }
    
    if (bodyTestStatus.active) {
        unsigned long elapsed = (millis() - bodyTestStatus.startTime) / 1000;
        if (elapsed >= bodyTestStatus.durationSec) {
    outputManager->stopBodyValveTest();
    bodyTestStatus.active = false;
    bodyTestStatus.awaitingInput = true;
    Serial.println("[Process] Body Test Finished - Awaiting ml input");
    logger.log("[Process] Body Test Finished - Awaiting ml input");
}
    }
    
    // === СИНХРОНИЗАЦИЯ СОСТОЯНИЯ КАЛИБРОВКИ КЛАПАНОВ ===
    if (valveCalMenu) {
        // Обновляем состояние мастера (таймеры, переходы)
        valveCalMenu->update();
        
        // Копируем состояние в статус для Web
        CalibWizardState& ws = valveCalMenu->getWizardState();
        currentStatus.calibWizard.valve = (int)ws.valve + 1;  // 0-based to 1-based
        currentStatus.calibWizard.step = (int)ws.step;
        currentStatus.calibWizard.isRunning = ws.isTestRunning;
        currentStatus.calibWizard.totalSec = ws.testDurationSec;
        currentStatus.calibWizard.remainingSec = valveCalMenu->getTestRemaining();
        currentStatus.calibWizard.volume = ws.enteredVolume;
        currentStatus.calibWizard.capacity = ws.calculatedCapacity;
    }
    // ====================================================
        
    if (emergencyState) return;
    updateDisplayData();

    switch (currentStage) {
        case Stage::IDLE: handleIdleState(); break;
        case Stage::WATER_TEST: handleWaterTest(); break;
        case Stage::RAZGON: handleRazgon(); break;
        case Stage::WAITING: handleWaiting(); break;
        case Stage::OTBOR: handleDistOtbor(); break;
        case Stage::REPLACEMENT: handleDistReplacement(); break;
        case Stage::BAKSTOP: handleDistBakstop(); break;
        case Stage::NASEBYA: handleNasebya(); break;
        case Stage::VALVE_CAL:
        case Stage::SET_PW_AS:
        case Stage::GOLOVY:
        case Stage::GOLOVY_OK:
        case Stage::TELO: handleRectProcess(); break;
        case Stage::FINISHING_WORK: handleFinishingWork(); break;
        default: break;
    }
    
    if (activeProcess == PROCESS_RECT) {
        updateRectWebInfo();
    }
    if (processRunning) {
        currentStatus.processTimeSec = (millis() - processStartTime) / 1000;
        currentStatus.stageTimeSec = (millis() - stageStartTime) / 1000;
    }
}

// ==================== SAFETY ====================

void ProcessEngine::checkSafety() {
    const SensorData& data = sensorAdapter->getData();
    SystemConfig& cfg = configManager->getConfig();
    
    // --- 1. Проверка TSA (Приоритет выше) ---
    if (data.isTsaValid() && data.tsa.value > cfg.tsaLimit) {  // если датчик не валиден — тревога не срабатывает
        if (!alarmTSA_Active) {
            alarmTSA_Active = true; 
            alarmStartTime = millis();
            currentStatus.safety = SafetyState::WARNING_TSA;
            currentStatus.alarm = AlarmType::ALARM_TSA;
            outputManager->alarmBeep(AlarmType::ALARM_TSA);
            Serial.println("[Safety] TSA LIMIT EXCEEDED! Countdown started.");
             // === ЛОГИРОВАНИЕ ===
            logger.log("ALARM! TSA LIMIT > " + String(cfg.tsaLimit) + "C");
            // ==================
        }
        
        // Обратный отсчет
        unsigned long limitSec = (unsigned long)cfg.emergencyTime * 60;
        unsigned long elapsedSec = (millis() - alarmStartTime) / 1000;
        currentStatus.alarmTimerSec = (elapsedSec < limitSec) ? (limitSec - elapsedSec) : 0;
        
        // Если время вышло -> FINISHING WORK (ТЗ: переход к процедуре завершения)
        if (currentStatus.alarmTimerSec == 0 && currentStage != Stage::FINISHING_WORK) {
    Serial.println("[Safety] VREAC time elapsed! -> FINISHING WORK");
    logger.log("[Safety] VREAC time elapsed! -> FINISHING WORK");
    changeStage(Stage::FINISHING_WORK); 
    currentStatus.safety = SafetyState::EMERGENCY;
}
    } else {
        // Температура в норме
        if (alarmTSA_Active) { 
            alarmTSA_Active = false; 
            outputManager->stopAlarm(); 
            Serial.println("[Safety] TSA normalized.");
            // === ЛОГИРОВАНИЕ ===
            logger.log("ALARM CLEARED: TSA Normal");
            // ==================
        }
        if (!alarmBOX_Active) { 
            currentStatus.safety = SafetyState::NORMAL; 
            currentStatus.alarm = AlarmType::NONE; 
        }
    }

    // --- 2. Проверка BOX (Только если нет аварии TSA и BME доступен) ---
    // Если BME недоступен, boxTemp заморожен, проверку пропускаем
    if (!alarmTSA_Active && currentStatus.bmeAvailable) {
        if (data.boxTemp > cfg.boxMaxTemp) {
            if (!alarmBOX_Active) {
                alarmBOX_Active = true; 
                currentStatus.safety = SafetyState::WARNING_BOX;
                currentStatus.alarm = AlarmType::ATTENTION_BOX;
                outputManager->alarmBeep(AlarmType::ATTENTION_BOX);
                Serial.println("[Safety] BOX OVERHEAT!");
                // === ЛОГИРОВАНИЕ ===
                logger.log("WARNING: BOX OVERHEAT > " + String(cfg.boxMaxTemp) + "C");
                // ==================
            }
        } else {
            if (alarmBOX_Active) {
                alarmBOX_Active = false; 
                outputManager->stopAlarm();
                currentStatus.safety = SafetyState::NORMAL; 
                currentStatus.alarm = AlarmType::NONE;
                Serial.println("[Safety] BOX Temp normalized.");
                // === ЛОГИРОВАНИЕ ===
                logger.log("WARNING CLEARED: BOX Normal");
                // ==================
            }
        }
    }
}

// ==================== COMMANDS ====================

// -----------------------------------------------------------------------------
// checkSensorsReady() — проверка готовности датчиков перед запуском процесса.
//
// Логика проверки для каждого из 4 датчиков (TSA, TANK, TSAR, AQUA):
//   1. isCalibrated()  — адрес датчика сохранён в EEPROM (оператор провёл калибровку)
//   2. isConnected()   — датчик отвечает на шине 1-Wire в данный момент
//
// Если хотя бы один датчик не прошёл проверку — возвращает false.
// В errorMsg записываются имена проблемных датчиков через пробел (для LCD, 20 символов).
// Причина также пишется в Serial и на SD-карту через logger.
// -----------------------------------------------------------------------------
bool ProcessEngine::checkSensorsReady(String& errorMsg) {
    SensorManager* sm = SensorManager::getInstance();

    // Таблица проверяемых датчиков: индекс и отображаемое имя
    struct SensorCheck {
        SensorIndex index;
        const char* name;
    };
    const SensorCheck checks[4] = {
        { SENSOR_TSA,  "TSA"  },
        { SENSOR_TANK, "TANK" },
        { SENSOR_TSAR, "TSAR" },
        { SENSOR_AQUA, "AQUA" }
    };

    errorMsg = "";
    bool allReady = true;

    for (int i = 0; i < 4; i++) {
        bool calibrated = sm->isCalibrated(checks[i].index);
        bool connected  = sm->isConnected(checks[i].index);

        if (!calibrated || !connected) {
            // Добавляем имя датчика в строку ошибки (с разделителем-пробелом)
            if (errorMsg.length() > 0) errorMsg += " ";
            errorMsg += checks[i].name;

            // Детальный лог: указываем точную причину для каждого датчика
            if (!calibrated) {
                logger.log("START BLOCKED: " + String(checks[i].name) + " not calibrated");
            } else {
                logger.log("START BLOCKED: " + String(checks[i].name) + " offline (no response)");
            }
            allReady = false;
        }
    }
    return allReady;
}



EngineResponse ProcessEngine::handleCommand(UiCommand command, int param) {
    

        // === ОБРАБОТКА ЗАПУСКА ТЕСТОВ (РАЗДЕЛЬНО) ===
    
    // 1. Тест Голов
    if (command == UiCommand::TEST_HEAD) {
        SystemConfig& cfg = configManager->getConfig();
        outputManager->setBodyValveType(cfg.bodyValveNC); // Синхронизация типа
        
        // Если тест уже шел - перезапускаем
        outputManager->stopHeadValveTest(); 
        headTestStatus.active = false;

        // Запуск
        headTestStatus.active = true;
headTestStatus.startTime = millis();
headTestStatus.durationSec = cfg.active_test;
headTestStatus.openSec = cfg.headOpenMs;
headTestStatus.closeSec = cfg.headCloseMs;
headTestStatus.awaitingInput = false;
outputManager->startHeadValveCycling(cfg.headOpenMs * 1000, cfg.headCloseMs * 1000);
        Serial.println("[Process] HEAD Test Started");
        // === ЛОГИРОВАНИЕ ===
        logger.log("[Process] HEAD Test Started");
        return EngineResponse::OK;
    }
    
    // 2. Тест Тела
    if (command == UiCommand::TEST_BODY) {
        SystemConfig& cfg = configManager->getConfig();
        outputManager->setBodyValveType(cfg.bodyValveNC);
        
        outputManager->stopBodyValveTest();
        bodyTestStatus.active = false;

        // Запуск
        bodyTestStatus.active = true;
bodyTestStatus.startTime = millis();
bodyTestStatus.durationSec = cfg.active_test;
bodyTestStatus.openSec = cfg.bodyOpenMs;
bodyTestStatus.closeSec = cfg.bodyCloseMs;
bodyTestStatus.awaitingInput = false;
outputManager->startBodyValveCycling(cfg.bodyOpenMs * 1000, cfg.bodyCloseMs * 1000);
        Serial.println("[Process] BODY Test Started");
        // === ЛОГИРОВАНИЕ ===
        logger.log("[Process] BODY Test Started");
        return EngineResponse::OK;
    }
    
    // 3. Остановка всех тестов
    if (command == UiCommand::STOP_TEST) {
        // Web просит остановить все активные тесты
        if (headTestStatus.active) outputManager->stopHeadValveTest();
        if (bodyTestStatus.active) outputManager->stopBodyValveTest();
        
        headTestStatus.active = false;
        bodyTestStatus.active = false;
        Serial.println("[Process] All Tests Stopped");
        // === ЛОГИРОВАНИЕ ===
        logger.log("[Process] All Tests Stopped");
        return EngineResponse::OK;
    }
    if (command == UiCommand::CALC_VALVE) {
    // Команда приходит с параметром ml из веба — см. AppNetwork.cpp
    // ml передаётся через отдельный API endpoint /api/calcvalve
    return EngineResponse::OK;
}
        // === КОМАНДА ЗАВЕРШИТЬ КАЛИБРОВКУ (ДО проверки VALVE_CAL) ===
    if (command == UiCommand::FINISH_CALIBRATION) {
        if (currentStage == Stage::VALVE_CAL) {
            Serial.println("[WebCmd] Finish Calibration -> SET PW & AS");
            logger.log("[WebCmd] Finish Calibration -> SET PW & AS");
            // === СБРОС АВАРИИ ПРИ ЗАВЕРШЕНИИ КАЛИБРОВКИ ===
            alarmTSA_Active = false;
            alarmBOX_Active = false;
            currentStatus.alarm = AlarmType::NONE;
            currentStatus.safety = SafetyState::NORMAL;
            outputManager->stopAlarm();
            outputManager->resetEmergency();
            // ===============================================
            changeStage(Stage::SET_PW_AS);
            if (setPwAsMenu) setPwAsMenu->display(); 
            return EngineResponse::SHOW_PROCESS_SCREEN;
        }
        // Если мы не в VALVE_CAL, просто игнорируем
        return EngineResponse::OK;
    }

    // === КОМАНДЫ КАЛИБРОВКИ ИЗ WEB (работают в любой момент, ДО проверки VALVE_CAL) ===
    if (command == UiCommand::CALIB_START_DRY) {
        if (valveCalMenu) {
            valveCalMenu->startCalibFromWeb(param, 10);  // 10 сек
            Serial.printf("[WebCmd] Calib DRY: valve=%d, 10 sec\n", param);
        }
        return EngineResponse::OK;
    }
    
    if (command == UiCommand::CALIB_START_CAPACITY) {
        if (valveCalMenu) {
            valveCalMenu->startCalibFromWeb(param, 60);  // 60 сек
            Serial.printf("[WebCmd] Calib CAPACITY: valve=%d, 60 sec\n", param);
        }
        return EngineResponse::OK;
    }
    
    if (command == UiCommand::CALIB_CANCEL) {
        if (valveCalMenu) {
            valveCalMenu->cancelCalibFromWeb();
            Serial.println("[WebCmd] Calib CANCELLED");
        }
        return EngineResponse::OK;
    }
    // ================================================================

    if (currentStage == Stage::VALVE_CAL) {
        if (command == UiCommand::DIALOG_YES) valveCalMenu->handleSetButton();
        else if (command == UiCommand::DIALOG_NO) valveCalMenu->handleBackButton();
        return EngineResponse::OK; 
    }
    // ==============================================================
    
       
       // === НОВАЯ КОМАНДА ДЛЯ WEB (Кнопка "ДАЛЕЕ") ===
    if (command == UiCommand::NEXT_STAGE) {
        if (currentStage == Stage::VALVE_CAL) {
            Serial.println("[Web] VALVE CAL -> SET PW & AS");
            // === ЛОГИРОВАНИЕ ===
        logger.log("CMD: VALVE CAL -> SET PW & AS");
            changeStage(Stage::SET_PW_AS);
            // Инициализируем меню, чтобы LCD показал настройки
            if (setPwAsMenu) setPwAsMenu->display(); 
            return EngineResponse::SHOW_PROCESS_SCREEN;
        }
        else if (currentStage == Stage::SET_PW_AS) {
            Serial.println("[Web] SET PW & AS -> GOLOVY");
            // === ЛОГИРОВАНИЕ ===
        logger.log("CMD:  SET PW & AS -> GOLOVY");
            // Сбрасываем стадию голов перед стартом
            currentGolovyStage = GolovyStage::IDLE; 
            changeStage(Stage::GOLOVY);
            return EngineResponse::SHOW_PROCESS_SCREEN;
        }
        // Можно добавить и для WATER_TEST, если нужно, но там DIALOG_YES
    }
    if (currentStage == Stage::GOLOVY_OK) {
        if (command == UiCommand::DIALOG_YES) {
            Serial.println("[Process] GOLOVY OK -> TELO");
            const SensorData& data = sensorAdapter->getData();
            logger.log("CMD: GOLOVY OK -> TELO. TSAR: " + String(data.tsar.value, 1) + "C"
                     + "  confirmed by operator");
            outputManager->stopValveCycling(); 
            outputManager->closeHeadValve();
            outputManager->closeBodyValve();
            changeStage(Stage::TELO);
            return EngineResponse::SHOW_PROCESS_SCREEN;
        }
    }

    if (currentStage == Stage::REPLACEMENT) {
        if (command == UiCommand::DIALOG_YES) {
            outputManager->openBodyValve();
            logger.log("REPLACEMENT: confirmed by operator. Resuming OTBOR.");
            changeStage(Stage::OTBOR);
            return EngineResponse::SHOW_PROCESS_SCREEN;
        } else if (command == UiCommand::DIALOG_NO) {
            outputManager->closeBodyValve();
            logger.log("REPLACEMENT: skipped by operator. -> BAKSTOP.");
            changeStage(Stage::BAKSTOP);
            return EngineResponse::SHOW_PROCESS_SCREEN;
        }
    }
        // === ОБРАБОТКА КАЛИБРОВКИ ДАТЧИКОВ (WEB) ===
    if (command == UiCommand::IDENTIFY_TSA || command == UiCommand::IDENTIFY_AQUA || 
    command == UiCommand::IDENTIFY_TSAR || command == UiCommand::IDENTIFY_TANK) {
    
    int idx = 0;
    String name = "TSA";
    if (command == UiCommand::IDENTIFY_AQUA) { idx = 1; name = "AQUA"; }
    else if (command == UiCommand::IDENTIFY_TSAR) { idx = 2; name = "TSAR"; }
    else if (command == UiCommand::IDENTIFY_TANK) { idx = 3; name = "TANK"; }

    currentStatus.webCalibSensorName = name;
    currentStatus.webCalibStatus = 1; // Сразу поиск
    SensorManager::getInstance()->startCalibration((SensorIndex)idx);
    Serial.print("[WebCalib] Started for "); Serial.println(name);
    logger.log("CMD: [WebCalib] Started for " + name);
    return EngineResponse::OK;
}
    
     if (command == UiCommand::DIALOG_NO) {
    if (currentStatus.webCalibStatus >= 1 && currentStatus.webCalibStatus <= 3) {
        int idx = 0;
        if (currentStatus.webCalibSensorName == "AQUA")  idx = 1;
        else if (currentStatus.webCalibSensorName == "TSAR") idx = 2;
        else if (currentStatus.webCalibSensorName == "TANK") idx = 3;
        SensorManager::getInstance()->cancelCalibration((SensorIndex)idx);
        currentStatus.webCalibStatus = 0;
        currentStatus.webCalibSensorName = "";
        Serial.println("[WebCalib] Cancelled");
        logger.log("[WebCalib] Cancelled");
        return EngineResponse::OK;
    }
}

    if (command == UiCommand::DIALOG_YES) {
    if (currentStatus.webCalibStatus == 2) { // Найден — сохраняем
        int idx = 0;
        if (currentStatus.webCalibSensorName == "AQUA")  idx = 1;
        else if (currentStatus.webCalibSensorName == "TSAR") idx = 2;
        else if (currentStatus.webCalibSensorName == "TANK") idx = 3;
        SensorManager::getInstance()->confirmCalibrationSave((SensorIndex)idx);
        currentStatus.webCalibStatus = 0;
        currentStatus.webCalibSensorName = "";
        Serial.println("[WebCalib] Saved");
        logger.log("[WebCalib] Saved");
        return EngineResponse::OK;
    }
    if (currentStatus.webCalibStatus == 3) { // Не найден — просто закрываем
        currentStatus.webCalibStatus = 0;
        currentStatus.webCalibSensorName = "";
        return EngineResponse::OK;
    }
    // ... остальной DIALOG_YES (WATER_TEST и т.д.)
}
    if (dialogPending && currentStage == Stage::WATER_TEST) {
        if (command == UiCommand::DIALOG_YES) {
            outputManager->closeWaterValve();
            outputManager->stopMixer();
            dialogPending = false;
            logger.log("WATER TEST: confirmed by operator");
            changeStage(Stage::RAZGON);
            currentStatus.stageName = "RAZGON"; 
            return EngineResponse::SHOW_PROCESS_SCREEN;
        } else if (command == UiCommand::DIALOG_NO) {
            outputManager->closeWaterValve();
            outputManager->stopMixer();
            dialogPending = false;
            // === ЛОГИРОВАНИЕ ===
            logger.log("Process CANCELLED by user (Water Test)");
            stopCurrentProcess(); 
            return EngineResponse::SHOW_PROCESS_MENU;
        }
    }

        if (command == UiCommand::STOP_PROCESS) {
            logger.log("Command: STOP PROCESS (Manual Stop)");
        // Запускаем этап завершения, но НЕ останавливаем процесс таймером!
        // processRunning должен остаться true, чтобы update() крутил логику FINISHING_WORK
        changeStage(Stage::FINISHING_WORK);
        return EngineResponse::SHOW_PROCESS_SCREEN;
    }
    
    // --- Запуск DIST ---
    if (command == UiCommand::START_DIST && !processRunning) {
        String errMsg;
        if (!checkSensorsReady(errMsg)) {
            // Сохраняем текст ошибки в статус — меню прочитает его и покажет экран ошибки
            currentStatus.sensorErrorMsg = errMsg;
            Serial.println("[Process] START_DIST BLOCKED. Problem sensors: " + errMsg);
            return EngineResponse::ERROR_INVALID_STATE;
        }
        currentStatus.sensorErrorMsg = ""; // Сброс: датчики в норме
        startProcess(PROCESS_DIST);
        return EngineResponse::SHOW_WATER_TEST;
    }

    // --- Запуск RECT ---
    if (command == UiCommand::START_RECT && !processRunning) {
        String errMsg;
        if (!checkSensorsReady(errMsg)) {
            currentStatus.sensorErrorMsg = errMsg;
            Serial.println("[Process] START_RECT BLOCKED. Problem sensors: " + errMsg);
            return EngineResponse::ERROR_INVALID_STATE;
        }
        currentStatus.sensorErrorMsg = ""; // Сброс: датчики в норме
        startProcess(PROCESS_RECT);
        return EngineResponse::SHOW_WATER_TEST;
    }
    
    return EngineResponse::OK;
}

bool ProcessEngine::startProcess(ProcessType type) {
    if (processRunning) return false;
    activeProcess = type; // Сначала запоминаем тип процесса
    processRunning = true;
    processStartTime = millis();
    counter = 0;
    
    emergencyState = false; alarmTSA_Active = false; alarmBOX_Active = false;
    outputManager->stopAlarm(); outputManager->resetEmergency();
    
    razgonMixerStarted = false;
    midtermHandled = false;
    
    // Сброс переменных TELO при старте
    rtsarM = 0.0f;
    adPressM = 0.0f;

    // === НОВОЕ: Сброс накопленного объема голов ===
    headsVolDone = 0.0f;
    lastVolCalcTime = millis();
    // ==============================================
    if (type == PROCESS_DIST) {
        outputManager->setBodyValveType(false); 
    } else {
        outputManager->setBodyValveType(configManager->getConfig().bodyValveNC);
    }
    
    configManager->startProcess(type); 
String procName = getProcessName();
    Serial.print("[Process] START: ");
    Serial.println(getProcessName()); // Вывод названия процесса (исправлено)
 // === ДОБАВИТЬ ЛОГИРОВАНИЕ ===
    logger.log("Process START: " + procName);
    // Ключевые параметры конфигурации
    SystemConfig& cfg = configManager->getConfig();
    if (type == PROCESS_DIST) {
        logger.log("  razgonTemp: " + String(cfg.razgonTemp) + "C"
                 + "  bakStopTemp: " + String(cfg.bakStopTemp) + "C"
                 + "  midterm: " + String(cfg.midterm) + "C");
    } else {
        logger.log("  razgonTemp: " + String(cfg.razgonTemp) + "C"
                 + "  asVolume: " + String(cfg.asVolume) + "L"
                 + "  nasebTime: " + String(cfg.nasebTime) + "min"
                 + "  reklapTime: " + String(cfg.reklapTime) + "min");
    }
    // ============================
    changeStage(Stage::WATER_TEST);
    return true;
}

bool ProcessEngine::stopCurrentProcess() {
    if (!processRunning) return false;
    processRunning = false;
    activeProcess = PROCESS_NONE;
    configManager->stopProcess();
    changeStage(Stage::IDLE);
    return true;
}

void ProcessEngine::emergencyStop() {
    emergencyState = true;
    processRunning = false;
    outputManager->emergencyStop();
    // === ЛОГИРОВАНИЕ ===
    logger.log("!!! EMERGENCY STOP TRIGGERED !!!");
    // ==================
    changeStage(Stage::IDLE);
    currentStatus.stageName = "EMERGENCY";
}

// ==================== STAGE HANDLERS ====================
void ProcessEngine::printStartupInfo() {
    Serial.println(F("\n========================================"));
    Serial.println(F("       SYSTEM STARTUP REPORT"));
    Serial.println(F("========================================"));
    
    // 1. Загрузка конфигурации
    SystemConfig cfg = configManager->getConfig();
    
    // 2. Прогрев датчиков
    Serial.println(F("Sensors warm-up..."));
    sensorAdapter->update(); 
    delay(1000);             
    sensorAdapter->update(); 
    
    const SensorData& data = sensorAdapter->getData();

    // --- БЛОК 1: СИСТЕМА ---
    Serial.println(F("--- SYSTEM CONFIG ---"));
    Serial.print(F("  Heater Type    : ")); Serial.println(cfg.heaterType);
    Serial.print(F("  Power (W)      : ")); Serial.println(cfg.power);
    Serial.print(F("  Full Power     : ")); Serial.println(cfg.fullPwr ? "YES" : "NO");

    // --- БЛОК 2: ДИСТИЛЛЯЦИЯ (DIST) ---
    Serial.println(F("\n--- DISTILLATION SETTINGS ---"));
    Serial.print(F("  Valve Use      : ")); Serial.println(cfg.valveuse ? "YES" : "NO");
    Serial.print(F("  Midterm Temp   : ")); Serial.print(cfg.midterm); Serial.println(" C");
    Serial.print(F("  BakStop Temp   : ")); Serial.print(cfg.bakStopTemp); Serial.println(" C");
    Serial.print(F("  Mixer Enabled  : ")); Serial.println(cfg.mixerEnabled ? "YES" : "NO");
    if (cfg.mixerEnabled) {
        Serial.print(F("  Mixer On (s)   : ")); Serial.println(cfg.mixerOnTime);
        Serial.print(F("  Mixer Off (s)  : ")); Serial.println(cfg.mixerOffTime);
    }

    // --- БЛОК 3: РЕКТИФИКАЦИЯ (RECT) ---
    Serial.println(F("\n--- RECTIFICATION SETTINGS ---"));
    Serial.print(F("  AS_Volume      : ")); Serial.println(cfg.asVolume);
    Serial.print(F("  Cycle Limit    : ")); Serial.println(cfg.cycleLim);
    Serial.print(F("  Calibraton     : ")); Serial.println(cfg.calibration ? "YES" : "NO");
    Serial.print(F("  Razgon Temp    : ")); Serial.print(cfg.razgonTemp); Serial.println(" C");
    Serial.print(F("  Naseb Time(m)  : ")); Serial.println(cfg.nasebTime);
    Serial.print(F("  Reklap Time(m) : ")); Serial.println(cfg.reklapTime);
    
    // --- БЛОК 4: КЛАПАНА (VALVES) ---
    Serial.println(F("\n--- VALVE TIMINGS & CAPACITY ---"));
    // Головы
    Serial.print(F("  Use Head Valve : ")); Serial.println(cfg.useHeadValve ? "YES" : "NO");
    Serial.print(F("  Heads Type KSS : ")); Serial.println(cfg.headsTypeKSS ? "YES" : "NO");
    Serial.print(F("  Head Open (s)  : ")); Serial.println(cfg.headOpenMs);
    Serial.print(F("  Head Close (s) : ")); Serial.println(cfg.headCloseMs);
    Serial.print(F("  Head Cap (ml/m): ")); Serial.println(cfg.valve_head_capacity);
    // Тело
    Serial.print(F("  Body Valve NC  : ")); Serial.println(cfg.bodyValveNC ? "NC" : "NO");
    Serial.print(F("  Body Open (s)  : ")); Serial.println(cfg.bodyOpenMs);
    Serial.print(F("  Body Close (s) : ")); Serial.println(cfg.bodyCloseMs);
    Serial.print(F("  Body Cap (ml/m): ")); Serial.println(cfg.valve_body_capacity);
    Serial.print(F("  Body Cap0(NO)  : ")); Serial.println(cfg.valve0_body_capacity);
    
    // Параметры TELO
    Serial.print(F("  Delta (C)      : ")); Serial.println(cfg.delta);
    Serial.print(F("  Histeresis (C) : ")); Serial.println(cfg.histeresis);
    // Correlation теперь константа, но выводим то, что в памяти для информации
    Serial.print(F("  Correlation(E) : ")); Serial.println(cfg.correlation); 

    // --- БЛОК 5: БЕЗОПАСНОСТЬ И СЕТЬ ---
    Serial.println(F("\n--- SAFETY & NETWORK ---"));
    Serial.print(F("  TSA Limit (C)  : ")); Serial.println(cfg.tsaLimit);
    Serial.print(F("  Box Max Temp(C): ")); Serial.println(cfg.boxMaxTemp);
    Serial.print(F("  Emergency (min): ")); Serial.println(cfg.emergencyTime);
    Serial.print(F("  WiFi Check(m)  : ")); Serial.println(cfg.chekwifi);     // ИСПРАВЛЕНО
    Serial.print(F("  Active Test(s) : ")); Serial.println(cfg.active_test);  // ИСПРАВЛЕНО

    // --- БЛОК 6: ДАТЧИКИ ---
    Serial.println(F("\n--- SENSORS (CURRENT) ---"));
    Serial.print(F("  TSA (Cube)     : ")); Serial.print(data.tsa.value); Serial.println(" C");
    Serial.print(F("  TSAR (Column)  : ")); Serial.print(data.tsar.value); Serial.println(" C");
    Serial.print(F("  AQUA (Water)   : ")); Serial.print(data.aqua.value); Serial.println(" C");
    Serial.print(F("  TANK (Def)     : ")); Serial.print(data.tank.value); Serial.println(" C");
    Serial.print(F("  Pressure (hPa) : ")); Serial.println(data.pressure);
    Serial.print(F("  Box Temp       : ")); Serial.print(data.boxTemp); Serial.println(" C");
    
    Serial.println(F("========================================"));
    Serial.println(F("System Ready. Waiting for command..."));
    Serial.println();
    logger.log("System Ready. Waiting for command...");
}
void ProcessEngine::handleIdleState() {}

void ProcessEngine::handleWaterTest() {
    currentStatus.stageName = "WATER_TEST"; 
    if (!dialogPending) {
        outputManager->openWaterValve(); 
        if (activeProcess == PROCESS_DIST && configManager->getConfig().mixerEnabled) outputManager->startMixer();
        dialogPending = true;
        
    }
}

void ProcessEngine::handleRazgon() {
    currentStatus.stageName = "RAZGON";
    const SensorData& data = sensorAdapter->getData();
    SystemConfig& cfg = configManager->getConfig(); 

    if (cfg.heaterType == 0) { 
        if (activeProcess == PROCESS_RECT) outputManager->setHeater(true, true); 
        else outputManager->setHeater(true, cfg.fullPwr);
    } else {
        outputManager->setHeater(false, false);
    }

    if (activeProcess == PROCESS_DIST && cfg.mixerEnabled) {
        if (!outputManager->isMixerCycling()) outputManager->startMixerCycling(cfg.mixerOnTime, cfg.mixerOffTime);
    } else if (outputManager->isMixerCycling()) outputManager->stopMixer();

    if (data.tank.value >= cfg.razgonTemp) {
        Serial.println("[Process] RAZGON complete.");
        logger.log("[Process] RAZGON complete."
                 + String("  TSA: ") + String(data.tsa.value, 1) + "C"
                 + "  time: " + String(millis() / 60000) + "min");
        if (cfg.heaterType == 0) outputManager->setHeater(true, false);
        outputManager->openWaterValve();

                if (activeProcess == PROCESS_RECT) {
            // Инициализация клапанов для RECT
            if (cfg.useHeadValve) outputManager->closeHeadValve(); // Головы всегда NC -> Close = LOW
            outputManager->closeBodyValve(); // Close учитывает тип (NC->LOW, NO->HIGH)
        } else {
            // Инициализация клапанов для DIST
            if (cfg.useHeadValve) outputManager->closeHeadValve();
            // В DIST используется только НО клапан.
            // "Открыть" для НО -> LOW. Функция openBodyValve сделает это, если в конфиге NO.
            if (cfg.valveuse) outputManager->openBodyValve(); 
        }
        changeStage(Stage::WAITING);
    }
}

void ProcessEngine::handleWaiting() {
    currentStatus.stageName = "WAITING";
    const SensorData& data = sensorAdapter->getData();
    SystemConfig& cfg = configManager->getConfig(); 

    outputManager->openWaterValve();

    if (activeProcess == PROCESS_DIST && cfg.mixerEnabled) {
        if (!outputManager->isMixerCycling()) outputManager->startMixerCycling(cfg.mixerOnTime, cfg.mixerOffTime);
    } else if (outputManager->isMixerCycling()) outputManager->stopMixer();

    if (activeProcess == PROCESS_RECT && !cfg.bodyValveNC) outputManager->closeBodyValve();

    if (previousStage != Stage::WAITING) { 
        if (data.isTsarValid()) {
            initialTsarTemp = data.tsar.value;
        } else {
            initialTsarTemp = 0.0f;
            logger.log("WARNING: TSAR not valid at WAITING start! Waiting for sensor...");
            Serial.println("[WAITING] TSAR invalid at start, waiting for sensor recovery.");
        }
        logger.log("WAITING: start. TSAR: " + String(data.tsar.value, 1) + "C"
                 + "  TSA: " + String(data.tsa.value, 1) + "C");
        previousStage = Stage::WAITING; 
    }

    // Выход только если TSAR валиден и прогрелся на 5°C от начальной точки
    if (data.isTsarValid() && data.tsar.value >= (initialTsarTemp + 5.0f)) {
        logger.log("WAITING: complete. TSAR: " + String(data.tsar.value, 1) + "C"
                 + "  time: " + String(currentStatus.stageTimeSec / 60) + "min");
        if (activeProcess == PROCESS_DIST) changeStage(Stage::OTBOR);
        else changeStage(Stage::NASEBYA);
    }
}

// ==================== DIST LOGIC ====================

void ProcessEngine::handleDistOtbor() {
    currentStatus.stageName = "OTBOR";
    const SensorData& data = sensorAdapter->getData();
    SystemConfig& cfg = configManager->getConfig(); 

    if (previousStage != Stage::OTBOR) {
        logger.log("OTBOR: start. TANK: " + String(data.tank.value, 1) + "C");
        previousStage = Stage::OTBOR;
    }

    if (cfg.mixerEnabled) {
        if (!outputManager->isMixerCycling()) outputManager->startMixerCycling(cfg.mixerOnTime, cfg.mixerOffTime);
    } else if (outputManager->isMixerCycling()) outputManager->stopMixer();

        // Логика Midterm (Смена посуды)
    if (!midtermHandled) {
        if (data.tank.value >= cfg.midterm) { 
            
            // === УЧЕТ ДАВЛЕНИЯ ===
            float pressure_mmHg = data.pressure * 0.75006;
            // Приводим текущую температуру бака к "нормальному" давлению
            float tankCorrected = data.tank.value + (760.0 - pressure_mmHg) * 0.037;

            // Сравниваем приведенную температуру с уставкой
            if (tankCorrected >= cfg.midterm) {
                if (cfg.valveuse) outputManager->closeBodyValve();
                midtermHandled = true;
                logger.log("OTBOR: midterm reached. T=" + String(data.tank.value, 1) + "C (Corrected: " + String(tankCorrected, 1) + "C)");
                changeStage(Stage::REPLACEMENT);
                return;
            }
        }
    }

    // Логика остановки по температуре бака
    if (data.tank.value >= cfg.bakStopTemp) {
        if (cfg.valveuse) outputManager->closeBodyValve();
        logger.log("OTBOR: bakStop reached. TANK: " + String(data.tank.value, 1) + "C");
        changeStage(Stage::BAKSTOP);
    }
}

void ProcessEngine::handleDistReplacement() { currentStatus.stageName = "REPLACEMENT"; }

void ProcessEngine::handleDistBakstop() {
    currentStatus.stageName = "BAKSTOP";

    if (previousStage != Stage::BAKSTOP) {
        const SensorData& data = sensorAdapter->getData();
        outputManager->setHeaterPowerOff();
        logger.log("BAKSTOP: start. TANK: " + String(data.tank.value, 1) + "C");
        logger.log("BAKSTOP: Heater power OFF (contactor kept)");
        Serial.println("[BAKSTOP] Heater power pins OFF");
        previousStage = Stage::BAKSTOP;
    }

    if (currentStatus.stageTimeSec >= 5) changeStage(Stage::FINISHING_WORK);
}

// ==================== RECT LOGIC ====================

void ProcessEngine::handleNasebya() {
    currentStatus.stageName = "NASEBYA";
    SystemConfig& cfg = configManager->getConfig();
    outputManager->openWaterValve();

    if (previousStage != Stage::NASEBYA) {
        const SensorData& data = sensorAdapter->getData();
        logger.log("NASEBYA: start. TSAR: " + String(data.tsar.value, 1) + "C"
                 + "  target: " + String(cfg.nasebTime) + "min");
        previousStage = Stage::NASEBYA;
    }
    
    unsigned long targetSeconds = (unsigned long)cfg.nasebTime * 60;
    
    // === ЛОГИКА ЗАВЕРШЕНИЯ СТАБИЛИЗАЦИИ ===
    if (currentStatus.stageTimeSec >= targetSeconds) {
        const SensorData& data = sensorAdapter->getData();
        Serial.println("[Process] NASEBYA complete.");
        logger.log("[Process] NASEBYA complete. TSAR: " + String(data.tsar.value, 1) + "C");

        // 1. Первый вход (counter == 0) -> переход к настройкам или калибровке
        if (counter == 0) {
            if (cfg.calibration) {
                changeStage(Stage::VALVE_CAL);
                valveCalMenu->display(); 
            } else {
                changeStage(Stage::SET_PW_AS);
                if (setPwAsMenu) setPwAsMenu->display();
            }
        } 
        // 2. Повторный вход (только для метода Стандарт)
        // Шпора сюда не попадает, она сразу финишит при Залёте.
        else {
            const SensorData& data = sensorAdapter->getData();
            
            // Пересчитываем поправку давления (актуальная на текущий момент)
            float pCorr = (data.pressure - adPressM) * 0.0278;
            
            // Порог возврата: должна упасть ниже уставки Залёта (rtsarM + Histeresis)
            float threshold = rtsarM + cfg.histeresis + pCorr;

            logger.log("NASEBYA (zalyot): TSAR: " + String(data.tsar.value, 2) + "C"
                     + "  threshold: " + String(threshold, 2) + "C"
                     + "  time: " + String(currentStatus.stageTimeSec / 60) + "min");

            if (data.tsar.value < threshold) {
                Serial.println("[Process] TSAR normalized. Resuming TELO.");
                logger.log("TSAR normalized. Resuming TELO.");
                changeStage(Stage::TELO);
            } else {
                Serial.println("[Process] TSAR still high! Finishing process.");
                logger.log("TSAR still high after stabilization! Finishing process.");
                changeStage(Stage::FINISHING_WORK);
            }
        }
    }
}

// ==================== GOLOVY LOGIC ====================

// === РАСЧЁТ ТАЙМИНГОВ КЛАПАНА (с уменьшением износа) ===
void ProcessEngine::calcValveTiming(float targetSpeed, float capacity, int& openMs, int& closeMs) {
    SystemConfig& cfg = configManager->getConfig();
    int minOpen = cfg.minOpenTime > 0 ? cfg.minOpenTime : 100;
    
    if (capacity <= 0) {
        openMs = minOpen;
        closeMs = 1000;
        return;
    }
    
    float dutyCycle = targetSpeed / (capacity * 60.0f);
    if (dutyCycle > 0.9f) dutyCycle = 0.9f;
    if (dutyCycle < 0.001f) dutyCycle = 0.001f;
    
    // === ОПТИМИЗАЦИЯ ИЗНОСА КЛАПАНА ===
    // Используем minOpen как базу для длины цикла
    // Если нужна большая скорость - увеличиваем время открытия (меньше срабатываний)
    // Если нужна маленькая скорость - используем minOpen как раньше
    
    float idealOpenMs = dutyCycle * minOpen;  // Идеальное время открытия для данного duty cycle
    
    if (idealOpenMs >= minOpen) {
        // Большая скорость: увеличиваем время открытия, уменьшаем износ
        openMs = (int)(idealOpenMs + 0.5f);
        closeMs = minOpen - openMs;  // closeMs теперь меньше
    } else {
        // Маленькая скорость: используем минимум
        openMs = minOpen;
    }
    
    // Рассчитываем closeMs для маленькой скорости
    float closeMsCalc = (float)openMs * (1.0f - dutyCycle) / dutyCycle;
    
    if (idealOpenMs < minOpen) {
        closeMs = (int)(closeMsCalc + 0.5f);
    }
    
    if (closeMs < 100) closeMs = 100;
}

void ProcessEngine::handleGolovy() {
    currentStatus.stageName = "GOLOVY";
    SystemConfig& cfg = configManager->getConfig();

    // === Расчет накопленного объема голов (Вариант A) ===
    // Скорость рассчитывается по реальному расходу клапана с учётом duty cycle
    unsigned long now = millis();
    float dt_h = (float)(now - lastVolCalcTime) / 3600000.0f;
    lastVolCalcTime = now;

    // Расчёт реальной скорости отбора
    float accumSpeed = 0.0f;
    float corrFactor = cfg.speedHeadCorr / 100.0f;  // Коррекция скорости голов (0-200%)
    
    // === ВАЖНО: Рассчитываем скорости ВСЕГДА ===
    koff = cfg.power / 1000.0f;
    speedGolovy = koff * 50.0f;
    speedTelo = koff * 500.0f;
    // ===============================================================
    
    if (currentGolovyStage != GolovyStage::IDLE) {
        if (currentGolovyStage == GolovyStage::KSS_SPIT) {
            // Spit: клапан открыт постоянно → реальная пропускная способность
            float cap = cfg.useHeadValve    ? (float)cfg.valve_head_capacity :
                        cfg.bodyValveNC     ? (float)cfg.valve_body_capacity :
                                              (float)cfg.valve0_body_capacity;
            accumSpeed = cap * 60.0f;  // перевести мл/мин в мл/ч
        }
        else {
            // Standard/AkaTelo: используем эталонную скорость
            // speedGolovy уже рассчитана как 50 * koff для голов
            // speedTelo рассчитана как 500 * koff для тела
            if (currentGolovyStage == GolovyStage::KSS_STANDARD || 
                currentGolovyStage == GolovyStage::ST_MAIN) {
                accumSpeed = speedGolovy * corrFactor;
            } else {
                // KSS_AKATELO - используем скорость тела
                accumSpeed = speedTelo * (cfg.speedBodyCorr / 100.0f);
            }
        }
    }
    headsVolDone += accumSpeed * dt_h;
    headsVolSub += accumSpeed * dt_h;  // Также считаем объём для текущего подэтапа

    // Обновляем статус для веба
    currentStatus.headsVolDone = headsVolDone;
    currentStatus.headsVolSub = headsVolSub;  // Добавлено: объём для подэтапа
    currentStatus.headsSpeed = accumSpeed;        // Реальная скорость по capacity клапана
    currentStatus.headsSpeedCalc = speedGolovy;   // Расчётная скорость для времени этапа
    currentStatus.bodySpeed = 0.0f;
    // ==============================================
    
    if (currentGolovyStage == GolovyStage::IDLE) {
        const SensorData& data = sensorAdapter->getData();
        logger.log("GOLOVY: init. TSAR: " + String(data.tsar.value, 1) + "C"
                 + "  koff: " + String(koff, 2) + "  speed: " + String(speedGolovy, 1) + " ml/h");

        if (cfg.headsTypeKSS) startKssSpit(cfg);
        else startStandardGolovy(cfg);
    }

    switch (currentGolovyStage) {
        case GolovyStage::ST_MAIN:
            if (headsVolSub >= currentStatus.rectVolumeTarget) {
                Serial.println("[GOLOVY] ST Main complete.");
                logger.log("GOLOVY: Standard Stage Complete");
                finishGolovyStage(); 
            }
            break;

        case GolovyStage::KSS_SPIT:
            if (headsVolSub >= currentStatus.rectVolumeTarget) {
                Serial.println("[GOLOVY] KSS Spit complete. Vol: " + String(headsVolSub, 1) + " ml");
                logger.log("[GOLOVY] KSS Spit complete. Vol: " + String(headsVolSub, 1) + " ml");
                if (cfg.useHeadValve) outputManager->closeHeadValve();
                else outputManager->closeBodyValve();
                
                startKssStandard(cfg);
            }
            break;

        case GolovyStage::KSS_STANDARD:
            if (headsVolSub >= currentStatus.rectVolumeTarget) {
                Serial.println("[GOLOVY] KSS Standard complete. Vol: " + String(headsVolSub, 1) + " ml");
                logger.log("[GOLOVY] KSS Standard complete. Vol: " + String(headsVolSub, 1) + " ml");
                outputManager->stopValveCycling();
                startKssAkaTelo(cfg);
            }
            break;

        case GolovyStage::KSS_AKATELO:
            if (headsVolSub >= currentStatus.rectVolumeTarget) {
                Serial.println("[GOLOVY] KSS AkaTelo complete. Vol: " + String(headsVolSub, 1) + " ml");
                logger.log("[GOLOVY] KSS AkaTelo complete. Vol: " + String(headsVolSub, 1) + " ml");
                finishGolovyStage();
            }
            break;
        default: break;
    }
}

void ProcessEngine::handleGolovyOk() {
    currentStatus.stageName = "GOLOVY_OK";  // Исправлено: было "GOLOVY OK?" (с пробелом)
    outputManager->openWaterValve();
}

// --- Вспомогательные функции GOLOVY ---

// --- Standard ---
void ProcessEngine::startStandardGolovy(SystemConfig& cfg) {
    float headVol = cfg.asVolume * 0.1f; 
    float vHeadMin = (headVol / speedGolovy) * 60.0f;
    golovyTargetTime = (unsigned long)(vHeadMin * 60.0f); 

    currentGolovyStage = GolovyStage::ST_MAIN;
    stageStartTime = millis();
    
    // === СБРОС СЧЁТЧИКА ОБЪЁМА ПОДЭТАПА ===
    headsVolSub = 0.0f;
    currentStatus.headsVolTarget = (int)headVol;
    currentStatus.rectVolumeTarget = (int)headVol;  // Используется для проверки завершения
    // =====================================

    // === ЛОГИРОВАНИЕ ===
    logger.log("GOLOVY: Standard Start");
    logger.log("  Target Vol: " + String(headVol, 1) + " ml");
    logger.log("  Duration: " + String(vHeadMin, 1) + " min");
    // ==================
    
    Serial.print("[GOLOVY] ST Method. Time: "); Serial.print(vHeadMin); Serial.println(" min");
    Serial.print("[GOLOVY] ST Method. Speed: "); Serial.print(speedGolovy, 1); Serial.println(" ml/h");

    if (cfg.useHeadValve) {
        // Конфиг 1 или 2: Есть клапан голов
        int openMs, closeMs;
        float targetSpeed = speedGolovy * (cfg.speedHeadCorr / 100.0f);
        calcValveTiming(targetSpeed, (float)cfg.valve_head_capacity, openMs, closeMs);
        Serial.printf("[GOLOVY] ST: Calc timing - open=%dms, close=%dms, target=%.1f ml/h\n", 
            openMs, closeMs, targetSpeed);
        outputManager->startHeadValveCycling(openMs, closeMs);
        outputManager->closeBodyValve(); // Тело закрыто
    } else {
        // Нет клапана голов, работаем через тело
        if (cfg.bodyValveNC) {
            // Конфиг 3: Body(NC). Импульсный режим (тайминги голов!)
            int openMs, closeMs;
            float targetSpeed = speedGolovy * (cfg.speedHeadCorr / 100.0f);
            calcValveTiming(targetSpeed, (float)cfg.valve_head_capacity, openMs, closeMs);
            outputManager->startBodyValveCycling(openMs, closeMs);
        } else {
            // Конфиг 4: Body(NO). Ручной режим.
            outputManager->openBodyValve(); // Открыть (LOW)
            Serial.println("[GOLOVY] Manual mode (NO valve)");
            logger.log("[GOLOVY] Manual mode (NO valve)");
        }
    }
}

// --- KSS Spit ---
void ProcessEngine::startKssSpit(SystemConfig& cfg) {
    float headVol = cfg.asVolume * 0.02f;
float vHeadMin = 0;

// Определяем актуальную пропускную способность в зависимости от конфига клапана
float cap = cfg.useHeadValve    ? (float)cfg.valve_head_capacity :
            cfg.bodyValveNC     ? (float)cfg.valve_body_capacity :
                                  (float)cfg.valve0_body_capacity;

// Защита от деления на ноль: если capacity не откалиброван или = 0,
// подставляем минимум 1 мл/мин и пишем предупреждение в лог
if (cap < 1.0f) {
    cap = 1.0f;
    logger.log("WARNING: KSS Spit capacity = 0! Using 1 ml/min. Check VALVE CAL settings.");
    Serial.println("[GOLOVY] WARNING: valve capacity = 0, using fallback 1 ml/min");
}

vHeadMin = headVol / cap;

    golovyTargetTime = (unsigned long)(vHeadMin * 60.0f); 
    currentGolovyStage = GolovyStage::KSS_SPIT;
    stageStartTime = millis();
    
    // === СБРОС СЧЁТЧИКА ОБЪЁМА ПОДЭТАПА ===
    headsVolSub = 0.0f;
    currentStatus.headsVolTarget = (int)headVol;
    currentStatus.rectVolumeTarget = (int)headVol;  // Используется для проверки завершения
    // =====================================

    Serial.print("[GOLOVY] KSS Spit. Time: "); Serial.print(vHeadMin); Serial.println(" min");
    Serial.print("[GOLOVY] KSS Spit. Speed: "); Serial.print(cap, 1); Serial.println(" ml/min (full flow)");
    // === ЛОГИРОВАНИЕ ===
    logger.log("GOLOVY: KSS Spit Start");
    logger.log("  Target Vol: " + String(headVol, 1) + " ml");
    logger.log("  Duration: " + String(vHeadMin, 1) + " min");
    // ==================

    if (cfg.useHeadValve) {
        outputManager->openHeadValve(); // Открыть Головы
        outputManager->closeBodyValve();
    } else {
        // Работаем через тело
        outputManager->openBodyValve(); // Открыть (учитывает NC/NO)
    }
}

// --- KSS Standard ---
void ProcessEngine::startKssStandard(SystemConfig& cfg) {
    float headVol = cfg.asVolume * 0.03f;
    float vHeadMin = (headVol / speedGolovy) * 60.0f;
    golovyTargetTime = (unsigned long)(vHeadMin * 60.0f); 
    
    currentGolovyStage = GolovyStage::KSS_STANDARD;
    stageStartTime = millis();
    
    // === СБРОС СЧЁТЧИКА ОБЪЁМА ПОДЭТАПА ===
    headsVolSub = 0.0f;
    currentStatus.headsVolTarget = (int)headVol;
    currentStatus.rectVolumeTarget = (int)headVol;  // Используется для проверки завершения
    // =====================================

    Serial.print("[GOLOVY] KSS Standard. Time: "); Serial.print(vHeadMin); Serial.println(" min");
    Serial.print("[GOLOVY] KSS Standard. Speed: "); Serial.print(speedGolovy, 1); Serial.println(" ml/h");
    Serial.printf("[GOLOVY] DEBUG: headCap=%d, koff=%.2f, speedHeadCorr=%d\n",
        cfg.valve_head_capacity, koff, cfg.speedHeadCorr);
    // === ЛОГИРОВАНИЕ ===
    logger.log("GOLOVY: KSS Standard Start");
    logger.log("  Target Vol: " + String(headVol, 1) + " ml");
    logger.log("  Duration: " + String(vHeadMin, 1) + " min");
    // ==================

    if (cfg.useHeadValve) {
        int openMs, closeMs;
        float targetSpeed = speedGolovy * (cfg.speedHeadCorr / 100.0f);
        calcValveTiming(targetSpeed, (float)cfg.valve_head_capacity, openMs, closeMs);
        Serial.printf("[GOLOVY] KSS_STD: Calc timing - open=%dms, close=%dms, target=%.1f ml/h\n", 
            openMs, closeMs, targetSpeed);
        outputManager->startHeadValveCycling(openMs, closeMs);
        outputManager->closeBodyValve();
    } else {
        if (cfg.bodyValveNC) {
            int openMs, closeMs;
            float targetSpeed = speedGolovy * (cfg.speedHeadCorr / 100.0f);
            calcValveTiming(targetSpeed, (float)cfg.valve_head_capacity, openMs, closeMs);
            outputManager->startBodyValveCycling(openMs, closeMs);
        }
    }
}

// --- KSS AkaTelo ---
void ProcessEngine::startKssAkaTelo(SystemConfig& cfg) {
    float headVol = cfg.asVolume * 0.15f;
    float vHeadMin = (headVol / speedTelo) * 60.0f;
    golovyTargetTime = (unsigned long)(vHeadMin * 60.0f); 
    
    currentGolovyStage = GolovyStage::KSS_AKATELO;
    stageStartTime = millis();
    
    // === СБРОС СЧЁТЧИКА ОБЪЁМА ПОДЭТАПА ===
    headsVolSub = 0.0f;
    currentStatus.headsVolTarget = (int)headVol;
    currentStatus.rectVolumeTarget = (int)headVol;  // Используется для проверки завершения
    // =====================================

    Serial.print("[GOLOVY] KSS AkaTelo. Time: "); Serial.print(vHeadMin); Serial.println(" min");
    Serial.print("[GOLOVY] KSS AkaTelo. Speed: "); Serial.print(speedTelo, 1); Serial.println(" ml/h");
    // === ЛОГИРОВАНИЕ ===
    logger.log("GOLOVY: KSS AkaTelo Start");
    logger.log("  Target Vol: " + String(headVol, 1) + " ml");
    logger.log("  Duration: " + String(vHeadMin, 1) + " min");
    // ==================

    if (cfg.useHeadValve) {
        int openMs, closeMs;
        float targetSpeed = speedTelo * (cfg.speedBodyCorr / 100.0f);
        float cap = (float)cfg.valve_body_capacity;
        calcValveTiming(targetSpeed, cap, openMs, closeMs);
        Serial.printf("[GOLOVY] KSS_AKATELO: Calc timing - open=%dms, close=%dms, target=%.1f ml/h\n", 
            openMs, closeMs, targetSpeed);
        outputManager->startHeadValveCycling(openMs, closeMs);
        outputManager->closeBodyValve();
    } else {
        if (cfg.bodyValveNC) {
            int openMs, closeMs;
            float targetSpeed = speedTelo * (cfg.speedBodyCorr / 100.0f);
            float cap = (float)cfg.valve_body_capacity;
            calcValveTiming(targetSpeed, cap, openMs, closeMs);
            outputManager->startBodyValveCycling(openMs, closeMs);
        } else {
            // Body(NO) -> Ручной
            outputManager->openBodyValve();
            Serial.println("[GOLOVY] Manual mode (KSS AkaTelo)");
        }
    }
}

void ProcessEngine::finishGolovyStage() {
    currentGolovyStage = GolovyStage::IDLE; 
    Serial.println("[Process] -> GOLOVY OK?");
    
    // Клапан (голова или тело) ПРОДОЛЖАЕТ работать в том же режиме до подтверждения
    // После подтверждения -> TELO, где будет доотбор голов (если есть клапан голов) и старт клапана тела
    
    // === ЛОГИРОВАНИЕ ===
    logger.log("GOLOVY: All Heads Stages Complete. Waiting for user confirmation.");
    // ==================
    changeStage(Stage::GOLOVY_OK);
}

// ==================== TELO LOGIC ====================

void ProcessEngine::handleTelo() {
    currentStatus.stageName = "TELO";
    
    SystemConfig& cfg = configManager->getConfig();
    const SensorData& data = sensorAdapter->getData();

    // Статическая переменная для отслеживания предыдущего состояния BME
    static bool bmeWasAvailable = true;
    // Статические для логирования только при изменении
    static int lastOpenMs = -1;
    static int lastCloseMs = -1;

    // 1. Инициализация референсных значений (только при первом входе)
    if (rtsarM < 0.1f) { 
        koff = cfg.power / 1000.0f; 
        rtsarM = data.tsar.value;
        
        // === ИЗМЕНЕНО: Безопасный захват давления ===
        // Если BME жив - берем текущее. Если нет - берем что есть (последнее известное или 0)
        adPressM = data.pressure; 
        
        // Логируем состояние датчика при старте этапа
        if (!currentStatus.bmeAvailable) {
            logger.log("WARNING: BME280 not available at TELO start! Pressure correction disabled.");
            Serial.println("[TELO] BME missing! Using frozen pressure values.");
            bmeWasAvailable = false; // Запоминаем, что его нет
        } else {
            bmeWasAvailable = true; // Сбрасываем флаг
        }
        // ==========================================
        
        bodyOpenCor = cfg.bodyOpenMs * koff;
        speedShpora = 500.0 * koff; 
        lastShporaAdjustTime = millis(); 
        bodyVolDone = 0.0f;  // Сброс накопленного объёма тела
        shporaReductionCount = 0; // Сброс счётчика снижений скорости 

        // === ЛОГИРОВАНИЕ СТАРТА ТЕЛО ===
        bool isShpora = (cfg.cycleLim == 1 && cfg.bodyValveNC);
        logger.log("TELO Start. Method: " + String(isShpora ? "SPORA" : "STANDARD"));
        logger.log("  rtsarM: " + String(rtsarM, 2) + "C"
                 + "  adPressM: " + String(adPressM, 1) + "hPa"
                 + "  koff: " + String(koff, 2));
        logger.log("  bodyOpenCor: " + String(bodyOpenCor, 1) + "s"
                 + "  Initial Speed: " + String(speedShpora, 1) + "ml/h");
        // ==============================
        
        
    }

    // 2. Расчет поправки давления
    // === НОВОЕ: Мониторинг потери BME в процессе ===
    // Если только что потеряли датчик (был true, стал false)
    if (bmeWasAvailable && !currentStatus.bmeAvailable) {
        logger.log("ERROR: BME280 connection lost during TELO! Using last known pressure.");
        Serial.println("[TELO] BME LOST! Pressure frozen at: " + String(data.pressure) + " hPa");
    }
    // Если датчик вернулся (был false, стал true)
    else if (!bmeWasAvailable && currentStatus.bmeAvailable) {
        logger.log("INFO: BME280 connection restored.");
        Serial.println("[TELO] BME Restored.");
    }
    // Обновляем состояние
    bmeWasAvailable = currentStatus.bmeAvailable;
    // ==============================================

    // 2. Расчет поправки давления
    // ВАЖНО: data.pressure содержит последнее удачное значение, если BME отвалился.
    // Поэтому формула остается БЕЗ ИЗМЕНЕНИЙ. Она автоматически будет давать 
    // коррекцию ~0, так как (FrozenPressure - adPressM) будет величиной постоянной.
    float pressureCorrection = (data.pressure - adPressM) * 0.0278;
    
    // 3. Расчёт накопленного объёма тела
    unsigned long now = millis();
    float dt_h = (float)(now - lastVolCalcTime) / 3600000.0f;
    lastVolCalcTime = now;
    
    float bodySpeedMlH = currentStatus.bodySpeed;
    bodyVolDone += bodySpeedMlH * dt_h;
    currentStatus.bodyVolDone = bodyVolDone;

    // 3. Логика тела
    // Метод Шпора доступен только для NC клапана! И если установлен 1 цикл в настройках Иначе работаем как Overhist
    bool isShpora = (cfg.cycleLim == 1 && cfg.bodyValveNC);

    // --- УПРАВЛЕНИЕ КЛАПАНОМ ТЕЛА ---
    if (isShpora) {
        // === МЕТОД 2: ШПОРА (Только NC) ===
        
        // Реакция на Delta
        if (data.tsar.value >= (rtsarM + cfg.delta + pressureCorrection)) {
            
            // ПРОВЕРКА ТАЙМЕРА: Прошло ли достаточно времени с прошлого снижения?
            if (millis() - lastShporaAdjustTime >= SHPORA_STABILIZATION_MS) {
                bodyOpenCor /= CORRELATION_COEFF;
                speedShpora /= CORRELATION_COEFF;
                shporaReductionCount++;
                
                lastShporaAdjustTime = millis(); // Запоминаем время корректировки
                
                Serial.print("[TELO] Shpora Delta! New Speed: "); 
                Serial.println(speedShpora);

                // === РАСШИРЕННОЕ ЛОГИРОВАНИЕ СНИЖЕНИЯ СКОРОСТИ ===
                float threshold = rtsarM + cfg.delta + pressureCorrection;
                unsigned long stageTimeSec = (millis() - stageStartTime) / 1000;
                unsigned long hours = stageTimeSec / 3600;
                unsigned long mins = (stageTimeSec % 3600) / 60;
                
                logger.log("TELO: Delta breach! TSAR=" + String(data.tsar.value, 2) + "C > threshold=" + String(threshold, 2) + 
                           "C (rtsarM=" + String(rtsarM, 2) + " + delta=" + String(cfg.delta, 2) + " + corr=" + String(pressureCorrection, 2) + ")");
                logger.log("TELO: Speed " + String(speedShpora * CORRELATION_COEFF, 1) + " -> " + String(speedShpora, 1) + 
                           " ml/h (reduction #" + String(shporaReductionCount) + ", time: " + String(hours) + "h " + String(mins) + "m)");
                // ========================================================
            }
        }

        // Реакция на Залёт
        if (data.tsar.value >= (rtsarM + cfg.histeresis + pressureCorrection)) {
            // Вариант А: Закрываем всё при Залёте
            finishTelo(cfg);
            // === ЛОГИРОВАНИЕ ЗАЛЁТА ===
            logger.log("TELO: HYSTERESIS BREACH! T=" + String(data.tsar.value, 2) + "C");
            // =========================
            return;
        }

        // === ПРОВЕРКА МИНИМАЛЬНОЙ СКОРОСТИ ===
        const float MIN_BODY_SPEED = 300.0f;
        if (speedShpora < MIN_BODY_SPEED) {
            logger.log("TELO: Min speed " + String(MIN_BODY_SPEED, 0) + " ml/h reached! Finishing.");
            Serial.println("[TELO] Min speed reached! Finishing TELO.");
            finishTelo(cfg);
            return;
        }
        // ======================================

        // Управление клапаном тела (через calcValveTiming)
        int openMs, closeMs;
        float targetSpeed = speedShpora * (cfg.speedBodyCorr / 100.0f);
        calcValveTiming(targetSpeed, (float)cfg.valve_body_capacity, openMs, closeMs);
        
        // Выводим только при изменении
        if (openMs != lastOpenMs || closeMs != lastCloseMs) {
            Serial.printf("[TELO] Shpora: open=%ums, close=%ums, speed=%.1f ml/h\n", 
                openMs, closeMs, speedShpora);
            lastOpenMs = openMs;
            lastCloseMs = closeMs;
        }
        outputManager->startBodyValveCycling(openMs, closeMs);

    }

    else {
        // === МЕТОД 1: OVERHIST ===
        
        if (cfg.bodyValveNC) {
            // NC Клапан: Импульсный режим
            int openMs, closeMs;
            float targetSpeed = speedTelo * (cfg.speedBodyCorr / 100.0f);
            calcValveTiming(targetSpeed, (float)cfg.valve_body_capacity, openMs, closeMs);
            outputManager->startBodyValveCycling(openMs, closeMs);
        } else {
            // NO Клапан: Ручной режим (Просто Открыть)
            outputManager->openBodyValve();
        }

        // Реакция на Залёт
        if (data.tsar.value >= (rtsarM + cfg.histeresis + pressureCorrection)) {
            // Вариант А: Закрываем всё при Залёте
            outputManager->stopValveCycling();
            if (cfg.useHeadValve) outputManager->closeHeadValve();
            outputManager->closeBodyValve(); // Закроет и NO, и NC

            // === ЛОГИРОВАНИЕ ЗАЛЁТА ===
            logger.log("TELO: HYSTERESIS BREACH! T=" + String(data.tsar.value, 2) + "C");
            // =========================

            counter++;
            
            if (counter >= cfg.cycleLim) {
                finishTelo(cfg);
            } else {
                // Повторная стабилизация
                
                cfg.nasebTime = cfg.reklapTime;
                changeStage(Stage::NASEBYA);
            }
            return;
        }
    }

    // === 4. ДООТБОР ГОЛОВ (Всегда в TELO) ===
    // Запускается каждый цикл loop, пока мы в TELO.
    // Если был Залёт -> мы ушли в return выше -> эта часть не выполняется -> клапан закрыт.
    // Если пришли из NASEBYA -> выполняется -> клапан открывается снова.
    // ВАЖНО: Доотбор голов на RECT не зависит от мощности, всегда 50 мл/ч
    if (cfg.useHeadValve) {
        int openMs, closeMs;
        float targetSpeed = 50.0f * (cfg.speedHeadCorr / 100.0f);  // Фиксированные 50 мл/ч
        calcValveTiming(targetSpeed, (float)cfg.valve_head_capacity, openMs, closeMs);
        outputManager->startHeadValveCycling(openMs, closeMs);
    }
}
void ProcessEngine::finishTelo(SystemConfig& cfg) {
    Serial.println("[Process] -> FINISHING WORK");
    
    outputManager->stopValveCycling();
    if (cfg.useHeadValve) outputManager->closeHeadValve();
    outputManager->closeBodyValve();
    
    changeStage(Stage::FINISHING_WORK);
}

// ==================== RECT PROCESS ROUTER ====================
void ProcessEngine::handleRectProcess() {
    if (currentStage == Stage::VALVE_CAL) {
       // currentStatus.stageName = "VALVE CAL";
        valveCalMenu->update();
        if (valveCalMenu->isReadyToExit()) {
             changeStage(Stage::SET_PW_AS);
             setPwAsMenu->display();
        }
    } 
    else if (currentStage == Stage::SET_PW_AS) {
        currentStatus.stageName = "SET_PW_AS";  // Добавлено для корректного отображения в web
        if (setPwAsMenu->isReadyToRun()) {
            currentGolovyStage = GolovyStage::IDLE; 
            changeStage(Stage::GOLOVY);
        }
    }
    else if (currentStage == Stage::GOLOVY) {
        handleGolovy();
    }
    else if (currentStage == Stage::GOLOVY_OK) {
        handleGolovyOk();
        // НЕ вызываем handleGolovy() - он запускает новые этапы!
    }
    else if (currentStage == Stage::TELO) {
        handleTelo();
    }
}

// ==================== FINISHING ====================

void ProcessEngine::handleFinishingWork() {
    currentStatus.stageName = "FINISHING";
    
    // Первый вход в этап
    if (previousStage != Stage::FINISHING_WORK) {
        // 1. Безопасность: Выключаем нагрев, открываем воду
        outputManager->setHeaterOff();
        outputManager->stopValveCycling();  // ← останавливаем циклы ПЕРЕД закрытием
        outputManager->closeHeadValve();    // ← закрываем клапан голов
        outputManager->closeBodyValve();    // ← закрываем клапан тела
        outputManager->openWaterValve();
        previousStage = Stage::FINISHING_WORK;
        Serial.println("[Process] FINISHING_WORK: Start 5 min cooling");
        logger.log("FINISHING: Start 5 min cooling");
    }

    // Таймер 5 минут (300 сек)
   
    unsigned long elapsedSec = (millis() - stageStartTime) / 1000;
     // === НОВОЕ: Расчет остатка времени для веб-интерфейса ===
    // Если прошло меньше 300 сек, вычисляем разницу. Иначе 0.
    currentStatus.finishingRemainSec = (elapsedSec < 300) ? (300 - elapsedSec) : 0;
    // ======================================================
    if (elapsedSec >= 300) {
        
        outputManager->closeWaterValve();
        outputManager->powerOffBodyValve();
        outputManager->stopMixer();
        processRunning = false;
        activeProcess = PROCESS_NONE;
        configManager->stopProcess();
        changeStage(Stage::IDLE);
        currentStatus.stageName = "ENDED";
        Serial.println("[Process] FINISHING_WORK complete.");
        logger.log("Process Finished");
    }
}

// ==================== DISPLAY ====================

void ProcessEngine::changeStage(Stage newStage) {
    previousStage = currentStage;
    currentStage = newStage;
    stageStartTime = millis();
    if (newStage != Stage::RAZGON) razgonMixerStarted = false;
    currentStatus.stageName = getStageName(newStage);
    
    // Вывод читаемого названия этапа
    Serial.print("[Engine] Stage: ");
    Serial.println(getStageName(newStage)); 
    // === ЛОГИРОВАНИЕ ===
  logger.log(String("Stage change: ") + getStageName(newStage));
    
    // === ИНИЦИАЛИЗАЦИЯ МЕНЮ ПРИ ПЕРЕХОДЕ НА НОВЫЕ ЭТАПЫ ===
    if (newStage == Stage::VALVE_CAL && valveCalMenu) {
        valveCalMenu->display();  // Показать мастер калибровки на LCD
        Serial.println("[Engine] Valve calibration menu displayed");
    }
    else if (newStage == Stage::SET_PW_AS && setPwAsMenu) {
        setPwAsMenu->display();   // Показать меню настройки мощности
    }
}

void ProcessEngine::updateDisplayData() {
    const SensorData& data = sensorAdapter->getData();
    SystemConfig& cfg = configManager->getConfig();

    // === НОВОЕ: Проверка доступности BME ===
    // Если статус не OK, считаем датчик недоступным
    currentStatus.bmeAvailable = (data.bmeStatus == SensorStatus::OK);
    // ======================================
    
    currentStatus.currentTsa = data.tsa.value;
    currentStatus.currentAqua = data.aqua.value;
    currentStatus.currentTsar = data.tsar.value;
    currentStatus.currentTank = data.tank.value;
    // Конвертируем гПа → мм рт.ст. для таблицы ABV (getABV ожидает мм рт.ст.)
// Коэффициент: 1 гПа = 0.75006 мм рт.ст.
    float pressure = data.pressure * 0.75006f;
    
    // Расчет крепости (getABV возвращает -1 если температура ниже 78.5°C)
    float abvBak = configManager->getABV(data.tank.value, pressure, false);
    currentStatus.currentStrengthBak = (abvBak >= 0) ? abvBak : 0;
    currentStatus.strengthBakValid = (abvBak >= 0);
    
    float abvOut = 0;
    if (activeProcess == PROCESS_DIST) {
        abvOut = configManager->getABV(data.tank.value, pressure, true);
    } else {
        abvOut = configManager->getABV(data.tsar.value, pressure, true);
    }
    currentStatus.currentStrength = (abvOut >= 0) ? abvOut : 0;
    currentStatus.strengthOutValid = (abvOut >= 0);
    
    static char buf[25]; // Буфер
    
    // === 1. DIST PROCESS ===
    if (activeProcess == PROCESS_DIST) {
        bool showStrength = (currentStage >= Stage::OTBOR);
        
        // --- Строка 0: TSA, DIST, Крепость куба ---
        char strAbvBak[8];
        if (showStrength && currentStatus.strengthBakValid) snprintf(strAbvBak, sizeof(strAbvBak), "%%%.0f", currentStatus.currentStrengthBak);
        else snprintf(strAbvBak, sizeof(strAbvBak), "%%--");
        
        // Используем символы W / A / X для статуса сети
        snprintf(buf, sizeof(buf), "%5.2f %s %s %7s", data.tsa.value, "DIST", currentStatus.networkSymbol.c_str(), strAbvBak);
        currentStatus.line0 = String(buf);
        
        // --- Строка 1: AQUA, Этап, Крепость в отборе ---
        char strAbvOut[8];
        if (showStrength && currentStatus.strengthOutValid) snprintf(strAbvOut, sizeof(strAbvOut), "%%%.0f", currentStatus.currentStrength);
        else snprintf(strAbvOut, sizeof(strAbvOut), "%%--");
        
        snprintf(buf, sizeof(buf), "%5.2f %-8.8s%6s", data.aqua.value, currentStatus.stageName.c_str(), strAbvOut);
        currentStatus.line1 = String(buf);

        // --- Строка 2: TSAR, Статус ---
        char statusBuf[15];
        if (currentStatus.safety == SafetyState::WARNING_TSA) {
            snprintf(statusBuf, sizeof(statusBuf), "ALARMTSA %s", formatTimeMMSS(currentStatus.alarmTimerSec).c_str());
        } else if (currentStatus.safety == SafetyState::WARNING_BOX) {
            snprintf(statusBuf, sizeof(statusBuf), "ATTENT %dC", (int)data.boxTemp);
        } else if (currentStatus.safety == SafetyState::EMERGENCY) {
            strcpy(statusBuf, "EMERGENCY");
        } else {
            strcpy(statusBuf, "NORMA");
        }
        snprintf(buf, sizeof(buf), "%5.2f %-14s", data.tsar.value, statusBuf);
        currentStatus.line2 = String(buf);

        // --- Строка 3: TANK, Время, Стоп ---
        unsigned long t = currentStatus.processTimeSec;
        char timeStr[6];
        snprintf(timeStr, sizeof(timeStr), "%02d:%02d", (int)(t/3600), (int)((t%3600)/60));
        snprintf(buf, sizeof(buf), "%5.2f %s %7d", data.tank.value, timeStr, cfg.bakStopTemp);
        currentStatus.line3 = String(buf);
    } 
    // === 2. RECT PROCESS ===
    else if (activeProcess == PROCESS_RECT) {
        if (currentStage == Stage::GOLOVY_OK) {
            currentStatus.line0 = " GOLOVY OK?";
            currentStatus.line1 = "> YES";
            currentStatus.line2 = "";
            currentStatus.line3 = "";
        } else {
            const char* methodStr = "ST"; 
            if (cfg.headsTypeKSS) methodStr = "KSS";
            
            snprintf(buf, sizeof(buf), "%5.2f RECT %s %s", data.tsa.value, currentStatus.networkSymbol.c_str(), methodStr);
            currentStatus.line0 = String(buf);
            
            // Строка 1 (TELO / GOLOVY)
            if (currentStage == Stage::TELO) {
                 bool isShpora = (cfg.cycleLim == 1 && cfg.bodyValveNC);
                 if (isShpora) {
                     float speedLh = speedShpora / 1000.0;
                     char teloBuf[15];
                     snprintf(teloBuf, sizeof(teloBuf), "TELO %.2f Sp", speedLh);
                     snprintf(buf, sizeof(buf), "%5.2f %-13s", data.aqua.value, teloBuf);
                     currentStatus.line1 = String(buf);
                 } else {
                     char teloBuf[15];
                     snprintf(teloBuf, sizeof(teloBuf), "TELO %d OV", counter);
                     snprintf(buf, sizeof(buf), "%5.2f %-13s", data.aqua.value, teloBuf);
                     currentStatus.line1 = String(buf);
                 }
            } 
            else if (currentStage == Stage::GOLOVY && golovyTargetTime > 0) {
                 long elapsed = currentStatus.stageTimeSec;
                 long remainSec = (elapsed < (long)golovyTargetTime) ? ((long)golovyTargetTime - elapsed) : 0;
                 int remainMin = (remainSec + 59) / 60; 
                 
                 const char* subName = "GOLOVY";
                 if (currentGolovyStage == GolovyStage::KSS_SPIT) subName = "GOLOVY Sp";
                 else if (currentGolovyStage == GolovyStage::KSS_STANDARD) subName = "GOLOVY St";
                 else if (currentGolovyStage == GolovyStage::KSS_AKATELO) subName = "GOLOVY Ak";
                 
                 snprintf(buf, sizeof(buf), "%5.2f %-10s %3d", data.aqua.value, subName, remainMin);
                 currentStatus.line1 = String(buf);
            } 
            else if (currentStage == Stage::NASEBYA) {
                 int totalSec = cfg.nasebTime * 60;
                 int elapsedSec = currentStatus.stageTimeSec;
                 int remainSec = (elapsedSec < totalSec) ? (totalSec - elapsedSec) : 0;
                 int remainMinNas = (remainSec + 59) / 60;
                 snprintf(buf, sizeof(buf), "%5.2f %-10s %3d", data.aqua.value, "NASEBYA", remainMinNas);
                 currentStatus.line1 = String(buf);
            } else {
                 snprintf(buf, sizeof(buf), "%5.2f %-10s", data.aqua.value, currentStatus.stageName.c_str());
                 currentStatus.line1 = String(buf);
            }
            
            // Строка 2 (Статус)
            char statusBuf[15];
            if (currentStatus.safety == SafetyState::WARNING_TSA) snprintf(statusBuf, sizeof(statusBuf), "ALARMTSA %s", formatTimeMMSS(currentStatus.alarmTimerSec).c_str());
            else if (currentStatus.safety == SafetyState::WARNING_BOX) snprintf(statusBuf, sizeof(statusBuf), "ATTENT %dC", (int)data.boxTemp);
            else if (currentStatus.safety == SafetyState::EMERGENCY) strcpy(statusBuf, "EMERGENCY");
            else strcpy(statusBuf, "NORMA");
            
            snprintf(buf, sizeof(buf), "%5.2f %-14s", data.tsar.value, statusBuf);
            currentStatus.line2 = String(buf);

            // Строка 3
            unsigned long t = currentStatus.processTimeSec;
            char timeStr[6];
            snprintf(timeStr, sizeof(timeStr), "%02d:%02d", (int)(t/3600), (int)((t%3600)/60));
            snprintf(buf, sizeof(buf), "%5.2f %s", data.tank.value, timeStr);
            currentStatus.line3 = String(buf);
        }
    } 
    else {
        currentStatus.line0 = "SYSTEM IDLE";
        currentStatus.line1 = "";
        currentStatus.line2 = "";
        currentStatus.line3 = "";
    }

         // === ОБНОВЛЕНИЕ СТАТУСА ТЕСТОВ ДЛЯ WEB ===
    // Head Test
    currentStatus.headTestActive = headTestStatus.active;
    if (headTestStatus.active) {
        unsigned long elapsed = (millis() - headTestStatus.startTime) / 1000;
        currentStatus.headTestRemainingSec = (elapsed < headTestStatus.durationSec) ? (headTestStatus.durationSec - elapsed) : 0;
        currentStatus.headTestTotalSec = headTestStatus.durationSec;
    } else {
        currentStatus.headTestRemainingSec = 0;
        currentStatus.headTestTotalSec = 0;
    }

    // Body Test
    currentStatus.bodyTestActive = bodyTestStatus.active;
    if (bodyTestStatus.active) {
        unsigned long elapsed = (millis() - bodyTestStatus.startTime) / 1000;
        currentStatus.bodyTestRemainingSec = (elapsed < bodyTestStatus.durationSec) ? (bodyTestStatus.durationSec - elapsed) : 0;
        currentStatus.bodyTestTotalSec = bodyTestStatus.durationSec;
    } else {
        currentStatus.bodyTestRemainingSec = 0;
        currentStatus.bodyTestTotalSec = 0;
    }
    // Флаг ожидания ввода мл
if (headTestStatus.awaitingInput) {
    currentStatus.testAwaitingInput = true;
    currentStatus.testAwaitingType = "head";
} else if (bodyTestStatus.awaitingInput) {
    currentStatus.testAwaitingInput = true;
    SystemConfig& cfg = configManager->getConfig();
    currentStatus.testAwaitingType = cfg.bodyValveNC ? "body_nc" : "body_no";
} else {
    currentStatus.testAwaitingInput = false;
    currentStatus.testAwaitingType = "";
}
}

String ProcessEngine::formatTimeMMSS(unsigned long seconds) const {
    int min = seconds / 60; int sec = seconds % 60;
    char buf[6]; snprintf(buf, sizeof(buf), "%02d:%02d", min, sec);
    return String(buf);
}

const SystemStatus& ProcessEngine::getStatus() const { return currentStatus; }
bool ProcessEngine::isProcessRunning() const { return processRunning; }
ProcessType ProcessEngine::getActiveProcessType() const { return activeProcess; }
String ProcessEngine::getProcessName() const { return (activeProcess == PROCESS_DIST ? "DIST" : "RECT"); }
String ProcessEngine::getStageName() const { return currentStatus.stageName; }

// Функция для получения названия этапа строкой
const char* ProcessEngine::getStageName(Stage stage) {
    switch (stage) {
        case Stage::IDLE: return "IDLE";
        case Stage::WATER_TEST: return "WATER_TEST";
        case Stage::RAZGON: return "RAZGON";
        case Stage::WAITING: return "WAITING";
        case Stage::OTBOR: return "OTBOR";
        case Stage::REPLACEMENT: return "REPLACEMENT";
        case Stage::BAKSTOP: return "BAKSTOP";
        case Stage::NASEBYA: return "NASEBYA";
        case Stage::VALVE_CAL: return "VALVE_CAL";
        case Stage::SET_PW_AS: return "SET_PW_AS";
        case Stage::GOLOVY: return "GOLOVY";
        case Stage::GOLOVY_OK: return "GOLOVY_OK";
        case Stage::TELO: return "TELO";
        case Stage::FINISHING_WORK: return "FINISHING_WORK";
        default: return "UNKNOWN";
    }
}

// === ОБНОВЛЕНИЕ ИНФОРМАЦИИ ДЛЯ WEB (RECT) ===
void ProcessEngine::updateRectWebInfo() {
    SystemConfig& cfg = configManager->getConfig();
    
    // 1. Информация о ГОЛОВАХ
    if (currentStage == Stage::GOLOVY) {
        // Метод
        currentStatus.rectMethodName = cfg.headsTypeKSS ? "KSS" : "STANDARD";
        
        // Расчет объема (AS Volume в мл)
        float asVol = (float)cfg.asVolume;
        float targetVol = 0;
        
        if (cfg.headsTypeKSS) {
            // KSS Logic
            switch (currentGolovyStage) {
                case GolovyStage::KSS_SPIT:
                    currentStatus.rectSubStage = "Spit";
                    targetVol = asVol * 0.02f; // 2%
                    break;
                case GolovyStage::KSS_STANDARD:
                    currentStatus.rectSubStage = "Standard";
                    targetVol = asVol * 0.03f; // 3%
                    break;
                case GolovyStage::KSS_AKATELO:
                    currentStatus.rectSubStage = "AkaTelo";
                    targetVol = asVol * 0.15f; // 15%
                    break;
                default:
                    currentStatus.rectSubStage = "Wait";
                    break;
            }
        } else {
            // Standard Logic
            currentStatus.rectSubStage = "Main";
            targetVol = asVol * 0.10f; // 10%
        }
        
        currentStatus.rectVolumeTarget = (int)targetVol;
        
        // Время (golovyTargetTime в секундах)
        long elapsed = currentStatus.stageTimeSec;
        long remain = (long)golovyTargetTime - elapsed;
        currentStatus.rectTimeRemaining = (remain > 0) ? (int)remain : 0;
    } 
    // Сброс при выходе из этапа
    else if (currentStage != Stage::GOLOVY_OK) {
        currentStatus.rectMethodName = "";
        currentStatus.headsSpeed = 0.0f;
        currentStatus.rectSubStage = "";
        currentStatus.rectTimeRemaining = 0;
        currentStatus.rectVolumeTarget = 0;
    }

    // 2. Информация о ТЕЛЕ
    if (currentStage == Stage::TELO) {
        // Метод
        bool isShpora = (cfg.cycleLim == 1 && cfg.bodyValveNC);
        currentStatus.bodyMethodName = isShpora ? "Шпора" : "Стандарт";
        
        // Скорость (speedShpora рассчитана в handleTelo, мл/час) с учётом коррекции
        currentStatus.bodySpeed = speedShpora * cfg.speedBodyCorr / 100.0f;
        
        // headsSpeed на TELO не нужен (не показываем) - не перезаписываем
        
        // Цикл
        currentStatus.bodyCycle = counter;
        
        // Расчёт оставшегося времени для TELO
        // Прогноз = AS - Heads - 15% tails
        float asVol = cfg.asVolume;
        float headsVol = headsVolDone;
        float tailsEst = asVol * 0.15f;
        float predictVol = asVol - headsVol - tailsEst;
        
        // Время = Объём / Скорость (в часах, переводим в секунды)
        if (speedShpora > 0 && predictVol > 0) {
            float timeHours = predictVol / speedShpora;
            int totalSec = (int)(timeHours * 3600.0f);
            // Вычитаем уже прошедшее время этапа
            int elapsedSec = currentStatus.stageTimeSec;
            int remainSec = totalSec - elapsedSec;
            currentStatus.rectTimeRemaining = (remainSec > 0) ? remainSec : 0;
        } else {
            currentStatus.rectTimeRemaining = 0;
        }
    } else {
        currentStatus.bodyMethodName = "";
        currentStatus.bodySpeed = 0;
        currentStatus.bodyCycle = 0;
    }
}