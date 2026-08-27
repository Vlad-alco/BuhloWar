#ifndef VALVE_CAL_MENU_H
#define VALVE_CAL_MENU_H

#include <LiquidCrystal_I2C.h>
#include "config.h"
#include "preferences.h"
#include "common.h"
#include "OutputManager.h"
#include <cstring>

// === КОНСТАНТЫ КАЛИБРОВКИ ===
// Фиксированный цикл для замера: 1 сек открыт / 1 сек закрыт
// За 60 сек = 30 импульсов, клапан открыт суммарно 30 сек
// duty = 0.5, capacity = volume * 60 / (0.5 * 60) = volume * 2
const int CALIB_CYCLE_OPEN_MS = 1000;   // 1 сек открыт
const int CALIB_CYCLE_CLOSE_MS = 1000;  // 1 сек закрыт
// =====================================

// === ЭТАПЫ МАСТЕРА КАЛИБРОВКИ ===
enum class CalibState {
  MENU_MAIN,           // Выбор клапана: HEADS / BODY NC / BODY NO / EXIT
  WIZARD_DRY_RUN,      // Шаг 1/2: Пролив системы (10 сек, 100% open)
  WIZARD_CAPACITY,     // Шаг 2/2: Замер (цикл 1с open / 1с close, 60 сек)
  WIZARD_INPUT,        // Ввод измеренного объёма
  WIZARD_RESULT        // Результат калибровки
};

enum class CalibValve {
  HEADS,
  BODY_NC,
  BODY_NO
};

enum class CalibStep {
  IDLE,           // 0
  DRY_RUN,        // 1
  CAPACITY,       // 2
  INPUT_VOLUME,   // 3
  RESULT          // 4
};

