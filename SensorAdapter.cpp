#include "SensorAdapter.h"
#include "config.h"
#include "common.h"
#include <Arduino.h>
#include "SDLogger.h" // Подключаем заголовок логера

extern SDLogger logger; // Объявляем, что переменная logger существует глобально где-то в другом месте

// ==================== ИНИЦИАЛИЗАЦИЯ ====================

bool SensorAdapter::begin(SensorManager* dsManager, TwoWire* wire) {
    if (!dsManager) {
        Serial.println("[SensorAdapter] ERROR: DS18B20 manager is null!");
        logger.log("[SensorAdapter] ERROR: DS18B20 manager is null!");
        return false;
    }
    
    this->dsManager = dsManager;
    
    // DS18B20 manager уже должен быть инициализирован
    Serial.println("[SensorAdapter] DS18B20 manager initialized");
    logger.log("[SensorAdapter] DS18B20 manager initialized");
    
    // Инициализация BME280
    bmeInitialized = bme.begin(BME280_ADDRESS_ALTERNATE, wire);
    
    if (!bmeInitialized) {
        Serial.println("[SensorAdapter] ERROR: Could not find BME280 sensor!");
        logger.log("[SensorAdapter] ERROR: Could not find BME280 sensor!");
        
        bmeInitialized = false;
    } else {
        // Настройка BME280
        bme.setSampling(
            Adafruit_BME280::MODE_NORMAL,
            Adafruit_BME280::SAMPLING_X1,
            Adafruit_BME280::SAMPLING_X1,
            Adafruit_BME280::SAMPLING_X1,
            Adafruit_BME280::FILTER_OFF,
            Adafruit_BME280::STANDBY_MS_1000
        );
        Serial.println("[SensorAdapter] BME280 initialized successfully");
        logger.log("[SensorAdapter] BME280 initialized successfully");
    }
    
    // Инициализация структуры данных
    currentData.tsa.name = "TSA";
    currentData.aqua.name = "AQUA";
    currentData.tsar.name = "TSAR";
    currentData.tank.name = "TANK";
    
    // Начальные значения
    lastDS18B20Update = 0;
    lastBME280Update = 0;
    
    // Инициализируем значения
    currentData.tsa.value = -127.0f;
    currentData.aqua.value = -127.0f;
    currentData.tsar.value = -127.0f;
    currentData.tank.value = -127.0f;
    currentData.boxTemp = -127.0f;
    currentData.pressure = 0.0f;
    currentData.humidity = 0.0f;
    currentData.bmeStatus = SensorStatus::NOT_FOUND;
    
    Serial.println("[SensorAdapter] Initialization complete");
    Serial.print("  - DS18B20 sensors found: ");
    logger.log("[SensorAdapter] Initialization complete");
    
    Serial.println(dsManager->getDiscoveredCount());
    Serial.print("  - BME280: ");
    Serial.println(bmeInitialized ? "OK" : "NOT FOUND");
    
    return true;
}

// ==================== ОСНОВНОЙ ЦИКЛ ====================

void SensorAdapter::update() {
    unsigned long currentTime = millis();
    
    // Обновление DS18B20 (каждую секунду)
    if (currentTime - lastDS18B20Update >= DS18B20_UPDATE_INTERVAL) {
        updateDS18B20Data();
        lastDS18B20Update = currentTime;
    }
    
    // Обновление BME280 (каждые 2 секунды)
    if (bmeInitialized && (currentTime - lastBME280Update >= BME280_UPDATE_INTERVAL)) {
        updateBME280Data();
        lastBME280Update = currentTime;
    }
    
    // Обновление общей структуры данных
    updateCurrentDataStruct();
}

// ==================== МЕТОДЫ ЧТЕНИЯ ДАТЧИКОВ ====================

float SensorAdapter::getBME280Temperature() {
    if (!bmeInitialized) return -127.0f;
    float temp = bme.readTemperature();
    return isnan(temp) ? -127.0f : temp;
}

float SensorAdapter::getBME280Pressure() {
    if (!bmeInitialized) return 0.0f;
    float pressure = bme.readPressure();
    return isnan(pressure) ? 0.0f : pressure / 100.0F; // Па -> гПа
}

float SensorAdapter::getBME280Humidity() {
    if (!bmeInitialized) return 0.0f;
    float humidity = bme.readHumidity();
    return isnan(humidity) ? 0.0f : humidity;
}

float SensorAdapter::getDS18B20Temperature(SensorIndex index) {
    if (!dsManager) return -127.0f;
    
    switch (index) {
        case SENSOR_TSA:
            return dsManager->getTSA();
        case SENSOR_AQUA:
            return dsManager->getAQUA();
        case SENSOR_TSAR:
            return dsManager->getTSAR();
        case SENSOR_TANK:
            return dsManager->getTANK();
        default:
            return -127.0f;
    }
}

// ==================== ОБНОВЛЕНИЕ ДАННЫХ ====================

void SensorAdapter::updateDS18B20Data() {
    if (!dsManager) return;
    
    // Обновление данных в менеджере
    dsManager->update();
}

