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
  WIZARD_DRY_RUN,      // Шаг 1: Пролив системы (10 сек)
  WIZARD_CAPACITY,     // Шаг 2: Измерение capacity (60 сек)
  WIZARD_INPUT,        // Ввод объёма (1 мл шаг)
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
  INPUT_VOLUME,  // Переименовано для избежания конфликта с INPUT из ESP32
  RESULT
};

// === СОСТОЯНИЕ МАСТЕРА КАЛИБРОВКИ ===
struct CalibWizardState {
  CalibValve valve = CalibValve::HEADS;    // HEADS / BODY_NC / BODY_NO
  CalibStep step = CalibStep::IDLE;         // IDLE / DRY_RUN / CAPACITY / INPUT_VOLUME / RESULT
  bool launchedByProcess = false;           // true = авто (LCD+Web sync), false = ручной Web only
  bool isTestRunning = false;
  unsigned long testStartTime = 0;
  int testDurationSec = 0;                  // 10 или 60
  float enteredVolume = 0.0f;               // мл (0.1 шаг)
  float calculatedCapacity = 0.0f;          // мл/мин
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
    wizard.launchedByProcess = true;  // Для вызова из процесса
    wizard.isTestRunning = false;
    wizard.enteredVolume = 0.0f;
    wizard.calculatedCapacity = 0.0f;
    currentState = CalibState::WIZARD_DRY_RUN;
    display();
  }
  
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
  
  void closeValve() {
    switch(wizard.valve) {
      case CalibValve::HEADS:
        output->closeHeadValve();
        break;
      case CalibValve::BODY_NC:
      case CalibValve::BODY_NO:
        output->closeBodyValve();
        break;
    }
  }
  
  void saveCapacity() {
    SystemConfig& cfg = config->getConfig();
    int capacityInt = (int)(wizard.calculatedCapacity + 0.5f);  // Округление
    
    switch(wizard.valve) {
      case CalibValve::HEADS:
        cfg.valve_head_capacity = capacityInt;
        break;
      case CalibValve::BODY_NC:
        cfg.valve_body_capacity = capacityInt;
        break;
      case CalibValve::BODY_NO:
        cfg.valve0_body_capacity = capacityInt;
        break;
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
    return (elapsed < wizard.testDurationSec) ? (wizard.testDurationSec - elapsed) : 0;
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
    // Заголовок
    lcd->setCursor(4, 0);
    lcd->print("VALVE CALIBRATION");
    
    // Пункты меню
    const char* items[] = {"HEADS", "BODY NC", "BODY NO", "EXIT"};
    
    for (int i = 0; i < 4; i++) {
      lcd->setCursor(0, i + 0);  // Сдвигаем меню на строку выше
      lcd->print(i == selectedItem ? ">" : " ");
      lcd->print(items[i]);
      
      // Показываем текущий capacity для каждого клапана
      SystemConfig& cfg = config->getConfig();
      if (i < 3) {
        lcd->print(" ");
        int cap = 0;
        if (i == 0) cap = cfg.valve_head_capacity;
        else if (i == 1) cap = cfg.valve_body_capacity;
        else cap = cfg.valve0_body_capacity;
        lcd->print(cap);
        lcd->print("ml/m");
      }
    }
  }
  
  void displayDryRun() {
    // Шаг 1/2: Пролив системы
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
    // Шаг 2/2: Измерение capacity
    lcd->setCursor(0, 0);
    lcd->print("VALVE CALIBRATION");
    
    lcd->setCursor(0, 1);
    lcd->print(getValveName(wizard.valve));
    lcd->print(" Step 2/2: Cap.");
    
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
      lcd->print("Place measuring cup");
      
      lcd->setCursor(0, 3);
      lcd->print("SET-start");
    }
  }
  
  void displayInput() {
    // Ввод объёма (целое число, шаг 1 мл)
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
    // Результат калибровки
    lcd->setCursor(0, 0);
    lcd->print("CALIBRATION DONE");
    
    lcd->setCursor(0, 1);
    lcd->print("Cap: ");
    lcd->print(wizard.calculatedCapacity, 1);
    lcd->print(" ml/min");
    
    lcd->setCursor(0, 2);
    SystemConfig& cfg = config->getConfig();
    lcd->print("minOpen: ");
    lcd->print(cfg.minOpenTime);
    lcd->print("ms (def)");
    
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
        
      case CalibState::WIZARD_DRY_RUN:
        if (!wizard.isTestRunning) {
          // Начинаем dry run (10 сек)
          wizard.testDurationSec = 10;
          wizard.isTestRunning = true;
          wizard.testStartTime = millis();
          openValveForTest();
          display();
        }
        break;
        
      case CalibState::WIZARD_CAPACITY:
        if (!wizard.isTestRunning) {
          // Начинаем тест capacity (60 сек)
          wizard.testDurationSec = 60;
          wizard.isTestRunning = true;
          wizard.testStartTime = millis();
          openValveForTest();
          display();
        }
        break;
        
      case CalibState::WIZARD_INPUT:
        // Расчёт capacity
        // capacity = объём_мл / время_мин
        wizard.calculatedCapacity = wizard.enteredVolume / (wizard.testDurationSec / 60.0f);
        saveCapacity();
        currentState = CalibState::WIZARD_RESULT;
        display();
        break;
        
      case CalibState::WIZARD_RESULT:
        // Переход к следующему клапану или выход
        currentState = CalibState::MENU_MAIN;
        selectedItem = 0;
        display();
        break;
    }
  }
  
  void handleBackButton() {
    if (wizard.isTestRunning) {
      // Останавливаем тест
      closeValve();
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
      
      if (elapsed >= wizard.testDurationSec) {
        // Тест завершён
        closeValve();
        wizard.isTestRunning = false;
        
        if (wizard.step == CalibStep::DRY_RUN) {
          // Dry run завершён, переходим к capacity
          wizard.step = CalibStep::CAPACITY;
          currentState = CalibState::WIZARD_CAPACITY;
        } else {
          // Capacity завершён, переходим к вводу объёма
          wizard.step = CalibStep::INPUT_VOLUME;
          currentState = CalibState::WIZARD_INPUT;
          // Инициализируем объём из текущего значения cap в настройках
          SystemConfig& cfg = config->getConfig();
          int currentCap = 0;
          switch(wizard.valve) {
            case CalibValve::HEADS: currentCap = cfg.valve_head_capacity; break;
            case CalibValve::BODY_NC: currentCap = cfg.valve_body_capacity; break;
            case CalibValve::BODY_NO: currentCap = cfg.valve0_body_capacity; break;
          }
          wizard.enteredVolume = (float)currentCap;
        }
        display();
      } else {
        unsigned long elapsed = (millis() - wizard.testStartTime) / 1000;
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
    wizard.enteredVolume = volume;
    wizard.calculatedCapacity = volume / (wizard.testDurationSec / 60.0f);
    saveCapacity();
    wizard.step = CalibStep::INPUT_VOLUME;
    currentState = CalibState::WIZARD_RESULT;
  }
  
  // === МЕТОД ДЛЯ ЗАПУСКА ТЕСТА ИЗ WEB ===
  // valveNum: 1=heads, 2=body_nc, 3=body_no
  // durationSec: 10 (dry run) или 60 (capacity)
  bool startCalibFromWeb(int valveNum, int durationSec) {
    if (wizard.isTestRunning) return false;  // Уже идёт тест
    
    // === ВАЖНО: Полный сброс состояния перед новым тестом ===
    wizard.isTestRunning = false;
    wizard.enteredVolume = 0.0f;
    wizard.calculatedCapacity = 0.0f;
    wizard.testDurationSec = 0;
    wizard.testStartTime = 0;
    wizard.launchedByProcess = false;
    
    // Устанавливаем клапан
    wizard.valve = (valveNum == 1) ? CalibValve::HEADS : 
                   (valveNum == 2) ? CalibValve::BODY_NC : CalibValve::BODY_NO;
    
    // Устанавливаем шаг
    if (durationSec == 10) {
      wizard.step = CalibStep::DRY_RUN;
      currentState = CalibState::WIZARD_DRY_RUN;
    } else {
      wizard.step = CalibStep::CAPACITY;
      currentState = CalibState::WIZARD_CAPACITY;
    }
    
    // Запускаем тест
    wizard.testDurationSec = durationSec;
    wizard.isTestRunning = true;
    wizard.testStartTime = millis();
    wizard.launchedByProcess = false;  // Запущен из Web
    
    openValveForTest();
    Serial.printf("[Calib] Web started: valve=%d, duration=%d sec\n", valveNum, durationSec);
    return true;
  }
  
  // === МЕТОД ДЛЯ ОТМЕНЫ ТЕСТА ИЗ WEB ===
  void cancelCalibFromWeb() {
    if (wizard.isTestRunning) {
      closeValve();
      wizard.isTestRunning = false;
    }
    wizard.step = CalibStep::IDLE;
    currentState = CalibState::MENU_MAIN;
    Serial.println("[Calib] Web cancelled");
  }
};

#endif
