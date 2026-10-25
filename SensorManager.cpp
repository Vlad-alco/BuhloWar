#include "SensorManager.h"
#include "SDLogger.h"
#include "SimSensors.h"

SensorManager* SensorManager::instance = nullptr;

SensorManager* SensorManager::getInstance() {
    if (!instance) instance = new SensorManager();
    return instance;
}

void SensorManager::SensorData::addToFilter(float temp) {
    temperatureBuffer[bufferIndex] = temp;
    bufferIndex = (bufferIndex + 1) % MOVING_AVERAGE_WINDOW;
    if (!bufferFilled && bufferIndex == 0) bufferFilled = true;
    float sum = 0;
    int count = bufferFilled ? MOVING_AVERAGE_WINDOW : bufferIndex;
    for (int i = 0; i < count; i++) sum += temperatureBuffer[i];
    filteredTemperature = (count > 0) ? (sum / count) : temp;
}

// === Этап 1: полный сброс фильтров после потери датчика ===
// Очищает среднее, медиану и серию отбросов. Без сброса среднего после
// переподключения фильтр несколько циклов усреднял бы старые температуры
// (например 92C) с новыми (20C) — показания ползли бы с ~77C несколько
// секунд. resetFilter даёт фильтру собраться заново с чистого листа.
void SensorManager::SensorData::resetFilter() {
    bufferIndex = 0;
    bufferFilled = false;
    memset(temperatureBuffer, 0, sizeof(temperatureBuffer));
    filteredTemperature = -127.0f;
    medianIndex = 0;
    medianFilled = false;
    memset(medianBuffer, 0, sizeof(medianBuffer));
    consecutiveRejected = 0;
}

// === Этап 1: сигнальные гарды одного сырого отсчёта ===
// Пайплайн: сырой DS18B20 -> медиана-5 (анти-выброс) -> гейт правдоподобия
// -> кандидат для скользящего среднего. Возврат NAN = отсчёт отброшен,
// среднее не трогаем (фильтр держит последнее достоверное значение).
// Параллельно: stale-детектор и счётчики для веба.
float SensorManager::applySignalGuards(int index, float rawTemp, unsigned long now) {
    static const char* const SLOT_NAMES[SENSOR_COUNT] = {"TSA", "AQUA", "TSAR", "TANK"};
    SensorData& sd = sensorData[index];

    // --- 1) МЕДИАНА-5: сырой отсчёт в кольцевой буфер, берём середину окна ---
    // Одиночный выброс исчезает полностью: он не может оказаться в средней
    // позиции отсортированного окна из 5 значений при 4 корректных соседях.
    sd.medianBuffer[sd.medianIndex] = rawTemp;
    sd.medianIndex = (sd.medianIndex + 1) % MOVING_AVERAGE_WINDOW;
    if (!sd.medianFilled && sd.medianIndex == 0) sd.medianFilled = true;
    float window[MOVING_AVERAGE_WINDOW];
    int n = sd.medianFilled ? MOVING_AVERAGE_WINDOW : sd.medianIndex;
    if (n < 1) n = 1;  // первый отсчёт после сброса
    for (int i = 0; i < n; i++) window[i] = sd.medianBuffer[i];
    // Сортировка вставками: n <= 5, стоимость копеечная даже на ядре 1
    for (int i = 1; i < n; i++) {
        float key = window[i];
        int j = i - 1;
        while (j >= 0 && window[j] > key) { window[j + 1] = window[j]; j--; }
        window[j + 1] = key;
    }
    float candidate = window[n / 2];

    // --- 2) ГЕЙТ ПРАВДОПОДОБИЯ ---
    // Работает только когда фильтр уже валиден: после обрыва filtered=-127,
    // и первый же отсчёт после восстановления должен пройти без ограничений.
    if (sd.filteredTemperature > -120.0f) {
        float jump = candidate - sd.filteredTemperature;
        if (jump < 0.0f) jump = -jump;
        if (jump > SIGNAL_MAX_JUMP_C) {
            sd.rejectedTotal++;
            sd.consecutiveRejected++;
            if (sd.consecutiveRejected == SIGNAL_REJECT_LOG_N) {
                // Одно событие на серию из N подряд — не спамим SD-лог
                String msg = "SIGNAL: ";
                msg += SLOT_NAMES[index];
                msg += ": ";
                msg += String(SIGNAL_REJECT_LOG_N);
                msg += " недостоверных отсчётов подряд (последний ";
                msg += String(rawTemp, 2);
                msg += "C, фильтр ";
                msg += String(sd.filteredTemperature, 2);
                msg += "C) — проверьте датчик/проводку";
                logger.log(msg);
            }
            return NAN;  // среднее не трогаем
        }
    }
    // Отсчёт принят — закрываем серию отбросов (лог восстановления)
    if (sd.consecutiveRejected >= SIGNAL_REJECT_LOG_N) {
        String msg = "SIGNAL: ";
        msg += SLOT_NAMES[index];
        msg += ": сигнал восстановлен после ";
        msg += String(sd.consecutiveRejected);
        msg += " отбросов (";
        msg += String(rawTemp, 2);
        msg += "C)";
        logger.log(msg);
    }
    sd.consecutiveRejected = 0;

    // --- 3) ДЕТЕКТОР ПОДВИСАНИЯ (stale) ---
    // Бит-в-бит одинаковые сырые значения дольше STALE_UNCHANGED_MS —
    // признак возврата старого буфера без реальной конвертации. Гейт
    // сюда не пускает мусор: frozen-датчик проходит гейт (кандидат равен
    // фильтру), поэтому детектор стоит ПОСЛЕ гейта.
    if (rawTemp == sd.lastRawTemp) {
        if (!sd.staleLogged && sd.lastRawChangeMs != 0 &&
            (now - sd.lastRawChangeMs) >= STALE_UNCHANGED_MS) {
            sd.staleLogged = true;
            sd.staleEpisodes++;
            sd.staleStartMs = sd.lastRawChangeMs;
            String msg = "SIGNAL: ";
            msg += SLOT_NAMES[index];
            msg += ": сырое значение не меняется ";
            msg += String((now - sd.lastRawChangeMs) / 1000);
            msg += "с (";
            msg += String(rawTemp, 2);
            msg += "C) — возможное подвисание 1-Wire";
            logger.log(msg);
        }
    } else {
        if (sd.staleLogged) {
            String msg = "SIGNAL: ";
            msg += SLOT_NAMES[index];
            msg += ": подвисание завершилось (";
            msg += String((now - sd.staleStartMs) / 1000);
            msg += "с без изменений), новое значение ";
            msg += String(rawTemp, 2);
            msg += "C";
            logger.log(msg);
            sd.staleLogged = false;
        }
        sd.lastRawTemp = rawTemp;
        sd.lastRawChangeMs = now;
    }

    return candidate;
}

