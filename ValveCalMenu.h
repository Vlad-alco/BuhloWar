#ifndef VALVE_CAL_MENU_H
#define VALVE_CAL_MENU_H

#include <LiquidCrystal_I2C.h>
#include "config.h"
#include "preferences.h"
#include "common.h"
#include "OutputManager.h"
#include <cstring>

// === КОНСТАНТЫ СКОРОСТИ ОТБОРА ===
// Базовая скорость при 1 кВт мощности
const int SPEED_HEAD_1kW = 50;    // мл/ч/кВт для голов
const int SPEED_BODY_1kW = 500;   // мл/ч/кВт для тела
// =================================

// === ЭТАПЫ МАСТЕРА КАЛИБРОВКИ ===
enum class CalibState {
  MENU_MAIN,           // Выбор клапана: HEADS / BODY NC / BODY NO / EXIT
  WIZARD_DRY_RUN,      // Шаг 1: Пролив системы (10 сек, 100% open)
  WIZARD_CAPACITY,     // Шаг 2: Capacity (HEADS=импульс голов, BODY_NO=100% open)
  WIZARD_CAP_HEADS,    // Шаг 2a: Capacity BODY NC на скорости голов (импульс)
  WIZARD_CAP_BODY,     // Шаг 2b: Capacity BODY NC на скорости тела (импульс)
  WIZARD_INPUT,        // Ввод объёма (1 мл шаг)
  WIZARD_INPUT2,       // Ввод объёма второго теста (BODY NC — тело)
  WIZARD_RESULT        // Результат калибровки
};

enum class CalibValve {
  HEADS,
  BODY_NC,
  BODY_NO
};

enum class CalibStep {
  IDLE,
  DRY_RUN,
  CAPACITY,
  CAPACITY_HEADS,
  CAPACITY_BODY,
  INPUT_VOLUME,
  INPUT_VOLUME2,
  RESULT
};

// === СОСТОЯНИЕ МАСТЕРА КАЛИБРОВКИ ===
struct CalibWizardState {
  CalibValve valve = CalibValve::HEADS;    // HEADS / BODY_NC / BODY_NO
  CalibStep step = CalibStep::IDLE;         // IDLE / DRY_RUN / CAPACITY / ...
  bool launchedByProcess = false;           // true = авто (LCD+Web sync), false = ручной Web only
  bool isTestRunning = false;
  unsigned long testStartTime = 0;
  int testDurationSec = 0;                  // 10 или 60
  float enteredVolume = 0.0f;               // мл (1 шаг)
  float headsTestVolume = 0.0f;             // запомненный объём первого теста BODY NC
  float calculatedCapacity = 0.0f;          // мл/мин (текущий/последний рассчитанный)
  float calculatedCapHeads = 0.0f;          // мл/мин — capacity на скорости голов (BODY NC)
  float calculatedCapBody = 0.0f;           // мл/мин — capacity на скорости тела (BODY NC)
  int testOpenMs = 0;                       // openMs использованный в текущем тесте
  int testCloseMs = 0;                      // closeMs использованный в текущем тесте
  bool testIsCycling = false;               // true = тест в импульсном режиме
};
// ====================================

class ValveCalMenu {
private:
  LiquidCrystal_I2C* lcd;
  ConfigManager* config;
  OutputManager* output;
  
  CalibState currentState = CalibState::MENU_MAIN;
  int selectedItem = 0;
  
  // Состояние мастера
  CalibWizardState wizard;
  
  bool exitConfirmed = false;
  
  // === ВСПОМОГАТЕЛЬНЫЕ МЕТОДЫ ===
  
  const char* getValveName(CalibValve v) {
    switch(v) {
      case CalibValve::HEADS: return "HEADS";
      case CalibValve::BODY_NC: return "BODY NC";
      case CalibValve::BODY_NO: return "BODY NO";
      default: return "???";
    }
  }
  
  void startWizard(CalibValve valve) {
    wizard.valve = valve;
    wizard.step = CalibStep::DRY_RUN;
    wizard.launchedByProcess = true;
    wizard.isTestRunning = false;
    wizard.enteredVolume = 0.0f;
    wizard.headsTestVolume = 0.0f;
    wizard.calculatedCapacity = 0.0f;
    wizard.calculatedCapHeads = 0.0f;
    wizard.calculatedCapBody = 0.0f;
    wizard.testOpenMs = 0;
    wizard.testCloseMs = 0;
    wizard.testIsCycling = false;
    currentState = CalibState::WIZARD_DRY_RUN;
    display();
  }
  
  // Открыть клапан на 100% (для dry run и BODY NO capacity)
  void openValveForTest() {
    switch(wizard.valve) {
      case CalibValve::HEADS:
        output->openHeadValve();
        break;
      case CalibValve::BODY_NC:
      case CalibValve::BODY_NO:
        output->openBodyValve();
        break;
    }
  }
  