void SensorAdapter::updateBME280Data() {
    if (!bmeInitialized) return;
    
    float temp = bme.readTemperature();
    float pressure = bme.readPressure();
    float humidity = bme.readHumidity();
    
    if (isnan(temp) || isnan(pressure) || isnan(humidity)) {
        Serial.println("[SensorAdapter] BME280 read error!");
        bmeInitialized = false;
        return;
    }
    
    currentData.boxTemp = temp;
    currentData.pressure = pressure / 100.0F; // Па -> гПа
    currentData.humidity = humidity;
}

void SensorAdapter::updateCurrentDataStruct() {
    currentData.timestamp = millis();
    
    if (dsManager) {
        // TSA
        currentData.tsa.value = dsManager->getTSA();
        currentData.tsa.status = convertDsStatus(SENSOR_TSA);
        
        // AQUA
        currentData.aqua.value = dsManager->getAQUA();
        currentData.aqua.status = convertDsStatus(SENSOR_AQUA);
        
        // TSAR
        currentData.tsar.value = dsManager->getTSAR();
        currentData.tsar.status = convertDsStatus(SENSOR_TSAR);
        
        // TANK
        currentData.tank.value = dsManager->getTANK();
        currentData.tank.status = convertDsStatus(SENSOR_TANK);
    } else {
        currentData.tsa.status = SensorStatus::NOT_FOUND;
        currentData.aqua.status = SensorStatus::NOT_FOUND;
        currentData.tsar.status = SensorStatus::NOT_FOUND;
        currentData.tank.status = SensorStatus::NOT_FOUND;
    }
    
    if (bmeInitialized) {
        currentData.bmeStatus = SensorStatus::OK;
    } else {
        currentData.bmeStatus = SensorStatus::NOT_FOUND;
    }
}

SensorStatus SensorAdapter::convertDsStatus(SensorIndex index) const {
    if (!dsManager) return SensorStatus::NOT_FOUND;
    
    bool connected = false;
    switch (index) {
        case SENSOR_TSA: 
            connected = dsManager->isTSAConnected(); 
            break;
        case SENSOR_AQUA: 
            connected = dsManager->isAQUAConnected(); 
            break;
        case SENSOR_TSAR: 
            connected = dsManager->isTSARConnected(); 
            break;
        case SENSOR_TANK: 
            connected = dsManager->isTANKConnected(); 
            break;
        default: 
            return SensorStatus::NOT_FOUND;
    }
    
    if (!connected) {
        return SensorStatus::OFF;
    }
    
    if (!dsManager->isCalibrated(index)) {
        return SensorStatus::NOT_FOUND;
    }
    
    return SensorStatus::OK;
}

// ==================== СТАТУС СИСТЕМЫ ====================

bool SensorAdapter::isAnySensorFailed() const {
    // Проверка DS18B20
    bool dsFailed = (currentData.tsa.status == SensorStatus::OFF ||
                    currentData.aqua.status == SensorStatus::OFF ||
                    currentData.tsar.status == SensorStatus::OFF ||
                    currentData.tank.status == SensorStatus::OFF);
    
    // Проверка BME280
    bool bmeFailed = (currentData.bmeStatus == SensorStatus::OFF);
    
    return dsFailed || bmeFailed;
}

String SensorAdapter::getSensorStatusString() const {
    String status = "Sensors: ";
    
    status += "DS[";
    status += sensorStatusToString(currentData.tsa.status) + " ";
    status += sensorStatusToString(currentData.aqua.status) + " ";
    status += sensorStatusToString(currentData.tsar.status) + " ";
    status += sensorStatusToString(currentData.tank.status);
    status += "] ";
    
    status += "BME[";
    status += sensorStatusToString(currentData.bmeStatus);
    status += "]";
    
    return status;
}

String SensorAdapter::sensorStatusToString(SensorStatus status) const {
    switch (status) {
        case SensorStatus::OK: return "OK";
        case SensorStatus::OFF: return "OFF";
        case SensorStatus::NOT_FOUND: return "NF";
        case SensorStatus::N_USE: return "NU";
        default: return "??";
    }
}

// ==================== УТИЛИТЫ ====================

float SensorAdapter::calculateDewPoint(float temp, float humidity) const {
    if (temp < 0.1 || humidity < 0.1) return 0.0;
    
    float a = 17.27;
    float b = 237.7;
    float gamma = (a * temp) / (b + temp) + log(humidity / 100.0);
    return (b * gamma) / (a - gamma);
}

float SensorAdapter::calculateHeatIndex(float temp, float humidity) const {
    if (temp < 20.0) return temp;
    
    float c1 = -8.78469475556;
    float c2 = 1.61139411;
    float c3 = 2.33854883889;
    float c4 = -0.14611605;
    float c5 = -0.012308094;
    float c6 = -0.0164248277778;
    float c7 = 0.002211732;
    float c8 = 0.00072546;
    float c9 = -0.000003582;
    
    float T = temp;
    float R = humidity;
    
    return c1 + c2*T + c3*R + c4*T*R + c5*T*T + c6*R*R + c7*T*T*R + c8*T*R*R + c9*T*T*R*R;
}

float SensorAdapter::pressureToAltitude(float pressure, float seaLevelPressure) const {
    if (pressure <= 0) return 0.0;
    
    return 44330.0 * (1.0 - pow(pressure / seaLevelPressure, 0.1903));
}