void SensorManager::begin() {
    sensors.begin();
    sensors.setWaitForConversion(false);
    loadAddressesFromConfig();
    sensorsInitialized = true;
    Serial.println("SensorManager initialized");
}

void SensorManager::update() {
    if (!sensorsInitialized) return;
    
    // === Этап 2: режим SIM — синтетические датчики вместо 1-Wire ===
    // Шина не трогается вовсе (ни requestTemperatures, ни чтения): сырые
    // отсчёты берутся из SimSensors и проходят ТОТ ЖЕ пайплайн (медиана-5
    // -> гейт -> stale -> среднее), что и реальные — все гарды Этапа 1 и
    // счётчики проверяются виртуально, включая SD-лог событий SIGNAL.
    // Темп сохранён штатный (UPDATE_INTERVAL), чтобы поведение фильтров
    // совпадало с реальным циклом опроса (~1.6 с).
    if (SimSensors::getInstance()->isActive()) {
        unsigned long simNow = millis();
        if (simNow - lastUpdateTime >= UPDATE_INTERVAL) {
            updateSensorData();
            lastUpdateTime = simNow;
        }
        return;
    }
    
    // === Этап 6: ожидание завершения асинхронного скана калибровки ===
    // Пока идёт конвертация скана (~800 мс), обычный опрос приостановлен:
    // его requestTemperatures() перезапустил бы нашу конвертацию.
    if (calibScanPending) {
        if (millis() - calibScanRequestTime >= 800) {
            finishCalibScan();  // Конвертация готова — читаем результаты шины
        }
        return;
    }
    
    // Не обновляем обычные данные пока идёт калибровка (чтобы не сбивать тайминги)
    for (int i = 0; i < SENSOR_COUNT; i++) {
        if (sensorData[i].calibrationInProgress) return;
    }
    
    unsigned long currentTime = millis();
    if (currentTime - lastUpdateTime < UPDATE_INTERVAL) return;  
    
    if (!conversionInProgress) {
        sensors.requestTemperatures();
        lastConversionStart = currentTime;
        conversionInProgress = true;
    } else if (currentTime - lastConversionStart >= 800) { 
        updateSensorData(); 
        conversionInProgress = false;
        lastUpdateTime = currentTime;
    }
}