  // Закрыть клапан
  void closeValveForTest() {
    switch(wizard.valve) {
      case CalibValve::HEADS:
        output->stopHeadValveTest();
        output->closeHeadValve();
        break;
      case CalibValve::BODY_NC:
      case CalibValve::BODY_NO:
        output->stopBodyValveTest();
        output->closeBodyValve();
        break;
    }
  }
  
  // === Расчёт таймингов импульсного теста ===
  // targetSpeedMlH — целевая скорость в мл/ч
  // Возвращает true если cycling запущен, false если fallback на 100% open
  bool startCyclingForTest(float targetSpeedMlH) {
    SystemConfig& cfg = config->getConfig();
    int minOpen = cfg.minOpenTime > 0 ? cfg.minOpenTime : 100;
    
    // Берём текущий capacity как оценку (или дефолт 100)
    float capEstimate = 0.0f;
    switch(wizard.valve) {
      case CalibValve::HEADS:
        capEstimate = (float)cfg.valve_head_capacity;
        break;
      case CalibValve::BODY_NC:
        // Для первого теста (головы) берём valve_body_capacity, для второго (тело) тоже
        capEstimate = (float)cfg.valve_body_capacity;
        break;
      case CalibValve::BODY_NO:
        capEstimate = (float)cfg.valve0_body_capacity;
        break;
    }
    if (capEstimate < 1.0f) capEstimate = 100.0f;
    
    // Рассчитываем duty cycle
    float dutyCycle = targetSpeedMlH / (capEstimate * 60.0f);
    if (dutyCycle > 0.9f) dutyCycle = 0.9f;
    if (dutyCycle < 0.001f) dutyCycle = 0.001f;
    
    // Рассчитываем openMs/closeMs (same logic as calcValveTiming)
    int openMs, closeMs;
    float idealOpenMs = dutyCycle * minOpen;
    
    if (idealOpenMs >= minOpen) {
      openMs = (int)(idealOpenMs + 0.5f);
      closeMs = minOpen - openMs;
    } else {
      openMs = minOpen;
      float closeMsCalc = (float)openMs * (1.0f - dutyCycle) / dutyCycle;
      closeMs = (int)(closeMsCalc + 0.5f);
    }
    if (closeMs < 100) closeMs = 100;
    
    // Запоминаем для обратного расчёта capacity
    wizard.testOpenMs = openMs;
    wizard.testCloseMs = closeMs;
    wizard.testIsCycling = true;
    
    // Запускаем cycling
    switch(wizard.valve) {
      case CalibValve::HEADS:
        output->startHeadValveCycling(openMs, closeMs);
        Serial.printf("[Calib] Heads cycling: open=%dms close=%dms duty=%.4f\n", openMs, closeMs, dutyCycle);
        break;
      case CalibValve::BODY_NC:
      case CalibValve::BODY_NO:
        output->startBodyValveCycling(openMs, closeMs);
        Serial.printf("[Calib] Body cycling: open=%dms close=%dms duty=%.4f\n", openMs, closeMs, dutyCycle);
        break;
    }
    return true;
  }
  
  // Обратный расчёт capacity по измеренному объёму и тестовому duty cycle
  float backCalculateCapacity(float volumeMl, int durationSec) {
    if (durationSec <= 0 || volumeMl <= 0) return 0.0f;
    
    if (wizard.testIsCycling && wizard.testOpenMs > 0 && wizard.testCloseMs > 0) {
      // Импульсный режим: реальная пропускная способность
      float dutyCycle = (float)wizard.testOpenMs / (float)(wizard.testOpenMs + wizard.testCloseMs);
      // volumeMl = capacity_true * dutyCycle * durationSec / 60.0
      // capacity_true = volumeMl * 60.0 / (dutyCycle * durationSec)
      float capTrue = volumeMl * 60.0f / (dutyCycle * (float)durationSec);
      return capTrue;
    } else {
      // 100% open: capacity = volume / time_min
      return volumeMl / ((float)durationSec / 60.0f);
    }
  }
  
