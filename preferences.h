#ifndef PREFERENCES_H
#define PREFERENCES_H

#include <Arduino.h>
#include <EEPROM.h>  // Стандартная EEPROM
#include "common.h"

// Адреса в EEPROM для каждой переменной
#define EEPROM_SIZE 512  // Размер EEPROM

enum EEPROMAddress {
  ADDR_HEATER_TYPE = 0,
  ADDR_POWER = 4,
  ADDR_AS_VOLUME = 8,
  ADDR_TSA_LIMIT = 12,
  ADDR_HISTERESIS = 16,
  ADDR_CORRELATION = 20,
  ADDR_DELTA = 24,
  ADDR_RAZGON_TEMP = 28,
  ADDR_CYCLE_LIM = 32,
  ADDR_USE_HEAD_VALVE = 36,
  ADDR_HEADS_TYPE_KSS = 40,
  ADDR_BODY_VALVE_NC = 44,
  ADDR_BAK_STOP_TEMP = 48,
  ADDR_MIXER_ON_TIME = 52,
  ADDR_MIXER_OFF_TIME = 56,
  ADDR_MIXER_ENABLED = 60,
  ADDR_HEAD_OPEN_MS = 64,
  ADDR_HEAD_CLOSE_MS = 68,
  ADDR_BODY_OPEN_MS = 72,
  ADDR_BODY_CLOSE_MS = 76,
  
  ADDR_EMERGENCY_TIME = 96,
  ADDR_NASEB_TIME = 100,
  ADDR_REKLAP_TIME = 104,
  ADDR_BOX_MAX_TEMP = 108,
  ADDR_ACTIVE_PROCESS = 112,
  ADDR_PROCESS_RUNNING = 116,
  ADDR_PROCESS_START_TIME = 120,
  ADDR_VALVE_CAL = 124, // Удалено, но адрес зарезервирован для совместимости
  ADDR_SYNC_IN_PROGRESS = 140,
  
  // Адреса датчиков
  ADDR_TSA_ADDRESS = 144,    // 8 байт (144-151)
  ADDR_AQUA_ADDRESS = 152,   // 8 байт (152-159)  
  ADDR_TSAR_ADDRESS = 160,   // 8 байт (160-167)
  ADDR_TANK_ADDRESS = 168,   // 8 байт (168-175)

  // --- НОВЫЕ АДРЕСА (вместо ZATOR/AUTOCLAVE) ---
  ADDR_CHEKWIFI = 176,       // Интервал проверки WiFi (мин)
  ADDR_VALVE_USE = 180,      // Использование клапана при DIST (bool)
  ADDR_MIDTERM = 184,        // Температура смены посуды (DIST)
  ADDR_CALIBRATION = 188,    // Флаг калибровки клапанов (bool)
  ADDR_ACTIVE_TEST = 192,    // Время теста клапана (сек)
  
  // Пропускные способности клапанов
  ADDR_VALVE_HEAD_CAP = 196,    // HCAP (мл/мин)
  ADDR_VALVE_BODY_CAP = 200,    // BCAP (мл/мин)
  ADDR_VALVE0_BODY_CAP = 204,   // B0CAP (мл/мин) для НО клапана
  ADDR_FULL_PWR = 208,
  ADDR_MIDTERM_ABV = 212   // Крепость смены тары (%)     
};

// Структура для хранения всех переменных
struct SystemConfig {
  // Основные настройки
  int heaterType = 0;
  int power = 3500;
  int asVolume = 5000;
  int tsaLimit = 40;
  float histeresis = 0.24f;
  float correlation = 1.13f;
  float delta = 0.06f;
  
  // Настройки дистилляции/ректификации
  int razgonTemp = 78;
  int cycleLim = 3;
  bool useHeadValve = true;
  bool headsTypeKSS = false;
  bool bodyValveNC = true;
  int bakStopTemp = 99;
  