// === СОСТОЯНИЕ МАСТЕРА КАЛИБРОВКИ ===
struct CalibWizardState {
  CalibValve valve = CalibValve::HEADS;    // HEADS / BODY_NC / BODY_NO
  CalibStep step = CalibStep::IDLE;
  bool launchedByProcess = false;           // true = авто (LCD+Web sync), false = ручной Web only
  bool isTestRunning = false;
  unsigned long testStartTime = 0;
  int testDurationSec = 0;                  // 10 или 60
  float enteredVolume = 0.0f;               // мл (введённый пользователем)
  float calculatedCapacity = 0.0f;          // мл/мин (рассчитанный результат)
  int testOpenMs = 0;                       // openMs использованный в тесте
  int testCloseMs = 0;                      // closeMs использованный в тесте
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
    wizard.calculatedCapacity = 0.0f;
    wizard.testOpenMs = 0;
    wizard.testCloseMs = 0;
    wizard.testIsCycling = false;
    currentState = CalibState::WIZARD_DRY_RUN;
    display();
  }
  
  // Открыть клапан на 100% (для dry run)
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
  
  // Закрыть клапан и остановить cycling
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
  
  // Запустить циклический тест: фиксированный 1с open / 1с close
  // duty = 0.5, за 60 сек клапан открыт 30 сек
  void startCyclingForTest() {
    wizard.testOpenMs = CALIB_CYCLE_OPEN_MS;
    wizard.testCloseMs = CALIB_CYCLE_CLOSE_MS;
    wizard.testIsCycling = true;
    
    switch(wizard.valve) {
      case CalibValve::HEADS:
        output->startHeadValveCycling(CALIB_CYCLE_OPEN_MS, CALIB_CYCLE_CLOSE_MS);
        Serial.printf("[Calib] Heads cycling: %dms/%dms (duty=0.5)\n", CALIB_CYCLE_OPEN_MS, CALIB_CYCLE_CLOSE_MS);
        break;
      case CalibValve::BODY_NC:
      case CalibValve::BODY_NO:
        output->startBodyValveCycling(CALIB_CYCLE_OPEN_MS, CALIB_CYCLE_CLOSE_MS);
        Serial.printf("[Calib] Body cycling: %dms/%dms (duty=0.5)\n", CALIB_CYCLE_OPEN_MS, CALIB_CYCLE_CLOSE_MS);
        break;
    }
  }
  
  // Расчёт capacity по измеренному объёму
  // Для cycling: capacity = volume * 60 / (duty * duration)
  // При duty=0.5, duration=60: capacity = volume * 2
  float calculateCapacity(float volumeMl, int durationSec) {
    if (durationSec <= 0 || volumeMl <= 0) return 0.0f;
    
    if (wizard.testIsCycling && wizard.testOpenMs > 0 && wizard.testCloseMs > 0) {
      float dutyCycle = (float)wizard.testOpenMs / (float)(wizard.testOpenMs + wizard.testCloseMs);
      return volumeMl * 60.0f / (dutyCycle * (float)durationSec);
    } else {
      // 100% open (не используется при калибровке, но оставлен для совместимости)
      return volumeMl / ((float)durationSec / 60.0f);
    }
  }
  
  // Сохранить результат калибровки в config
  void saveCapacity() {
    SystemConfig& cfg = config->getConfig();
    
    switch(wizard.valve) {
      case CalibValve::HEADS: {
        int cap = (int)(wizard.calculatedCapacity + 0.5f);
        cfg.valve_head_capacity = cap;
        Serial.printf("[Calib] Save HEADS capacity: %d ml/min\n", cap);
        break;
      }
      case CalibValve::BODY_NC: {
        int cap = (int)(wizard.calculatedCapacity + 0.5f);
        cfg.valve_body_capacity = cap;
        Serial.printf("[Calib] Save BODY NC capacity: %d ml/min\n", cap);
        break;
      }
      case CalibValve::BODY_NO: {
        int cap = (int)(wizard.calculatedCapacity + 0.5f);
        cfg.valve0_body_capacity = cap;
        Serial.printf("[Calib] Save BODY NO capacity: %d ml/min\n", cap);
        break;
      }
    }
    config->saveRectConfig();
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
      case CalibState::WIZARD_INPUT: displayInput(); break;
      case CalibState::WIZARD_RESULT: displayResult(); break;
    }
  }
  
  void displayMainMenu() {
    lcd->setCursor(4, 0);
    lcd->print("VALVE CALIBRATION");
    
    const char* items[] = {"HEADS", "BODY NC", "BODY NO", "EXIT"};
    SystemConfig& cfg = config->getConfig();
    
    for (int i = 0; i < 4; i++) {
      lcd->setCursor(0, i);
      lcd->print(i == selectedItem ? ">" : " ");
      lcd->print(items[i]);
      
      if (i < 3) {
        lcd->print(" ");
        if (i == 0) {
          lcd->print(cfg.valve_head_capacity);
          lcd->print("ml/m");
        } else if (i == 1) {
          lcd->print(cfg.valve_body_capacity);
          lcd->print("ml/m");
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
    lcd->print(" Step 1/2: Flush");
    
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
    lcd->setCursor(0, 0);
    lcd->print("VALVE CALIBRATION");
    
    lcd->setCursor(0, 1);
    lcd->print(getValveName(wizard.valve));
    lcd->print(" Step 2/2: Meas");
    
    if (wizard.isTestRunning) {
      lcd->setCursor(0, 2);
      lcd->print("Pulse: 1s/1s");
      
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
  
  void displayInput() {
    lcd->setCursor(0, 0);
    lcd->print("ENTER VOLUME");
    
    lcd->setCursor(0, 1);
    lcd->print("[");
    lcd->print((int)wizard.enteredVolume);
    lcd->print("]");
    lcd->print(" ml");
    
    lcd->setCursor(0, 2);
    lcd->print("UP/DOWN +/-1");
    
    lcd->setCursor(0, 3);
    lcd->print("SET-confirm");
  }
  
  void displayResult() {
    lcd->setCursor(0, 0);
    lcd->print("CALIBRATION DONE");
    
    lcd->setCursor(0, 1);
    lcd->print("Cap: ");
    lcd->print(wizard.calculatedCapacity, 1);
    lcd->print(" ml/min");
    
    lcd->setCursor(0, 2);
    lcd->print("Cycle: 1s/1s (0.5)");
    
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
        
      case CalibState::WIZARD_DRY_RUN: {
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
        break;
      }
      
      case CalibState::WIZARD_CAPACITY: {
        // Начинаем замер — цикл 1с/1с
        SystemConfig& cfgCap = config->getConfig();
        wizard.testDurationSec = cfgCap.calibCapacitySec > 0 ? cfgCap.calibCapacitySec : 60;
        wizard.isTestRunning = true;
        wizard.testStartTime = millis();
        startCyclingForTest();
        display();
        break;
      }
      
      case CalibState::WIZARD_INPUT: {
        // Расчёт и сохранение
        wizard.calculatedCapacity = calculateCapacity(wizard.enteredVolume, wizard.testDurationSec);
        Serial.printf("[Calib] %s capacity: %.1f ml/min (vol=%.0f ml, %d sec)\n", 
          getValveName(wizard.valve), wizard.calculatedCapacity, wizard.enteredVolume, wizard.testDurationSec);
        saveCapacity();
        wizard.step = CalibStep::RESULT;
        currentState = CalibState::WIZARD_RESULT;
        display();
        break;
      }
      
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
        currentState = CalibState::WIZARD_INPUT;
        display();
        break;
        
      case CalibState::WIZARD_INPUT:
        currentState = CalibState::WIZARD_CAPACITY;
        display();
        break;
        
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
            // Пролив завершён → замер (цикл 1с/1с)
            wizard.step = CalibStep::CAPACITY;
            currentState = CalibState::WIZARD_CAPACITY;
            break;
          }
          
          case CalibStep::CAPACITY: {
            // Замер завершён → ввод объёма
            wizard.step = CalibStep::INPUT_VOLUME;
            currentState = CalibState::WIZARD_INPUT;
            // Предзаполнение из текущего значения capacity
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
  // Вызывается при CALIB_SET_VOLUME — единый для всех клапанов
  void setVolumeFromWeb(float volume) {
    wizard.calculatedCapacity = calculateCapacity(volume, wizard.testDurationSec);
    Serial.printf("[Calib] Web %s capacity: %.1f ml/min (vol=%.0f ml, %d sec)\n", 
      getValveName(wizard.valve), wizard.calculatedCapacity, volume, wizard.testDurationSec);
    saveCapacity();
    wizard.step = CalibStep::RESULT;
    currentState = CalibState::WIZARD_RESULT;
  }
  
  // === МЕТОДЫ ДЛЯ ЗАПУСКА ТЕСТА ИЗ WEB ===
  
  // Начать dry run (100% open) из Web
  // valveNum: 1=heads, 2=body_nc, 3=body_no
  bool startDryRunFromWeb(int valveNum) {
    if (wizard.isTestRunning) return false;
    
    // Сброс состояния
    wizard.isTestRunning = false;
    wizard.enteredVolume = 0.0f;
    wizard.calculatedCapacity = 0.0f;
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
    
    openValveForTest();
    Serial.printf("[Calib] Web DRY: valve=%d, %d sec\n", valveNum, drySec);
    return true;
  }
  
  // Начать capacity тест из Web (цикл 1с/1с для всех клапанов)
  bool startCapacityFromWeb(int valveNum) {
    if (wizard.isTestRunning) return false;
    
    SystemConfig& cfg = config->getConfig();
    int capSec = cfg.calibCapacitySec > 0 ? cfg.calibCapacitySec : 60;
    
    wizard.step = CalibStep::CAPACITY;
    currentState = CalibState::WIZARD_CAPACITY;
    wizard.testDurationSec = capSec;
    wizard.isTestRunning = true;
    wizard.testStartTime = millis();
    wizard.launchedByProcess = false;
    
    startCyclingForTest();
    Serial.printf("[Calib] Web CAPACITY (1s/1s): valve=%d, %d sec\n", valveNum, capSec);
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
      return startDryRunFromWeb(valveNum);
    } else {
      return startCapacityFromWeb(valveNum);
    }
  }
};

#endif
