#include "preferences.h"
#include <math.h>

// Определение глобальной переменной
ConfigManager configManager;

// --- Таблица зависимости крепости от температуры (новая точная таблица) ---
const int ABV_TABLE_SIZE = 44;
const float abvTemp[ABV_TABLE_SIZE] = {
  78.5, 79.0, 79.5, 80.0, 80.5, 81.0, 81.5, 82.0, 82.5, 83.0, 
  83.5, 84.0, 84.5, 85.0, 85.5, 86.0, 86.5, 87.0, 87.5, 88.0, 
  88.5, 89.0, 89.5, 90.0, 90.5, 91.0, 91.5, 92.0, 92.5, 93.0, 
  93.5, 94.0, 94.5, 95.0, 95.5, 96.0, 96.5, 97.0, 97.5, 98.0, 
  98.5, 99.0, 99.5, 100.0
};
const float abvPot[ABV_TABLE_SIZE]  = {
  93.70, 89.06, 83.78, 77.48, 72.17, 67.27, 61.96, 55.75, 50.07, 45.50,
  42.09, 39.07, 35.81, 33.02, 30.39, 28.02, 25.79, 23.95, 22.17, 20.35,
  18.63, 17.16, 15.89, 14.49, 13.27, 12.11, 11.21, 10.39,  9.70,  9.06,
   8.49,  7.94,  7.34,  6.79,  6.21,  5.64,  5.08,  4.45,  3.88,  3.31,
   2.52,  1.69,  0.84,  0.00
};
const float abvOut[ABV_TABLE_SIZE]  = {
  94.35, 91.81, 89.37, 87.16, 85.83, 84.79, 83.69, 82.36, 81.28, 80.37,
  79.63, 78.87, 77.97, 76.94, 75.68, 74.34, 72.97, 71.68, 70.35, 68.88,
  67.37, 65.98, 64.49, 62.67, 60.97, 59.22, 57.58, 55.95, 54.31, 52.65,
  51.06, 49.21, 46.32, 45.27, 42.96, 40.52, 37.96, 35.07, 31.96, 28.69,
  23.54, 16.47,  8.78,  0.00
};

// Вспомогательная функция интерполяции
float interpolate(float x, float x0, float x1, float y0, float y1) {
  if (x1 == x0) return y0;
  return y0 + (x - x0) * (y1 - y0) / (x1 - x0);
}

// Расчет крепости с учетом атмосферного давления
// temp - измеренная температура, pressure - давление в мм рт.ст. (или hPa * 0.75006)
float ConfigManager::getABV(float temp, float pressure_mmHg, bool isOutput) {
  // Корректировка температуры кипения на давление
  // Формула: T_correct = T_meas + (760 - P) * 0.037 (примерный коэффициент)
  float tempCorrected = temp + (760.0 - pressure_mmHg) * 0.037;

  // Поиск в таблице
  if (tempCorrected <= abvTemp[0]) return isOutput ? abvOut[0] : abvPot[0];
  if (tempCorrected >= abvTemp[ABV_TABLE_SIZE - 1]) return isOutput ? abvOut[ABV_TABLE_SIZE - 1] : abvPot[ABV_TABLE_SIZE - 1];

  for (int i = 0; i < ABV_TABLE_SIZE - 1; i++) {
    if (tempCorrected >= abvTemp[i] && tempCorrected <= abvTemp[i+1]) {
      if (isOutput) {
        return interpolate(tempCorrected, abvTemp[i], abvTemp[i+1], abvOut[i], abvOut[i+1]);
      } else {
        return interpolate(tempCorrected, abvTemp[i], abvTemp[i+1], abvPot[i], abvPot[i+1]);
      }
    }
  }
  return 0.0;
}

// Вспомогательные методы для работы с EEPROM
void ConfigManager::writeInt(int address, int value) {
  EEPROM.write(address, value >> 8);
  EEPROM.write(address + 1, value & 0xFF);
  EEPROM.write(address + 2, (value >> 24) & 0xFF);
  EEPROM.write(address + 3, (value >> 16) & 0xFF);
}

