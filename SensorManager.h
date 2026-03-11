#ifndef SENSOR_MANAGER_H
#define SENSOR_MANAGER_H

#include <OneWire.h>
#include <DallasTemperature.h>
#include "config.h"
#include "preferences.h"  
#include "ProcessCommon.h" 

enum SensorIndex {
    SENSOR_TSA = 0,
    SENSOR_AQUA,
    SENSOR_TSAR,
    SENSOR_TANK,
    SENSOR_COUNT
};

#define MOVING_AVERAGE_WINDOW 5
#define CALIBRATION_TEMP_INCREASE 5.0f  // ИЗМЕНЕНО: 5 градусов по вашему ТЗ
#define CALIBRATION_TIMEOUT 30000       

// Максимальное количество устройств на шине для сканирования
#define MAX_CALIBRATION_DEVICES 15

class SensorManager {
private:
    unsigned long lastConversionStart = 0;
    bool conversionInProgress = false;
    static SensorManager* instance;
    OneWire oneWire;
    DallasTemperature sensors;
    
    struct SensorData {
        float temperatureBuffer[MOVING_AVERAGE_WINDOW];
        int bufferIndex = 0;
        bool bufferFilled = false;
        float filteredTemperature = -127.0f;
        bool isConnected = false;
        bool isCalibrated = false;
        DeviceAddress address;
        unsigned long lastValidRead = 0;
        
        // Для калибровки (индивидуальная логика остается для совместимости с Web)
        float calibrationInitialTemp = -127.0f;
        bool calibrationInProgress = false;
        unsigned long calibrationStartTime = 0;
        
        SensorData() { memset(address, 0, 8); memset(temperatureBuffer, 0, sizeof(temperatureBuffer)); }
        void addToFilter(float temp);
    };
    
    SensorData sensorData[SENSOR_COUNT];
    unsigned long lastUpdateTime = 0;
    const unsigned long UPDATE_INTERVAL = 1000; 
    bool sensorsInitialized = false;

    // === НОВОЕ: Данные для сканирования всей шины при калибровке ===
    // Запоминаем начальную температуру всех найденных устройств
    float calibInitialTemps[MAX_CALIBRATION_DEVICES];
    DeviceAddress calibInitialAddrs[MAX_CALIBRATION_DEVICES];
    int calibDeviceCount = 0;
    // ==============================================================
    
    void updateSensorData();
    void loadAddressesFromConfig();
    void saveAddressToConfig(SensorIndex index);
    
    SensorManager() : oneWire(ONE_WIRE_BUS), sensors(&oneWire) {}
    
public:
    static SensorManager* getInstance();
    
    void begin();
    void update();
    
    float getTemperature(SensorIndex index);
    bool isConnected(SensorIndex index);
    
    float getTSA();
    float getAQUA();
    float getTSAR();
    float getTANK();
    
    bool isTSAConnected();
    bool isAQUAConnected();
    bool isTSARConnected();
    bool isTANKConnected();
    
    // === НОВЫЙ МЕТОД ===
    // Возвращает true если найден нагретый датчик > 5 градусов
    bool checkCalibrationDelta(SensorIndex index, DeviceAddress& foundAddr);
    // ===================
    
    bool startCalibration(SensorIndex index);
    bool checkCalibration(SensorIndex index, DeviceAddress& foundAddr); // Оставим для Web
    void cancelCalibration(SensorIndex index);
    void confirmCalibrationSave(SensorIndex index);
    
    bool isCalibrated(SensorIndex index);
    bool isCalibrating(SensorIndex index);
    float getCalibrationInitialTemp(SensorIndex index);
    unsigned long getCalibrationElapsed(SensorIndex index);
    
    float getCurrentRawTemperature();
    int getDiscoveredCount();
    bool getDiscoveredAddress(int deviceIndex, DeviceAddress& addr);
    
    void resetCalibration(SensorIndex index);
};

#endif