void SensorManager::loadAddressesFromConfig() {
    SystemConfig& cfg = configManager.getConfig();
    uint8_t* addresses[] = { cfg.tsaAddress, cfg.aquaAddress, cfg.tsarAddress, cfg.tankAddress };
    const char* sensorNames[] = {"TSA", "AQUA", "TSAR", "TANK"};
    
    for (int i = 0; i < SENSOR_COUNT; i++) {
        bool hasAddress = false;
        for (int j = 0; j < 8; j++) { if (addresses[i][j] != 0) { hasAddress = true; break; } }
        
        if (hasAddress) {
            memcpy(sensorData[i].address, addresses[i], 8);
            sensorData[i].isCalibrated = true;
            Serial.print("Loaded address for "); Serial.print(sensorNames[i]); Serial.print(": ");
            for (int j = 0; j < 8; j++) { if (addresses[i][j] < 16) Serial.print("0"); Serial.print(addresses[i][j], HEX); if (j < 7) Serial.print(":"); }
            Serial.println();
        } else {
            Serial.print(sensorNames[i]); Serial.println(": no address stored");
        }
    }
}

void SensorManager::updateSensorData() {
    unsigned long now = millis();
    // === Этап 2: в SIM сырые отсчёты — синтетика, шина 1-Wire не опрашивается.
    // Слоты обновляются ВСЕ (даже без калибровки) — виртуальный прогон полного
    // цикла возможен на любом устройстве. isCalibrated не трогается: меню
    // калибровки остаются честными к реальному железу.
    SimSensors* sim = SimSensors::getInstance();
    bool simActive = sim->isActive();
    for (int i = 0; i < SENSOR_COUNT; i++) {
        if (!simActive && !sensorData[i].isCalibrated) continue;
        float rawTemp;
        if (simActive) {
            rawTemp = sim->rawFor((uint8_t)i, now);
        } else {
            rawTemp = sensors.getTempC(sensorData[i].address);
        }
        if (rawTemp != DEVICE_DISCONNECTED_C && rawTemp > -127.0f) {
            sensorData[i].isConnected = true;
            sensorData[i].lastValidRead = now;
            // === Этап 1: медиана-5 + гейт правдоподобия + stale-детектор ===
            // В среднее попадает уже ОТСЕЯННОЕ значение (медиана окна),
            // а не сырой отсчёт: выброс и мусор больше не размазываются
            // по фильтру. NAN = отсчёт отброшен — среднее не трогаем.
            float candidate = applySignalGuards(i, rawTemp, now);
            if (!isnan(candidate)) {
                sensorData[i].addToFilter(candidate);
            }
        } else {
            sensorData[i].isConnected = false;
            if (now - sensorData[i].lastValidRead > 5000) {
                // Этап 1: сбрасываем ВСЕ фильтры (было: только filtered=-127),
                // чтобы после переподключения среднее не смешивало старые
                // и новые температуры.
                sensorData[i].resetFilter();
            }
        }
    }
}

bool SensorManager::startCalibration(SensorIndex index) {
    if (index >= SENSOR_COUNT) return false;
    
    // 1. Сбрасываем состояние
    sensorData[index].calibrationInProgress = false;
    calibDeviceCount = 0;
    
    // 2. Этап 6: скан шины теперь НЕ блокирует ядро 1 на ~750 мс.
    // Как было: setWaitForConversion(true) + requestTemperatures()
    // останавливали loop() на время конвертации всех датчиков. Теперь
    // запрос асинхронный, а чтение результатов происходит в update()
    // через 800 мс (finishCalibScan). Счётчик устройств берём из
    // последнего сканирования адресов — он не требует конвертации.
    calibDeviceCount = sensors.getDeviceCount();
    if (calibDeviceCount == 0) return false;
    if (calibDeviceCount > MAX_CALIBRATION_DEVICES) calibDeviceCount = MAX_CALIBRATION_DEVICES;

    // Запускаем конвертацию асинхронно и выходим сразу
    sensors.setWaitForConversion(false);
    sensors.requestTemperatures();
    calibScanRequestTime = millis();
    calibScanPending = true;
    calibScanTarget = index;

    Serial.println("[Calibration] Async bus scan started...");
    Serial.print("Calibration started for slot "); Serial.println(index);
    return true;
}