int ConfigManager::readInt(int address, int defaultValue) {
  if (EEPROM.read(address) == 0xFF && EEPROM.read(address + 1) == 0xFF &&
      EEPROM.read(address + 2) == 0xFF && EEPROM.read(address + 3) == 0xFF) {
    return defaultValue;
  }
  
  int value = (EEPROM.read(address) << 8) | EEPROM.read(address + 1);
  value |= (EEPROM.read(address + 2) << 24) | (EEPROM.read(address + 3) << 16);
  return value;
}

void ConfigManager::writeFloat(int address, float value) {
  byte* p = (byte*)(void*)&value;
  for (int i = 0; i < sizeof(value); i++) {
    EEPROM.write(address + i, *p++);
  }
}

float ConfigManager::readFloat(int address, float defaultValue) {
  float value = 0.0;
  byte* p = (byte*)(void*)&value;
  
  bool isEmpty = true;
  for (int i = 0; i < sizeof(value); i++) {
    if (EEPROM.read(address + i) != 0xFF) {
      isEmpty = false;
      break;
    }
  }
  
  if (isEmpty) return defaultValue;
  
  for (int i = 0; i < sizeof(value); i++) {
    *p++ = EEPROM.read(address + i);
  }
  return value;
}

void ConfigManager::writeBool(int address, bool value) {
  EEPROM.write(address, value ? 1 : 0);
}

bool ConfigManager::readBool(int address, bool defaultValue) {
  byte val = EEPROM.read(address);
  if (val == 0xFF) return defaultValue;
  return val == 1;
}

void ConfigManager::writeULong(int address, unsigned long value) {
  for (int i = 0; i < 4; i++) {
    EEPROM.write(address + i, (value >> (8 * i)) & 0xFF);
  }
}

unsigned long ConfigManager::readULong(int address, unsigned long defaultValue) {
  if (EEPROM.read(address) == 0xFF && EEPROM.read(address + 1) == 0xFF &&
      EEPROM.read(address + 2) == 0xFF && EEPROM.read(address + 3) == 0xFF) {
    return defaultValue;
  }
  
  unsigned long value = 0;
  for (int i = 0; i < 4; i++) {
    value |= ((unsigned long)EEPROM.read(address + i) << (8 * i));
  }
  return value;
}

// Основные методы
void ConfigManager::begin() {
  EEPROM.begin(EEPROM_SIZE);
  loadConfig();
  Serial.println("ConfigManager initialized with EEPROM");
}