  // Сохранить результаты калибровки
  void saveCapacity() {
    SystemConfig& cfg = config->getConfig();
    
    switch(wizard.valve) {
      case CalibValve::HEADS: {
        // HEADS: calculatedCapacity = результат импульсного теста на скорости голов
        int capacityInt = (int)(wizard.calculatedCapacity + 0.5f);
        cfg.valve_head_capacity = capacityInt;
        Serial.printf("[Calib] Save HEADS capacity: %d ml/min\n", capacityInt);
        break;
      }
      case CalibValve::BODY_NC: {
        // BODY NC: два значения
        int capHeadsInt = (int)(wizard.calculatedCapHeads + 0.5f);
        int capBodyInt = (int)(wizard.calculatedCapBody + 0.5f);
        cfg.valve_body_capacity_heads = capHeadsInt;
        cfg.valve_body_capacity = capBodyInt;
        Serial.printf("[Calib] Save BODY NC: cap_heads=%d, cap_body=%d ml/min\n", capHeadsInt, capBodyInt);
        break;
      }
      case CalibValve::BODY_NO: {
        // BODY NO: 100% open, одно значение
        int capacityInt = (int)(wizard.calculatedCapacity + 0.5f);
        cfg.valve0_body_capacity = capacityInt;
        Serial.printf("[Calib] Save BODY NO capacity: %d ml/min\n", capacityInt);
        break;
      }
    }
    config->saveRectConfig();
  }
  
  // Получить целевую скорость голов (мл/ч) с учётом мощности
  float getHeadsTargetSpeed() {
    SystemConfig& cfg = config->getConfig();
    float koff = cfg.power / 1000.0f;
    return koff * (float)cfg.speedGolovyBase;
  }
  
  // Получить целевую скорость тела (мл/ч) с учётом мощности
  float getBodyTargetSpeed() {
    SystemConfig& cfg = config->getConfig();
    float koff = cfg.power / 1000.0f;
    return koff * (float)cfg.speedTeloBase;
  }
  
  // Проверить, является ли текущий клапан BODY NC (нужна двухточечная калибровка)
  bool isTwoPointCalibration() {
    return wizard.valve == CalibValve::BODY_NC;
  }
  
  // Проверить, нужен ли импульсный режим для capacity теста
  bool needsCyclingForCapacity() {
    switch(wizard.valve) {
      case CalibValve::HEADS:
        return true;     // Всегда импульс на скорости голов
      case CalibValve::BODY_NC:
        return true;     // Импульс на головах и теле
      case CalibValve::BODY_NO:
        return false;    // 100% open
    }
    return false;
  }
  
public:
  ValveCalMenu(LiquidCrystal_I2C* lcdPtr, ConfigManager* cfg, OutputManager* out) {
    lcd = lcdPtr;
    config = cfg;
    output = out;
  }
  
  // === ГЕТТЕРЫ ДЛЯ WEB ===
  CalibWizardState& getWizardState() { return wizard; }
  bool isTestRunning() { return wizard.isTestRunning; }
  int getTestRemaining() {
    if (!wizard.isTestRunning) return 0;
    unsigned long elapsed = (millis() - wizard.testStartTime) / 1000;
    return (elapsed < (unsigned long)wizard.testDurationSec) ? (wizard.testDurationSec - (int)elapsed) : 0;
  }
  // ========================
  
  void display() {
    exitConfirmed = false;
    if (wizard.isTestRunning) return;  // Не обновляем экран во время теста
    
    lcd->clear();
    
    switch(currentState) {
      case CalibState::MENU_MAIN: displayMainMenu(); break;
      case CalibState::WIZARD_DRY_RUN: displayDryRun(); break;
      case CalibState::WIZARD_CAPACITY: displayCapacity(); break;
      case CalibState::WIZARD_CAP_HEADS: displayCapHeads(); break;
      case CalibState::WIZARD_CAP_BODY: displayCapBody(); break;
      case CalibState::WIZARD_INPUT: displayInput(); break;
      case CalibState::WIZARD_INPUT2: displayInput2(); break;
      case CalibState::WIZARD_RESULT: displayResult(); break;
    }
  }
  
  void displayMainMenu() {
    // Заголовок
    lcd->setCursor(4, 0);
    lcd->print("VALVE CALIBRATION");
    
    // Пункты меню
    const char* items[] = {"HEADS", "BODY NC", "BODY NO", "EXIT"};
    
    for (int i = 0; i < 4; i++) {
      lcd->setCursor(0, i + 0);
      lcd->print(i == selectedItem ? ">" : " ");
      lcd->print(items[i]);
      
      // Показываем текущий capacity для каждого клапана
      SystemConfig& cfg = config->getConfig();
      if (i < 3) {
        lcd->print(" ");
        if (i == 0) {
          lcd->print(cfg.valve_head_capacity);
          lcd->print("ml/m");
        } else if (i == 1) {
          lcd->print(cfg.valve_body_capacity);
          if (cfg.valve_body_capacity_heads > 0) {
            lcd->print("H");
            lcd->print(cfg.valve_body_capacity_heads);
          }
        } else {
          lcd->print(cfg.valve0_body_capacity);
          lcd->print("ml/m");
        }
      }
    }
  }
  