// === Этап 6: чтение результатов скана шины (вызывается из update()) ===
void SensorManager::finishCalibScan() {
    calibScanPending = false;

    // Конвертация завершена — чтение адресов и температур мгновенное
    for (int i = 0; i < calibDeviceCount; i++) {
        sensors.getAddress(calibInitialAddrs[i], i);
        calibInitialTemps[i] = sensors.getTempCByIndex(i);
        
        Serial.print("  Dev "); Serial.print(i); 
        Serial.print(" Addr: ");
        for(int j=0; j<8; j++) { Serial.print(calibInitialAddrs[i][j], HEX); Serial.print(":"); }
        Serial.print(" T: "); Serial.println(calibInitialTemps[i]);
    }

    // Запускаем таймер калибровки (дальше — обычный поток проверки дельты)
    sensorData[calibScanTarget].calibrationInProgress = true;
    sensorData[calibScanTarget].calibrationStartTime = millis();
}

// === НОВЫЙ МЕТОД: Проверка дельты для LCD (без диалогов) ===
bool SensorManager::checkCalibrationDelta(SensorIndex index, DeviceAddress& foundAddr) {
    if (index >= SENSOR_COUNT || !sensorData[index].calibrationInProgress) return false;

    // Таймаут
    if (millis() - sensorData[index].calibrationStartTime > CALIBRATION_TIMEOUT) {
        Serial.println("Calibration timeout");
        sensorData[index].calibrationInProgress = false;
        calibCheckPending = false;
        return false;
    }

    // === Этап 6: неблокирующая проверка дельты ===
    // Как было: каждый вызов делал БЛОКИРУЮЩУЮ конвертацию (~750 мс),
    // замораживая интерфейс на каждую проверку. Теперь: первый вызов
    // запускает конвертацию асинхронно и возвращает false; результаты
    // читаем вызовами позже (опрос идёт раз в секунду — к тому моменту
    // конвертация гарантированно готова).
    if (!calibCheckPending) {
        sensors.setWaitForConversion(false);
        sensors.requestTemperatures();
        calibCheckRequestTime = millis();
        calibCheckPending = true;
        return false;  // Результат будет готов на следующем опросе
    }
    if (millis() - calibCheckRequestTime < 800) {
        return false;  // Конвертация ещё идёт — опрос без блокировки
    }
    calibCheckPending = false;  // Конвертация готова, читаем ниже

    Serial.print("[CalibCheck] ");
    
    float maxDelta = 0;
    int maxDeltaIdx = -1;

    for (int i = 0; i < calibDeviceCount; i++) {
        float currentTemp = sensors.getTempCByIndex(i);
        float delta = currentTemp - calibInitialTemps[i];
        
        Serial.print("D"); Serial.print(i); Serial.print("="); Serial.print(delta, 1); Serial.print(" ");

        if (delta > CALIBRATION_TEMP_INCREASE) {
            // Нашли нагретый. Проверим, не занят ли он уже другим слотом
            bool alreadyUsed = false;
            for (int j = 0; j < SENSOR_COUNT; j++) {
                if (j != index && sensorData[j].isCalibrated && 
                    memcmp(sensorData[j].address, calibInitialAddrs[i], 8) == 0) {
                    alreadyUsed = true;
                    break;
                }
            }
            
            if (!alreadyUsed && delta > maxDelta) {
                maxDelta = delta;
                maxDeltaIdx = i;
            }
        }
    }
    Serial.println();

    if (maxDeltaIdx != -1) {
        // НАЙДЕН!
        memcpy(foundAddr, calibInitialAddrs[maxDeltaIdx], 8);
        
        // Сохраняем сразу в слот
        memcpy(sensorData[index].address, foundAddr, 8);
        sensorData[index].isCalibrated = true;
        sensorData[index].calibrationInProgress = false;
        
        // И сохраняем в EEPROM
        saveAddressToConfig(index);
        
        Serial.print("FOUND! Slot "); Serial.print(index); 
        Serial.print(" Delta: "); Serial.println(maxDelta);
        return true;
    }

    return false;
}

// Старый метод для Web (оставляем как есть или адаптируем)
bool SensorManager::checkCalibration(SensorIndex index, DeviceAddress& foundAddr) {
    // Для Web используем ту же логику, что и для LCD
    return checkCalibrationDelta(index, foundAddr);
}

void SensorManager::cancelCalibration(SensorIndex index) {
    if (index < SENSOR_COUNT) {
        sensorData[index].calibrationInProgress = false;
        // Этап 6: отменяем и незавершённый скан шины, если он был запущен
        if (calibScanPending && index == calibScanTarget) {
            calibScanPending = false;
            calibCheckPending = false;
        }
        calibDeviceCount = 0;
    }
}