void ConfigManager::loadConfig() {
  // Основные настройки
  currentConfig.heaterType = readInt(ADDR_HEATER_TYPE, 0);
  currentConfig.power = readInt(ADDR_POWER, 3500);
  currentConfig.asVolume = readInt(ADDR_AS_VOLUME, 5000);
  currentConfig.tsaLimit = readInt(ADDR_TSA_LIMIT, 40);
  currentConfig.histeresis = readFloat(ADDR_HISTERESIS, 0.24);
  currentConfig.correlation = readFloat(ADDR_CORRELATION, 1.13);
  currentConfig.delta = readFloat(ADDR_DELTA, 0.06);
  
  // Настройки дистилляции/ректификации
  currentConfig.razgonTemp = readInt(ADDR_RAZGON_TEMP, 30);
  currentConfig.cycleLim = readInt(ADDR_CYCLE_LIM, 3);
  currentConfig.useHeadValve = readBool(ADDR_USE_HEAD_VALVE, true);
  currentConfig.headsTypeKSS = readBool(ADDR_HEADS_TYPE_KSS, false);
  currentConfig.bodyValveNC = readBool(ADDR_BODY_VALVE_NC, false);
  currentConfig.bakStopTemp = readInt(ADDR_BAK_STOP_TEMP, 40);
  currentConfig.fullPwr = readBool(ADDR_FULL_PWR, true);
  
  // Настройки миксера
  currentConfig.mixerOnTime = readInt(ADDR_MIXER_ON_TIME, 30);
  currentConfig.mixerOffTime = readInt(ADDR_MIXER_OFF_TIME, 300);
  currentConfig.mixerEnabled = readBool(ADDR_MIXER_ENABLED, true);
  
  // Настройки клапанов
  currentConfig.headOpenMs = readInt(ADDR_HEAD_OPEN_MS, 1200);
  currentConfig.headCloseMs = readInt(ADDR_HEAD_CLOSE_MS, 2200);
  currentConfig.bodyOpenMs = readInt(ADDR_BODY_OPEN_MS, 1100);
  currentConfig.bodyCloseMs = readInt(ADDR_BODY_CLOSE_MS, 2100);
  
  // Безопасность
  currentConfig.emergencyTime = readInt(ADDR_EMERGENCY_TIME, 3);
  currentConfig.nasebTime = readInt(ADDR_NASEB_TIME, 10);
  currentConfig.reklapTime = readInt(ADDR_REKLAP_TIME, 10);
  currentConfig.boxMaxTemp = readInt(ADDR_BOX_MAX_TEMP, 60);
  
  // Процессы
  currentConfig.activeProcess = (ProcessType)readInt(ADDR_ACTIVE_PROCESS, PROCESS_NONE);
  currentConfig.processRunning = readBool(ADDR_PROCESS_RUNNING, false);
  currentConfig.processStartTime = readULong(ADDR_PROCESS_START_TIME, 0);
  
  // Служебные переменные
  currentConfig.syncInProgress = readBool(ADDR_SYNC_IN_PROGRESS, false);

  // --- НОВЫЕ ПЕРЕМЕННЫЕ ---
  currentConfig.chekwifi = readInt(ADDR_CHEKWIFI, 5);
  currentConfig.valveuse = readBool(ADDR_VALVE_USE, true);
  currentConfig.midterm = readInt(ADDR_MIDTERM, 35);
  currentConfig.midterm_abv = readInt(ADDR_MIDTERM_ABV, 0);
  currentConfig.calibration = readBool(ADDR_CALIBRATION, true);
  currentConfig.active_test = readInt(ADDR_ACTIVE_TEST, 60);
  
  currentConfig.valve_head_capacity = readInt(ADDR_VALVE_HEAD_CAP, 100);
  currentConfig.valve_body_capacity = readInt(ADDR_VALVE_BODY_CAP, 100);
  currentConfig.valve0_body_capacity = readInt(ADDR_VALVE0_BODY_CAP, 100);

  loadSensorAddress(ADDR_TSA_ADDRESS, currentConfig.tsaAddress);
  loadSensorAddress(ADDR_AQUA_ADDRESS, currentConfig.aquaAddress);
  loadSensorAddress(ADDR_TSAR_ADDRESS, currentConfig.tsarAddress);
  loadSensorAddress(ADDR_TANK_ADDRESS, currentConfig.tankAddress);
  
  Serial.println("Config loaded from EEPROM");
}