  void displayDryRun() {
    lcd->setCursor(0, 0);
    lcd->print("VALVE CALIBRATION");
    
    lcd->setCursor(0, 1);
    lcd->print(getValveName(wizard.valve));
    
    int totalSteps = isTwoPointCalibration() ? 4 : 3;
    lcd->print(" Step 1/");
    lcd->print(totalSteps);
    lcd->print(": Flush");
    
    if (wizard.isTestRunning) {
      lcd->setCursor(0, 2);
      lcd->print("Valve OPEN 100%");
      
      lcd->setCursor(0, 3);
      unsigned long elapsed = (millis() - wizard.testStartTime) / 1000;
      char buf[16];
      sprintf(buf, "%03d / %03d sec", (int)elapsed, wizard.testDurationSec);
      lcd->print(buf);
    } else {
      lcd->setCursor(0, 2);
      lcd->print("Place container");
      
      lcd->setCursor(0, 3);
      lcd->print("SET-start BACK-exit");
    }
  }
  
  void displayCapacity() {
    // HEADS: импульс на скорости голов
    // BODY NO: 100% open
    lcd->setCursor(0, 0);
    lcd->print("VALVE CALIBRATION");
    
    lcd->setCursor(0, 1);
    lcd->print(getValveName(wizard.valve));
    
    if (wizard.valve == CalibValve::HEADS) {
      lcd->print(" Step 2/3: Cap.");
    } else {
      lcd->print(" Step 2/3: Cap.");
    }
    
    if (wizard.isTestRunning) {
      lcd->setCursor(0, 2);
      if (wizard.testIsCycling) {
        lcd->print("Pulse: ");
        lcd->print(wizard.testOpenMs);
        lcd->print("/");
        lcd->print(wizard.testCloseMs);
        lcd->print("ms");
      } else {
        lcd->print("Valve OPEN 100%");
      }
      
      lcd->setCursor(0, 3);
      unsigned long elapsed = (millis() - wizard.testStartTime) / 1000;
      char buf[16];
      sprintf(buf, "%03d / %03d sec", (int)elapsed, wizard.testDurationSec);
      lcd->print(buf);
    } else {
      lcd->setCursor(0, 2);
      lcd->print("Place measuring cup");
      
      lcd->setCursor(0, 3);
      lcd->print("SET-start");
    }
  }
  
  void displayCapHeads() {
    // BODY NC Step 2/4: Capacity на скорости голов
    lcd->setCursor(0, 0);
    lcd->print("VALVE CALIBRATION");
    
    lcd->setCursor(0, 1);
    lcd->print("BODY NC Step 2/4");
    
    if (wizard.isTestRunning) {
      lcd->setCursor(0, 2);
      lcd->print("Pulse HEAD speed");
      
      lcd->setCursor(0, 3);
      unsigned long elapsed = (millis() - wizard.testStartTime) / 1000;
      char buf[16];
      sprintf(buf, "%03d / %03d sec", (int)elapsed, wizard.testDurationSec);
      lcd->print(buf);
    } else {
      lcd->setCursor(0, 2);
      lcd->print("Cup for HEAD speed");
      
      lcd->setCursor(0, 3);
      lcd->print("SET-start");
    }
  }
  
  void displayCapBody() {
    // BODY NC Step 3/4: Capacity на скорости тела
    lcd->setCursor(0, 0);
    lcd->print("VALVE CALIBRATION");
    
    lcd->setCursor(0, 1);
    lcd->print("BODY NC Step 3/4");
    
    if (wizard.isTestRunning) {
      lcd->setCursor(0, 2);
      lcd->print("Pulse BODY speed");
      
      lcd->setCursor(0, 3);
      unsigned long elapsed = (millis() - wizard.testStartTime) / 1000;
      char buf[16];
      sprintf(buf, "%03d / %03d sec", (int)elapsed, wizard.testDurationSec);
      lcd->print(buf);
    } else {
      lcd->setCursor(0, 2);
      lcd->print("Cup for BODY speed");
      
      lcd->setCursor(0, 3);
      lcd->print("SET-start");
    }
  }
  
  void displayInput() {
    lcd->setCursor(0, 0);
    lcd->print("ENTER VOLUME");
    
    lcd->setCursor(0, 1);
    lcd->print("[");
    lcd->print((int)wizard.enteredVolume);
    lcd->print("]");
    lcd->print(" ml");
    
    if (isTwoPointCalibration() && wizard.step == CalibStep::CAPACITY_HEADS) {
      lcd->setCursor(0, 2);
      lcd->print("HEAD speed test");
    } else {
      lcd->setCursor(0, 2);
      lcd->print("UP/DOWN +/-1");
    }
    
    lcd->setCursor(0, 3);
    lcd->print("SET-confirm");
  }
  
  void displayInput2() {
    lcd->setCursor(0, 0);
    lcd->print("ENTER VOLUME (2)");
    
    lcd->setCursor(0, 1);
    lcd->print("[");
    lcd->print((int)wizard.enteredVolume);
    lcd->print("]");
    lcd->print(" ml");
    
    lcd->setCursor(0, 2);
    lcd->print("BODY speed test");
    
    lcd->setCursor(0, 3);
    lcd->print("SET-confirm");
  }
  
