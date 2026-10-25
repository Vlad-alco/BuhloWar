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

// === Этап 1 (сигнальные гарды): защита от скачков и подвисаний показаний ===
// SIGNAL_MAX_JUMP_C — максимально правдоподобный скачок медианы за один
// цикл опроса (~1.6 с). Куб с брагой и колонка физически не меняют
// температуру быстрее ~0.3 C/с — запас более чем 5-кратный. Всё, что
// больше, — сбой датчика/шины, а не реальная температура.
#define SIGNAL_MAX_JUMP_C      3.0f
// Серия НЕдостоверных отсчётов подряд, после которой пишем одно событие
// в лог (не каждый отсчёт — чтобы не заспамить SD-лог и не грузить SPI).
#define SIGNAL_REJECT_LOG_N    5
// «Подвисание» (stale): сырое значение 1-Wire не изменилось бит-в-бит
// дольше этого времени. При живой шине шум обычно даёт мерцание младшего
// бита (0.0625 C) хотя бы раз в минуту; ровное число минутами — признак
// того, что библиотека возвращает старый буфер без конвертации.
#define STALE_UNCHANGED_MS     60000UL

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

        // === Этап 1: буфер медианы-5 (анти-выброс) ===
        // Пайплайн отсчёта: сырой DS18B20 -> медиана-5 (одиночный выброс
        // исчезает целиком, а не размазывается, как в среднем) -> гейт
        // правдоподобия -> скользящее среднее (temperatureBuffer выше).
        float medianBuffer[MOVING_AVERAGE_WINDOW];
        int medianIndex = 0;
        bool medianFilled = false;

        // === Этап 1: детектор подвисания (stale) ===
        // Сырое значение сравнивается бит-в-бит: живой датчик хотя бы
        // изредка «дрожит» на младший бит (0.0625 C) из-за шума.
        float lastRawTemp = -127.0f;          // последнее сырое значение
        unsigned long lastRawChangeMs = 0;    // millis() последнего изменения сырого
        unsigned long staleStartMs = 0;       // начало текущего эпизода (для длительности в логе)
        bool staleLogged = false;             // событие STALE записано (1 раз на эпизод)

        // === Этап 1: счётчики диагностики (в веб через /api/status "sig") ===
        unsigned long rejectedTotal = 0;      // отброшено гейтом правдоподобия — всего
        unsigned long staleEpisodes = 0;      // зафиксировано эпизодов подвисания
        int consecutiveRejected = 0;          // серия отбросов подряд (для батч-лога)
        
        // Для калибровки (индивидуальная логика остается для совместимости с Web)
        float calibrationInitialTemp = -127.0f;
        bool calibrationInProgress = false;
        unsigned long calibrationStartTime = 0;
        
        SensorData() {
            memset(address, 0, 8);
            memset(temperatureBuffer, 0, sizeof(temperatureBuffer));
            memset(medianBuffer, 0, sizeof(medianBuffer));
        }
        void addToFilter(float temp);
        // Полный сброс фильтров (среднее + медиана + серия отбросов).
        // Вызывается при признании датчика отключённым (>5 с без валидных
        // данных): иначе после переподключения среднее несколько циклов
        // усредняло бы СТАРЫЕ температуры с новыми — показания «сходили
        // бы с ума» ~5-6 секунд после каждого обрыва шины.
        void resetFilter();
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

    // === Этап 6: НЕБЛОКИРУЮЩАЯ калибровка (скан шины и проверка дельты) ===
    // Как было: startCalibration() и checkCalibrationDelta() делали блокирующую
    // конвертацию (~750 мс каждый вызов) — ядро 1 замирало, меню и процесс
    // подвисали. Теперь конвертация запускается асинхронно, а результаты
    // читаются в update()/следующем опросе, когда она гарантированно готова.
    bool calibScanPending = false;            // true = ждём окончания конвертации скана шины
    unsigned long calibScanRequestTime = 0;   // millis() запроса конвертации скана
    SensorIndex calibScanTarget = SENSOR_TSA; // какой слот калибруем
    bool calibCheckPending = false;           // true = ждём окончания конвертации проверки дельты
    unsigned long calibCheckRequestTime = 0;  // millis() запроса конвертации дельты
    // =====================================================================
    void finishCalibScan();
    
    void updateSensorData();
    // === Этап 1: сигнальные гарды одного сырого отсчёта ===
    // На входе — сырой DS18B20, на выходе — значение для среднего (медиана
    // окна) или NAN, если гейт отбросил отсчёт. Параллельно ведёт
    // stale-детектор и счётчики диагностики.
    float applySignalGuards(int index, float rawTemp, unsigned long now);
    void loadAddressesFromConfig();
    void saveAddressToConfig(SensorIndex index);
    
    SensorManager() : oneWire(ONE_WIRE_BUS), sensors(&oneWire) {}
    
public:
    static SensorManager* getInstance();
    
    void begin();
    void update();
    
    float getTemperature(SensorIndex index);
    bool isConnected(SensorIndex index);

    // === Этап 1: счётчики качества сигнала (для /api/status и мониторинга) ===
    unsigned long getRejectedTotal(SensorIndex index); // отбросов гейтом — всего
    unsigned long getStaleEpisodes(SensorIndex index); // эпизодов подвисания
    
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