void ConfigManager::saveConfig() {
  // Основные настройки
  writeInt(ADDR_HEATER_TYPE, currentConfig.heaterType);
  writeInt(ADDR_POWER, currentConfig.power);
  writeInt(ADDR_AS_VOLUME, currentConfig.asVolume);
  writeInt(ADDR_TSA_LIMIT, currentConfig.tsaLimit);
  writeFloat(ADDR_HISTERESIS, currentConfig.histeresis);
  writeFloat(ADDR_CORRELATION, currentConfig.correlation);
  writeFloat(ADDR_DELTA, currentConfig.delta);
  
  // Настройки дистилляции/ректификации
  writeInt(ADDR_RAZGON_TEMP, currentConfig.razgonTemp);
  writeInt(ADDR_CYCLE_LIM, currentConfig.cycleLim);
  writeBool(ADDR_USE_HEAD_VALVE, currentConfig.useHeadValve);
  writeBool(ADDR_HEADS_TYPE_KSS, currentConfig.headsTypeKSS);
  writeBool(ADDR_BODY_VALVE_NC, currentConfig.bodyValveNC);
  writeInt(ADDR_BAK_STOP_TEMP, currentConfig.bakStopTemp);
  writeBool(ADDR_FULL_PWR, currentConfig.fullPwr);
  
  // Настройки миксера
  writeInt(ADDR_MIXER_ON_TIME, currentConfig.mixerOnTime);
  writeInt(ADDR_MIXER_OFF_TIME, currentConfig.mixerOffTime);
  writeBool(ADDR_MIXER_ENABLED, currentConfig.mixerEnabled);
  
  // Настройки клапанов
  writeInt(ADDR_HEAD_OPEN_MS, currentConfig.headOpenMs);
  writeInt(ADDR_HEAD_CLOSE_MS, currentConfig.headCloseMs);
  writeInt(ADDR_BODY_OPEN_MS, currentConfig.bodyOpenMs);
  writeInt(ADDR_BODY_CLOSE_MS, currentConfig.bodyCloseMs);
  
  // Безопасность
  writeInt(ADDR_EMERGENCY_TIME, currentConfig.emergencyTime);
  writeInt(ADDR_NASEB_TIME, currentConfig.nasebTime);
  writeInt(ADDR_REKLAP_TIME, currentConfig.reklapTime);
  writeInt(ADDR_BOX_MAX_TEMP, currentConfig.boxMaxTemp);
  
  // Процессы
  writeInt(ADDR_ACTIVE_PROCESS, currentConfig.activeProcess);
  writeBool(ADDR_PROCESS_RUNNING, currentConfig.processRunning);
  writeULong(ADDR_PROCESS_START_TIME, currentConfig.processStartTime);
  
  // Служебные переменные
  writeBool(ADDR_SYNC_IN_PROGRESS, currentConfig.syncInProgress);

  // --- НОВЫЕ ПЕРЕМЕННЫЕ ---
  writeInt(ADDR_CHEKWIFI, currentConfig.chekwifi);
  writeBool(ADDR_VALVE_USE, currentConfig.valveuse);
  writeInt(ADDR_MIDTERM, currentConfig.midterm);
  writeInt(ADDR_MIDTERM_ABV, currentConfig.midterm_abv);
  writeBool(ADDR_CALIBRATION, currentConfig.calibration);
  writeInt(ADDR_ACTIVE_TEST, currentConfig.active_test);
  
  writeInt(ADDR_VALVE_HEAD_CAP, currentConfig.valve_head_capacity);
  writeInt(ADDR_VALVE_BODY_CAP, currentConfig.valve_body_capacity);
  writeInt(ADDR_VALVE0_BODY_CAP, currentConfig.valve0_body_capacity);

  // Сохраняем адреса датчиков
  saveSensorAddress(ADDR_TSA_ADDRESS, currentConfig.tsaAddress);
  saveSensorAddress(ADDR_AQUA_ADDRESS, currentConfig.aquaAddress);
  saveSensorAddress(ADDR_TSAR_ADDRESS, currentConfig.tsarAddress);
  saveSensorAddress(ADDR_TANK_ADDRESS, currentConfig.tankAddress);
  
  EEPROM.commit();
  
  currentConfig.localChangeTimestamp = millis();
  currentConfig.configChangedLocally = true;
  
  Serial.println("Full config saved to EEPROM");
}