  void displayResult() {
    lcd->setCursor(0, 0);
    lcd->print("CALIBRATION DONE");
    
    SystemConfig& cfg = config->getConfig();
    
    if (isTwoPointCalibration()) {
      // BODY NC: два результата
      lcd->setCursor(0, 1);
      lcd->print("H:");
      lcd->print((int)wizard.calculatedCapHeads);
      lcd->print(" B:");
      lcd->print((int)wizard.calculatedCapBody);
      lcd->print(" ml/m");
      
      lcd->setCursor(0, 2);
      lcd->print("minOpen:");
      lcd->print(cfg.minOpenTime);
      lcd->print("ms (def)");
    } else {
      // HEADS или BODY NO: один результат
      lcd->setCursor(0, 1);
      lcd->print("Cap: ");
      lcd->print(wizard.calculatedCapacity, 1);
      lcd->print(" ml/min");
      
      lcd->setCursor(0, 2);
      lcd->print("minOpen: ");
      lcd->print(cfg.minOpenTime);
      lcd->print("ms (def)");
    }
    
    lcd->setCursor(0, 3);
    lcd->print("SET-next BACK-exit");
  }
  
  // === ОБРАБОТЧИКИ КНОПОК ===
  
  void handleUpButton() {
    if (wizard.isTestRunning) return;
    
    switch(currentState) {
      case CalibState::MENU_MAIN:
        selectedItem--;
        if (selectedItem < 0) selectedItem = 3;
        display();
        break;
        
      case CalibState::WIZARD_INPUT:
      case CalibState::WIZARD_INPUT2:
        wizard.enteredVolume += 1.0f;
        if (wizard.enteredVolume > 9999.0f) wizard.enteredVolume = 9999.0f;
        display();
        break;
        
      default:
        break;
    }
  }
  
  void handleDownButton() {
    if (wizard.isTestRunning) return;
    
    switch(currentState) {
      case CalibState::MENU_MAIN:
        selectedItem++;
        if (selectedItem > 3) selectedItem = 0;
        display();
        break;
        
      case CalibState::WIZARD_INPUT:
      case CalibState::WIZARD_INPUT2:
        wizard.enteredVolume -= 1.0f;
        if (wizard.enteredVolume < 0.0f) wizard.enteredVolume = 0.0f;
        display();
        break;
        
      default:
        break;
    }
  }
  
