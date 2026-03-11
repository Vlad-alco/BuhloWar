#ifndef SENSOR_ADAPTER_H
#define SENSOR_ADAPTER_H

#include <Adafruit_BME280.h>
#include <Wire.h>
#include "config.h"
#include "common.h"
#include "SensorManager.h"  // Добавляем

// Константы интервалов обновления
#define DS18B20_UPDATE_INTERVAL 1000    // 1 секунда
#define BME280_UPDATE_INTERVAL  2000    // 2 секунды

class SensorAdapter {
private:
    // BME280
    Adafruit_BME280 bme;
    bool bmeInitialized = false;
    
    // DS18B20 менеджер
    SensorManager* dsManager = nullptr;
    
    // Текущие данные
    SensorData currentData;
    
    // Время последнего обновления
    unsigned long lastDS18B20Update = 0;
    unsigned long lastBME280Update = 0;
    
    // Обновление данных
    void updateDS18B20Data();
    void updateBME280Data();
    void updateCurrentDataStruct();
    SensorStatus convertDsStatus(SensorIndex index) const;
    
    // Утилиты
    String sensorStatusToString(SensorStatus status) const;
    
public:
    // Конструктор/деструктор
    SensorAdapter() = default;
    ~SensorAdapter() = default;
    
    // Инициализация
    bool begin(SensorManager* dsManager, TwoWire* wire = &Wire);
    
    // Основной метод обновления
    void update();
    
    // Получение данных
    const SensorData& getData() const { return currentData; }
    
    // Статус системы
    bool isAnySensorFailed() const;
    String getSensorStatusString() const;
    
    // Методы для отдельных датчиков (не const!)
    float getBME280Temperature();
    float getBME280Pressure();
    float getBME280Humidity();
    float getDS18B20Temperature(SensorIndex index);
    
    // Статус датчиков
    bool isBME280Available() const { return bmeInitialized; }
    bool isDS18B20Available() const { return dsManager != nullptr; }
    
    // Утилиты расчетов
    float calculateDewPoint(float temp, float humidity) const;
    float calculateHeatIndex(float temp, float humidity) const;
    float pressureToAltitude(float pressure, float seaLevelPressure = 1013.25) const;
};

#endif