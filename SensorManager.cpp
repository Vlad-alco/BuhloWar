#include "SensorManager.h"
#include "SDLogger.h"

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

void SensorManager::begin() {
    sensors.begin();
    sensors.setWaitForConversion(false);
    loadAddressesFromConfig();
    sensorsInitialized = true;
    Serial.println("SensorManager initialized");
}

void SensorManager::update() {
    if (!sensorsInitialized) return;
    
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
    for (int i = 0; i < SENSOR_COUNT; i++) {
        if (!sensorData[i].isCalibrated) continue;
        float rawTemp = sensors.getTempC(sensorData[i].address);
        if (rawTemp != DEVICE_DISCONNECTED_C && rawTemp > -127.0f) {
            sensorData[i].addToFilter(rawTemp);
            sensorData[i].isConnected = true;
            sensorData[i].lastValidRead = millis();
        } else {
            sensorData[i].isConnected = false;
            if (millis() - sensorData[i].lastValidRead > 5000) sensorData[i].filteredTemperature = -127.0f;
        }
    }
}

bool SensorManager::startCalibration(SensorIndex index) {
    if (index >= SENSOR_COUNT) return false;
    
    // 1. Сбрасываем состояние
    sensorData[index].calibrationInProgress = false;
    calibDeviceCount = 0;
    
    // 2. Сканируем всю шину (блокирующе, так как это разовая операция)
    sensors.setWaitForConversion(true);
    sensors.requestTemperatures();
    sensors.setWaitForConversion(false);
    
    // 3. Запоминаем температуры ВСЕХ найденных устройств
    calibDeviceCount = sensors.getDeviceCount();
    if (calibDeviceCount == 0) return false;
    if (calibDeviceCount > MAX_CALIBRATION_DEVICES) calibDeviceCount = MAX_CALIBRATION_DEVICES;

    Serial.println("[Calibration] Scanning bus...");
    for (int i = 0; i < calibDeviceCount; i++) {
        sensors.getAddress(calibInitialAddrs[i], i);
        calibInitialTemps[i] = sensors.getTempCByIndex(i);
        
        Serial.print("  Dev "); Serial.print(i); 
        Serial.print(" Addr: ");
        for(int j=0; j<8; j++) { Serial.print(calibInitialAddrs[i][j], HEX); Serial.print(":"); }
        Serial.print(" T: "); Serial.println(calibInitialTemps[i]);
    }

    // 4. Запускаем таймер
    sensorData[index].calibrationInProgress = true;
    sensorData[index].calibrationStartTime = millis();
    
    Serial.print("Calibration started for slot "); Serial.println(index);
    return true;
}

// === НОВЫЙ МЕТОД: Проверка дельты для LCD (без диалогов) ===
bool SensorManager::checkCalibrationDelta(SensorIndex index, DeviceAddress& foundAddr) {
    if (index >= SENSOR_COUNT || !sensorData[index].calibrationInProgress) return false;

    // Таймаут
    if (millis() - sensorData[index].calibrationStartTime > CALIBRATION_TIMEOUT) {
        Serial.println("Calibration timeout");
        sensorData[index].calibrationInProgress = false;
        return false;
    }

    // Сканируем текущие температуры
    sensors.setWaitForConversion(true);
    sensors.requestTemperatures();
    sensors.setWaitForConversion(false);

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
float SensorManager::getTSA() { return getTemperature(SENSOR_TSA); }
float SensorManager::getAQUA() { return getTemperature(SENSOR_AQUA); }
float SensorManager::getTSAR() { return getTemperature(SENSOR_TSAR); }
float SensorManager::getTANK() { return getTemperature(SENSOR_TANK); }
bool SensorManager::isTSAConnected() { return isConnected(SENSOR_TSA); }
bool SensorManager::isAQUAConnected() { return isConnected(SENSOR_AQUA); }
bool SensorManager::isTSARConnected() { return isConnected(SENSOR_TSAR); }
bool SensorManager::isTANKConnected() { return isConnected(SENSOR_TANK); }
bool SensorManager::isCalibrated(SensorIndex index) { return (index < SENSOR_COUNT) ? sensorData[index].isCalibrated : false; }
bool SensorManager::isCalibrating(SensorIndex index) { return (index < SENSOR_COUNT) ? sensorData[index].calibrationInProgress : false; }
float SensorManager::getCalibrationInitialTemp(SensorIndex index) { return (index < SENSOR_COUNT) ? sensorData[index].calibrationInitialTemp : -127.0f; }
unsigned long SensorManager::getCalibrationElapsed(SensorIndex index) { if (index >= SENSOR_COUNT || !sensorData[index].calibrationInProgress) return 0; return millis() - sensorData[index].calibrationStartTime; }
float SensorManager::getCurrentRawTemperature() { sensors.requestTemperatures(); if (sensors.getDeviceCount() > 0) return sensors.getTempCByIndex(0); return -127.0f; }
int SensorManager::getDiscoveredCount() { return sensors.getDeviceCount(); }
bool SensorManager::getDiscoveredAddress(int deviceIndex, DeviceAddress& addr) { if (deviceIndex < sensors.getDeviceCount()) return sensors.getAddress(addr, deviceIndex); return false; }
void SensorManager::resetCalibration(SensorIndex index) { if (index < SENSOR_COUNT) { sensorData[index].isCalibrated = false; memset(sensorData[index].address, 0, 8); } }