  void handleSetButton() {
    if (wizard.isTestRunning) return;
    
    switch(currentState) {
      case CalibState::MENU_MAIN:
        if (selectedItem < 3) {
          startWizard((CalibValve)selectedItem);
        } else {
          exitConfirmed = true;  // EXIT
        }
        break;
        
      case CalibState::WIZARD_DRY_RUN:
        if (!wizard.isTestRunning) {
          // Начинаем dry run — клапан 100% open
          SystemConfig& cfgDry = config->getConfig();
          wizard.testDurationSec = cfgDry.calibDrySec > 0 ? cfgDry.calibDrySec : 10;
          wizard.testIsCycling = false;
          wizard.testOpenMs = 0;
          wizard.testCloseMs = 0;
          wizard.isTestRunning = true;
          wizard.testStartTime = millis();
          openValveForTest();
          display();
        }
        break;
        
      case CalibState::WIZARD_CAPACITY:
        if (!wizard.isTestRunning) {
          // Capacity: HEADS = импульс голов, BODY NO = 100% open
          SystemConfig& cfgCap = config->getConfig();
          wizard.testDurationSec = cfgCap.calibCapacitySec > 0 ? cfgCap.calibCapacitySec : 60;
          wizard.isTestRunning = true;
          wizard.testStartTime = millis();
          
          if (needsCyclingForCapacity()) {
            // HEADS: импульс на скорости голов
            startCyclingForTest(getHeadsTargetSpeed());
          } else {
            // BODY NO: 100% open
            wizard.testIsCycling = false;
            openValveForTest();
          }
          display();
        }
        break;
        
      case CalibState::WIZARD_CAP_HEADS:
        if (!wizard.isTestRunning) {
          // BODY NC: capacity на скорости голов (импульс)
          SystemConfig& cfgCH = config->getConfig();
          wizard.testDurationSec = cfgCH.calibCapacitySec > 0 ? cfgCH.calibCapacitySec : 60;
          wizard.isTestRunning = true;
          wizard.testStartTime = millis();
          startCyclingForTest(getHeadsTargetSpeed());
          display();
        }
        break;
        
      case CalibState::WIZARD_CAP_BODY:
        if (!wizard.isTestRunning) {
          // BODY NC: capacity на скорости тела (импульс)
          SystemConfig& cfgCB = config->getConfig();
          wizard.testDurationSec = cfgCB.calibCapacitySec > 0 ? cfgCB.calibCapacitySec : 60;
          wizard.isTestRunning = true;
          wizard.testStartTime = millis();
          startCyclingForTest(getBodyTargetSpeed());
          display();
        }
        break;
        
      case CalibState::WIZARD_INPUT:
        if (isTwoPointCalibration() && wizard.step == CalibStep::CAPACITY_HEADS) {
          // BODY NC: первый тест (головы) завершён, запоминаем объём
          wizard.calculatedCapHeads = backCalculateCapacity(wizard.enteredVolume, wizard.testDurationSec);
          wizard.headsTestVolume = wizard.enteredVolume;
          Serial.printf("[Calib] BODY NC heads capacity: %.1f ml/min (vol=%.0f ml, %d sec)\n", 
            wizard.calculatedCapHeads, wizard.enteredVolume, wizard.testDurationSec);
          
          // Переходим к capacity на скорости тела
          wizard.step = CalibStep::CAPACITY_BODY;
          wizard.enteredVolume = 0.0f;
          currentState = CalibState::WIZARD_CAP_BODY;
        } else {
          // HEADS или BODY NO: финальный расчёт
          wizard.calculatedCapacity = backCalculateCapacity(wizard.enteredVolume, wizard.testDurationSec);
          Serial.printf("[Calib] %s capacity: %.1f ml/min (vol=%.0f ml, %d sec, cycling=%d)\n", 
            getValveName(wizard.valve), wizard.calculatedCapacity, wizard.enteredVolume, 
            wizard.testDurationSec, wizard.testIsCycling ? 1 : 0);
          saveCapacity();
          wizard.step = CalibStep::RESULT;
          currentState = CalibState::WIZARD_RESULT;
        }
        display();
        break;
        
      case CalibState::WIZARD_INPUT2:
        // BODY NC: второй тест (тело) завершён
        wizard.calculatedCapBody = backCalculateCapacity(wizard.enteredVolume, wizard.testDurationSec);
        Serial.printf("[Calib] BODY NC body capacity: %.1f ml/min (vol=%.0f ml, %d sec)\n", 
          wizard.calculatedCapBody, wizard.enteredVolume, wizard.testDurationSec);
        saveCapacity();
        wizard.step = CalibStep::RESULT;
        currentState = CalibState::WIZARD_RESULT;
        display();
        break;
        
      case CalibState::WIZARD_RESULT:
        currentState = CalibState::MENU_MAIN;
        selectedItem = 0;
        display();
        break;
    }
  }
  
  void handleBackButton() {
    if (wizard.isTestRunning) {
      // Останавливаем тест
      closeValveForTest();
      wizard.isTestRunning = false;
      display();
      return;
    }
    
    switch(currentState) {
      case CalibState::WIZARD_RESULT:
        currentState = CalibState::WIZARD_INPUT2;
        if (!isTwoPointCalibration()) {
          currentState = CalibState::WIZARD_INPUT;
        }
        display();
        break;
        
      case CalibState::WIZARD_INPUT2:
        currentState = CalibState::WIZARD_CAP_BODY;
        display();
        break;
        
      case CalibState::WIZARD_INPUT:
        if (isTwoPointCalibration() && wizard.step == CalibStep::CAPACITY_HEADS) {
          currentState = CalibState::WIZARD_CAP_HEADS;
        } else {
          currentState = CalibState::WIZARD_CAPACITY;
        }
        display();
        break;
        
      case CalibState::WIZARD_CAP_BODY:
        currentState = CalibState::WIZARD_INPUT;
        display();
        break;
        
      case CalibState::WIZARD_CAP_HEADS:
      case CalibState::WIZARD_CAPACITY:
      case CalibState::WIZARD_DRY_RUN:
        currentState = CalibState::MENU_MAIN;
        display();
        break;
        
      case CalibState::MENU_MAIN:
        exitConfirmed = true;
        break;
        
      default:
        currentState = CalibState::MENU_MAIN;
        display();
        break;
    }
  }
  
  bool isReadyToExit() {
    return exitConfirmed;
  }
  
  void resetExitFlag() {
    exitConfirmed = false;
  }
  