  // Настройки миксера
  int mixerOnTime = 30;
  int mixerOffTime = 300;
  bool mixerEnabled = true;
  
  // --- Калибровка клапанов (единицы измерения: СЕКУНДЫ) ---
  int headOpenMs = 1;           // По ТЗ: 1 сек
  int headCloseMs = 10;         // По ТЗ: 10 сек
  int bodyOpenMs = 2;           // По ТЗ: 2 сек
  int bodyCloseMs = 10;         // По ТЗ: 10 сек

  // Адреса датчиков температуры (по 8 байт каждый)
  uint8_t tsaAddress[8] = {0};
  uint8_t aquaAddress[8] = {0};
  uint8_t tsarAddress[8] = {0};
  uint8_t tankAddress[8] = {0};
  
  // Безопасность
  int emergencyTime = 3;  // VREAC
  int nasebTime = 10;
  int reklapTime = 10;
  int boxMaxTemp = 60;
  
  // --- НОВЫЕ ПЕРЕМЕННЫЕ ---
  int chekwifi = 5;             // Интервал проверки WiFi (мин)
  bool valveuse = false;        // Использовать клапан при DIST
  int midterm = 92;             // Температура смены посуды
  int midterm_abv = 0; 
  bool calibration = true;      // Калибровка клапанов (флаг запуска этапа)
  int active_test = 60;         // Время теста клапана (сек)
  int timezoneOffset = 3;
  
  // Пропускные способности
  int valve_head_capacity = 100; // HCAP
  int valve_body_capacity = 100; // BCAP
  int valve0_body_capacity = 100; // B0CAP
  // Внутри struct SystemConfig, после int active_test = 60;
  bool fullPwr = true; // Использовать полную мощность при разгоне (DIST)
  // Служебные
  unsigned long localChangeTimestamp = 0;
  unsigned long webChangeTimestamp = 0;
  bool configChangedLocally = false;
  bool syncInProgress = false;
  
  // Текущий активный процесс
  ProcessType activeProcess = PROCESS_NONE;
  bool processRunning = false;
  unsigned long processStartTime = 0;
};

class ConfigManager {
private:
  SystemConfig currentConfig;
  
public:
  ConfigManager() {}
  
  void begin();
  void loadConfig();
  void saveConfig();
  
  SystemConfig& getConfig();
  void setConfig(const SystemConfig& newConfig);
  
  bool startProcess(ProcessType process);
  void stopProcess();
  
  ProcessType getActiveProcess();
  bool isProcessRunning();
  bool isDistProcessRunning();
  bool isRectProcessRunning();
  
  // Отдельные методы для сохранения
  void saveDistConfig();
  void saveRectConfig();

  // Методы для работы с адресами датчиков
  void saveSensorAddress(int address, uint8_t* addr);
  void loadSensorAddress(int address, uint8_t* addr);
  
  // Функция расчета крепости
  float getABV(float temp, float pressure_mmHg, bool isOutput);
   // === НОВОЕ: Расчет температуры для смены тары ===
    // targetAbv - желаемая крепость в струе (например, 43.0)
    // pressure_mmHg - текущее давление в мм рт.ст.
    // Возвращает температуру куба, при которой нужно менять тару
    float getTempForOutputABV(float targetAbv, float pressure_mmHg);
    // Расчет крепости в струе по температуре куба
    float getOutputABVForTemp(float temp, float pressure_mmHg);
    // ================================================

private:
  // Вспомогательные методы для работы с EEPROM
  void writeInt(int address, int value);
  int readInt(int address, int defaultValue);
  void writeFloat(int address, float value);
  float readFloat(int address, float defaultValue);
  void writeBool(int address, bool value);
  bool readBool(int address, bool defaultValue);
  void writeULong(int address, unsigned long value);
  unsigned long readULong(int address, unsigned long defaultValue);
};

extern ConfigManager configManager;

#endif