void SensorManager::confirmCalibrationSave(SensorIndex index) {
    // В новой логике сохранение происходит сразу в checkCalibrationDelta
    // Оставим этот метод пустым или для совместимости вызовем saveAddressToConfig
    saveAddressToConfig(index);
}

void SensorManager::saveAddressToConfig(SensorIndex index) {
    if (index >= SENSOR_COUNT || !sensorData[index].isCalibrated) return;
    
    SystemConfig& cfg = configManager.getConfig();
    uint8_t* targetAddr = nullptr;
    int eepromAddr = 0;
    
    switch(index) {
        case SENSOR_TSA: targetAddr = cfg.tsaAddress; eepromAddr = ADDR_TSA_ADDRESS; break;
        case SENSOR_AQUA: targetAddr = cfg.aquaAddress; eepromAddr = ADDR_AQUA_ADDRESS; break;
        case SENSOR_TSAR: targetAddr = cfg.tsarAddress; eepromAddr = ADDR_TSAR_ADDRESS; break;
        case SENSOR_TANK: targetAddr = cfg.tankAddress; eepromAddr = ADDR_TANK_ADDRESS; break;
        default: return;
    }
    
    memcpy(targetAddr, sensorData[index].address, 8);
    configManager.saveSensorAddress(eepromAddr, sensorData[index].address);
    configManager.saveConfig();
    
    Serial.print("Saved address for sensor "); Serial.print(index); Serial.println(" to EEPROM");
}

// Геттеры
float SensorManager::getTemperature(SensorIndex index) { return (index < SENSOR_COUNT) ? sensorData[index].filteredTemperature : -127.0f; }
bool SensorManager::isConnected(SensorIndex index) { return (index < SENSOR_COUNT) ? sensorData[index].isConnected : false; }

// === Этап 1: счётчики качества сигнала для /api/status ===
unsigned long SensorManager::getRejectedTotal(SensorIndex index) { return (index < SENSOR_COUNT) ? sensorData[index].rejectedTotal : 0; }
unsigned long SensorManager::getStaleEpisodes(SensorIndex index) { return (index < SENSOR_COUNT) ? sensorData[index].staleEpisodes : 0; }
float SensorManager::getTSA() { return getTemperature(SENSOR_TSA); }
float SensorManager::getAQUA() { return getTemperature(SENSOR_AQUA); }
float SensorManager::getTSAR() { return getTemperature(SENSOR_TSAR); }
float SensorManager::getTANK() { return getTemperature(SENSOR_TANK); }
bool SensorManager::isTSAConnected() { return isConnected(SENSOR_TSA); }
bool SensorManager::isAQUAConnected() { return isConnected(SENSOR_AQUA); }
bool SensorManager::isTSARConnected() { return isConnected(SENSOR_TSAR); }
bool SensorManager::isTANKConnected() { return isConnected(SENSOR_TANK); }
bool SensorManager::isCalibrated(SensorIndex index) { return (index < SENSOR_COUNT) ? sensorData[index].isCalibrated : false; }
// Этап 6: во время асинхронного скана шины слот уже «калибруется» для меню —
// иначе меню показало бы NOT FND раньше, чем скан завершится
bool SensorManager::isCalibrating(SensorIndex index) {
    if (index >= SENSOR_COUNT) return false;
    if (calibScanPending && index == calibScanTarget) return true;
    return sensorData[index].calibrationInProgress;
}
float SensorManager::getCalibrationInitialTemp(SensorIndex index) { return (index < SENSOR_COUNT) ? sensorData[index].calibrationInitialTemp : -127.0f; }
unsigned long SensorManager::getCalibrationElapsed(SensorIndex index) { if (index >= SENSOR_COUNT || !sensorData[index].calibrationInProgress) return 0; return millis() - sensorData[index].calibrationStartTime; }
float SensorManager::getCurrentRawTemperature() { sensors.requestTemperatures(); if (sensors.getDeviceCount() > 0) return sensors.getTempCByIndex(0); return -127.0f; }
int SensorManager::getDiscoveredCount() { return sensors.getDeviceCount(); }
bool SensorManager::getDiscoveredAddress(int deviceIndex, DeviceAddress& addr) { if (deviceIndex < sensors.getDeviceCount()) return sensors.getAddress(addr, deviceIndex); return false; }
void SensorManager::resetCalibration(SensorIndex index) { if (index < SENSOR_COUNT) { sensorData[index].isCalibrated = false; memset(sensorData[index].address, 0, 8); } }