  // === ОБНОВЛЕНИЕ (вызывается из loop) ===
  void update() {
    if (wizard.isTestRunning) {
      unsigned long elapsed = (millis() - wizard.testStartTime) / 1000;
      
      if (elapsed >= (unsigned long)wizard.testDurationSec) {
        // Тест завершён
        closeValveForTest();
        wizard.isTestRunning = false;
        
        switch(wizard.step) {
          case CalibStep::DRY_RUN: {
            // Dry run завершён, определяем следующий шаг
            if (isTwoPointCalibration()) {
              // BODY NC: → capacity на скорости голов
              wizard.step = CalibStep::CAPACITY_HEADS;
              currentState = CalibState::WIZARD_CAP_HEADS;
            } else {
              // HEADS / BODY NO: → capacity
              wizard.step = CalibStep::CAPACITY;
              currentState = CalibState::WIZARD_CAPACITY;
            }
            break;
          }
          
          case CalibStep::CAPACITY_HEADS: {
            // BODY NC: capacity голов завершён → ввод объёма
            wizard.step = CalibStep::INPUT_VOLUME;
            currentState = CalibState::WIZARD_INPUT;
            // Предзаполнение из текущего значения
            SystemConfig& cfg = config->getConfig();
            wizard.enteredVolume = (float)cfg.valve_body_capacity_heads > 0 
              ? (float)cfg.valve_body_capacity_heads : (float)cfg.valve_body_capacity;
            break;
          }
          
          case CalibStep::CAPACITY: {
            // HEADS / BODY NO: capacity завершён → ввод объёма
            wizard.step = CalibStep::INPUT_VOLUME;
            currentState = CalibState::WIZARD_INPUT;
            // Предзаполнение из текущего значения
            SystemConfig& cfg = config->getConfig();
            int currentCap = 0;
            switch(wizard.valve) {
              case CalibValve::HEADS: currentCap = cfg.valve_head_capacity; break;
              case CalibValve::BODY_NC: currentCap = cfg.valve_body_capacity; break;
              case CalibValve::BODY_NO: currentCap = cfg.valve0_body_capacity; break;
            }
            wizard.enteredVolume = (float)currentCap;
            break;
          }
          
          case CalibStep::CAPACITY_BODY: {
            // BODY NC: capacity тела завершён → ввод объёма (2-й раз)
            wizard.step = CalibStep::INPUT_VOLUME2;
            currentState = CalibState::WIZARD_INPUT2;
            // Предзаполнение из текущего значения
            SystemConfig& cfg = config->getConfig();
            wizard.enteredVolume = (float)cfg.valve_body_capacity;
            break;
          }
          
          default:
            break;
        }
        display();
      } else {
        // Обновляем таймер на экране
        char buf[16];
        sprintf(buf, "%03d / %03d sec", (int)elapsed, wizard.testDurationSec);
        lcd->setCursor(0, 3);
        lcd->print(buf);
      }
    }
  }
  
  // === МЕТОД ДЛЯ ЗАПУСКА ИЗ ПРОЦЕССА ===
  void startFromProcess(CalibValve valve) {
    selectedItem = (int)valve;
    startWizard(valve);
    wizard.launchedByProcess = true;
  }
  
  // === МЕТОД ДЛЯ ВВОДА ОБЪЁМА ИЗ WEB ===
  void setVolumeFromWeb(float volume) {
    if (isTwoPointCalibration() && wizard.step == CalibStep::CAPACITY_HEADS) {
      // BODY NC: первый ввод (головы)
      wizard.calculatedCapHeads = backCalculateCapacity(volume, wizard.testDurationSec);
      wizard.headsTestVolume = volume;
      Serial.printf("[Calib] Web BODY NC heads capacity: %.1f ml/min\n", wizard.calculatedCapHeads);
      
      // Переходим к capacity на скорости тела
      wizard.step = CalibStep::CAPACITY_BODY;
      wizard.enteredVolume = 0.0f;
      currentState = CalibState::WIZARD_CAP_BODY;
    } else if (isTwoPointCalibration() && wizard.step == CalibStep::CAPACITY_BODY) {
      // BODY NC: второй ввод (тело)
      wizard.calculatedCapBody = backCalculateCapacity(volume, wizard.testDurationSec);
      Serial.printf("[Calib] Web BODY NC body capacity: %.1f ml/min\n", wizard.calculatedCapBody);
      saveCapacity();
      wizard.step = CalibStep::RESULT;
      currentState = CalibState::WIZARD_RESULT;
    } else {
      // HEADS или BODY NO: финальный расчёт
      wizard.calculatedCapacity = backCalculateCapacity(volume, wizard.testDurationSec);
      saveCapacity();
      wizard.step = CalibStep::RESULT;
      currentState = CalibState::WIZARD_RESULT;
    }
  }
  
  // === МЕТОДЫ ДЛЯ ЗАПУСКА ТЕСТА ИЗ WEB ===
  