void ConfigManager::saveDistConfig() {
  writeInt(ADDR_RAZGON_TEMP, currentConfig.razgonTemp);
  writeInt(ADDR_BAK_STOP_TEMP, currentConfig.bakStopTemp);
  writeInt(ADDR_HEATER_TYPE, currentConfig.heaterType);
  writeBool(ADDR_MIXER_ENABLED, currentConfig.mixerEnabled);
  writeInt(ADDR_MIXER_ON_TIME, currentConfig.mixerOnTime);
  writeInt(ADDR_MIXER_OFF_TIME, currentConfig.mixerOffTime);
  
  // Сохраняем новые настройки DIST
  writeBool(ADDR_VALVE_USE, currentConfig.valveuse);
  writeInt(ADDR_MIDTERM, currentConfig.midterm);
  writeInt(ADDR_MIDTERM_ABV, currentConfig.midterm_abv);
  EEPROM.commit();
  currentConfig.localChangeTimestamp = millis();
  currentConfig.configChangedLocally = true;
  Serial.println("Dist config saved to EEPROM");
}

void ConfigManager::saveRectConfig() {
  writeInt(ADDR_TSA_LIMIT, currentConfig.tsaLimit);
  writeInt(ADDR_RAZGON_TEMP, currentConfig.razgonTemp);
  writeInt(ADDR_CYCLE_LIM, currentConfig.cycleLim);
  writeFloat(ADDR_HISTERESIS, currentConfig.histeresis);
  writeFloat(ADDR_DELTA, currentConfig.delta);
  writeInt(ADDR_HEATER_TYPE, currentConfig.heaterType);
  writeBool(ADDR_USE_HEAD_VALVE, currentConfig.useHeadValve);
  writeBool(ADDR_BODY_VALVE_NC, currentConfig.bodyValveNC);
  writeBool(ADDR_SYNC_IN_PROGRESS, currentConfig.syncInProgress);
  writeBool(ADDR_HEADS_TYPE_KSS, currentConfig.headsTypeKSS);
  
  // Сохраняем новые настройки RECT
  writeBool(ADDR_CALIBRATION, currentConfig.calibration);
  writeInt(ADDR_ACTIVE_TEST, currentConfig.active_test);
  writeInt(ADDR_VALVE_HEAD_CAP, currentConfig.valve_head_capacity);
  writeInt(ADDR_VALVE_BODY_CAP, currentConfig.valve_body_capacity);
  writeInt(ADDR_VALVE0_BODY_CAP, currentConfig.valve0_body_capacity);
  // Сохраняем тайминги клапанов
  writeInt(ADDR_HEAD_OPEN_MS, currentConfig.headOpenMs);
  writeInt(ADDR_HEAD_CLOSE_MS, currentConfig.headCloseMs);
  writeInt(ADDR_BODY_OPEN_MS, currentConfig.bodyOpenMs);
  writeInt(ADDR_BODY_CLOSE_MS, currentConfig.bodyCloseMs);
  
  EEPROM.commit();
  currentConfig.localChangeTimestamp = millis();
  currentConfig.configChangedLocally = true;
  Serial.println("Rect config saved to EEPROM");
}

SystemConfig& ConfigManager::getConfig() {
  return currentConfig;
}

void ConfigManager::setConfig(const SystemConfig& newConfig) {
  currentConfig = newConfig;
}

bool ConfigManager::startProcess(ProcessType process) {
  if (currentConfig.processRunning) {
    Serial.println("Cannot start process: another process is running");
    return false;
  }
  
  currentConfig.activeProcess = process;
  currentConfig.processRunning = true;
  currentConfig.processStartTime = millis();
  
  writeInt(ADDR_ACTIVE_PROCESS, currentConfig.activeProcess);
  writeBool(ADDR_PROCESS_RUNNING, currentConfig.processRunning);
  writeULong(ADDR_PROCESS_START_TIME, currentConfig.processStartTime);
  EEPROM.commit();
  
  Serial.print("Process started: ");
  Serial.println(process);
  return true;
}

void ConfigManager::stopProcess() {
  currentConfig.processRunning = false;
  currentConfig.activeProcess = PROCESS_NONE;
  currentConfig.processStartTime = 0;
  
  writeInt(ADDR_ACTIVE_PROCESS, currentConfig.activeProcess);
  writeBool(ADDR_PROCESS_RUNNING, currentConfig.processRunning);
  writeULong(ADDR_PROCESS_START_TIME, currentConfig.processStartTime);
  EEPROM.commit();
  
  Serial.println("Process stopped");
}