  // Начать dry run (100% open) из Web
  // valveNum: 1=heads, 2=body_nc, 3=body_no
  bool startDryRunFromWeb(int valveNum) {
    if (wizard.isTestRunning) return false;
    
    // Сброс состояния
    wizard.isTestRunning = false;
    wizard.enteredVolume = 0.0f;
    wizard.headsTestVolume = 0.0f;
    wizard.calculatedCapacity = 0.0f;
    wizard.calculatedCapHeads = 0.0f;
    wizard.calculatedCapBody = 0.0f;
    wizard.testDurationSec = 0;
    wizard.testStartTime = 0;
    wizard.testOpenMs = 0;
    wizard.testCloseMs = 0;
    wizard.testIsCycling = false;
    wizard.launchedByProcess = false;
    
    wizard.valve = (valveNum == 1) ? CalibValve::HEADS : 
                   (valveNum == 2) ? CalibValve::BODY_NC : CalibValve::BODY_NO;
    
    SystemConfig& cfg = config->getConfig();
    int drySec = cfg.calibDrySec > 0 ? cfg.calibDrySec : 10;
    
    wizard.step = CalibStep::DRY_RUN;
    currentState = CalibState::WIZARD_DRY_RUN;
    wizard.testDurationSec = drySec;
    wizard.isTestRunning = true;
    wizard.testStartTime = millis();
    wizard.launchedByProcess = false;
    
    openValveForTest();
    Serial.printf("[Calib] Web DRY: valve=%d, %d sec\n", valveNum, drySec);
    return true;
  }
  
  // Начать capacity тест из Web (для HEADS и BODY NO)
  bool startCapacityFromWeb(int valveNum) {
    if (wizard.isTestRunning) return false;
    
    SystemConfig& cfg = config->getConfig();
    int capSec = cfg.calibCapacitySec > 0 ? cfg.calibCapacitySec : 60;
    
    wizard.testDurationSec = capSec;
    wizard.isTestRunning = true;
    wizard.testStartTime = millis();
    wizard.launchedByProcess = false;
    
    if (needsCyclingForCapacity()) {
      // HEADS: импульс на скорости голов
      wizard.step = CalibStep::CAPACITY;
      currentState = CalibState::WIZARD_CAPACITY;
      startCyclingForTest(getHeadsTargetSpeed());
      Serial.printf("[Calib] Web CAPACITY (cycling): valve=%d, %d sec\n", valveNum, capSec);
    } else {
      // BODY NO: 100% open
      wizard.step = CalibStep::CAPACITY;
      currentState = CalibState::WIZARD_CAPACITY;
      wizard.testIsCycling = false;
      openValveForTest();
      Serial.printf("[Calib] Web CAPACITY (100%%): valve=%d, %d sec\n", valveNum, capSec);
    }
    return true;
  }
  
  // Начать capacity на скорости голов (BODY NC) из Web
  bool startCapHeadsFromWeb(int valveNum) {
    if (wizard.isTestRunning) return false;
    
    SystemConfig& cfg = config->getConfig();
    int capSec = cfg.calibCapacitySec > 0 ? cfg.calibCapacitySec : 60;
    
    wizard.step = CalibStep::CAPACITY_HEADS;
    currentState = CalibState::WIZARD_CAP_HEADS;
    wizard.testDurationSec = capSec;
    wizard.isTestRunning = true;
    wizard.testStartTime = millis();
    wizard.launchedByProcess = false;
    
    startCyclingForTest(getHeadsTargetSpeed());
    Serial.printf("[Calib] Web CAP_HEADS: valve=%d, %d sec\n", valveNum, capSec);
    return true;
  }
  
  // Начать capacity на скорости тела (BODY NC) из Web
  bool startCapBodyFromWeb(int valveNum) {
    if (wizard.isTestRunning) return false;
    
    SystemConfig& cfg = config->getConfig();
    int capSec = cfg.calibCapacitySec > 0 ? cfg.calibCapacitySec : 60;
    
    wizard.step = CalibStep::CAPACITY_BODY;
    currentState = CalibState::WIZARD_CAP_BODY;
    wizard.testDurationSec = capSec;
    wizard.isTestRunning = true;
    wizard.testStartTime = millis();
    wizard.launchedByProcess = false;
    
    startCyclingForTest(getBodyTargetSpeed());
    Serial.printf("[Calib] Web CAP_BODY: valve=%d, %d sec\n", valveNum, capSec);
    return true;
  }
  
  // === МЕТОД ДЛЯ ОТМЕНЫ ТЕСТА ИЗ WEB ===
  void cancelCalibFromWeb() {
    if (wizard.isTestRunning) {
      closeValveForTest();
      wizard.isTestRunning = false;
    }
    wizard.step = CalibStep::IDLE;
    currentState = CalibState::MENU_MAIN;
    Serial.println("[Calib] Web cancelled");
  }
  
  // Совместимость со старым API
  bool startCalibFromWeb(int valveNum, int durationSec) {
    if (durationSec <= 15) {
      // Считаем это dry run
      return startDryRunFromWeb(valveNum);
    } else {
      // Capacity test
      return startCapacityFromWeb(valveNum);
    }
  }
};

#endif