ProcessType ConfigManager::getActiveProcess() {
  return currentConfig.activeProcess;
}

bool ConfigManager::isProcessRunning() {
  return currentConfig.processRunning;
}

bool ConfigManager::isDistProcessRunning() {
  return currentConfig.processRunning && currentConfig.activeProcess == PROCESS_DIST;
}

bool ConfigManager::isRectProcessRunning() {
  return currentConfig.processRunning && currentConfig.activeProcess == PROCESS_RECT;
}

void ConfigManager::saveSensorAddress(int address, uint8_t* addr) {
  for (int i = 0; i < 8; i++) {
    EEPROM.write(address + i, addr[i]);
  }
}

void ConfigManager::loadSensorAddress(int address, uint8_t* addr) {
  for (int i = 0; i < 8; i++) {
    addr[i] = EEPROM.read(address + i);
  }
}

// Обратный расчет: Крепость в струе -> Температура куба (с поправкой на давление)
float ConfigManager::getTempForOutputABV(float targetAbv, float pressure_mmHg) {
  // 1. Проверка границ
  // Если крепость выше максимальной в таблице (это почти спирт)
  if (targetAbv >= abvOut[0]) return abvTemp[0];
  // Если крепость ниже минимальной (вода)
  if (targetAbv <= abvOut[ABV_TABLE_SIZE - 1]) return abvTemp[ABV_TABLE_SIZE - 1];

  // 2. Поиск интервала в массиве abvOut
  // Массив abvOut отсортирован по убыванию (94% -> 0%).
  // Ищем пару индексов i и i+1, между которыми находится targetAbv
  int lowerIdx = -1;
  for (int i = 0; i < ABV_TABLE_SIZE - 1; i++) {
    // Ищем переход: [i] >= target > [i+1]
    if (abvOut[i] >= targetAbv && abvOut[i+1] < targetAbv) {
      lowerIdx = i;
      break;
    }
  }

  if (lowerIdx == -1) return 0.0; // Ошибка поиска

  // 3. Интерполяция температуры (находим T_base при 760 мм рт.ст.)
  // interpolate(x, x0, x1, y0, y1)
  float T_base = interpolate(targetAbv, abvOut[lowerIdx], abvOut[lowerIdx+1], abvTemp[lowerIdx], abvTemp[lowerIdx+1]);

  // 4. Корректировка на давление
  // Формула связи: T_привед = T_изм + (760 - P) * 0.037
  // Нам нужно T_изм (то, что покажет датчик).
  // T_изм = T_привед - (760 - P) * 0.037
  // T_изм = T_привед + (P - 760) * 0.037
  
  float T_corrected = T_base + (pressure_mmHg - 760.0) * 0.037;

  return T_corrected;
}
// Прямой расчет: Температура куба -> Крепость в струе (%)
float ConfigManager::getOutputABVForTemp(float temp, float pressure_mmHg) {
  // 1. Корректировка температуры
  float tempCorrected = temp + (760.0 - pressure_mmHg) * 0.037;

  // 2. Границы (защита от выхода за таблицу)
  if (tempCorrected <= abvTemp[0]) return abvOut[0]; 
  if (tempCorrected >= abvTemp[ABV_TABLE_SIZE - 1]) return abvOut[ABV_TABLE_SIZE - 1]; // Это 0.0

  // 3. Поиск и интерполяция
  for (int i = 0; i < ABV_TABLE_SIZE - 1; i++) {
    if (tempCorrected >= abvTemp[i] && tempCorrected <= abvTemp[i+1]) {
      return interpolate(tempCorrected, abvTemp[i], abvTemp[i+1], abvOut[i], abvOut[i+1]);
    }
  }
  return 0.0;
}

