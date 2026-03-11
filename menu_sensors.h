#ifndef MENU_SENSORS_H
#define MENU_SENSORS_H

#include <LiquidCrystal_I2C.h>
#include "config.h"
#include "common.h"
#include "SensorManager.h"

class ConfigManager;

class SensorsMenu {
private:
  LiquidCrystal_I2C* lcd;
  ConfigManager* config;
  AppState* appState;
  
  int selectedItem = 0;
  bool isCalibrating = false; // Флаг: идет процесс калибровки
  int calibItemIndex = -1;     // Какой датчик сейчас калибруется
  
  unsigned long lastAnimationUpdate = 0;
  int animationFrame = 0;
  
public:
  SensorsMenu(LiquidCrystal_I2C* lcdPtr, ConfigManager* cfg, AppState* statePtr) {
    lcd = lcdPtr;
    config = cfg;
    appState = statePtr;
  }
  
  void display() {
    lcd->clear();
    updateSensorDisplay();
    
    SensorManager* sm = SensorManager::getInstance();
    
    for (int i = 0; i < 4; i++) {
      lcd->setCursor(0, i);
      
      // Курсор выбора
      if (i == selectedItem) lcd->print(">");
      else lcd->print(" ");
      
      // Имя датчика
      const char* names[] = {"TSA ", "AQUA", "TSAR", "TANK"};
      lcd->print(names[i]);
      lcd->print(" ");
      
      // Статус
      // Если этот датчик сейчас калибруется
      if (isCalibrating && i == calibItemIndex) {
          lcd->print("heat up"); 
          
          // Анимация (опционально, если места хватает)
          // lcd->print("."); 
      } 
      // Если датчик откалиброван
      else if (sm->isCalibrated((SensorIndex)i)) {
          float t = sm->getTemperature((SensorIndex)i);
          if (t > -100) {
              lcd->print(t, 1);
              lcd->print("C");
          } else {
              lcd->print("NO DATA");
          }
      } 
      // Не калиброван
      else {
          lcd->print("NOT CALIB");
      }
    }
  }
  
  void handleUpButton() {
    if (!isCalibrating) {
      selectedItem = (selectedItem - 1 + 4) % 4;
      display();
    }
  }
  
  void handleDownButton() {
    if (!isCalibrating) {
      selectedItem = (selectedItem + 1) % 4;
      display();
    }
  }
  
  void handleSetButton() {
    if (isCalibrating) return; // Уже калибруется, кнопка заблокирована
    
    SensorManager* sm = SensorManager::getInstance();
    
    // Запускаем калибровку для выбранного датчика
    calibItemIndex = selectedItem;
    if (sm->startCalibration((SensorIndex)calibItemIndex)) {
        isCalibrating = true;
        display(); // Сразу обновим экран, чтобы появилось "heat up"
    }
  }
  
  void handleBackButton() {
    if (isCalibrating) {
        // Отмена калибровки
        SensorManager::getInstance()->cancelCalibration((SensorIndex)calibItemIndex);
        isCalibrating = false;
        calibItemIndex = -1;
        display();
    } else {
        // Выход в главное меню
        *appState = STATE_MAIN_MENU;
        needMainMenuRedraw = true;
    }
  }
  
  void update() {
    // Обновление экрана раз в секунду
    unsigned long currentTime = millis();
    static unsigned long lastTempUpdate = 0;
    if (currentTime - lastTempUpdate > 1000) {
      lastTempUpdate = currentTime;
      
      // Если идет калибровка
      if (isCalibrating) {
          SensorManager* sm = SensorManager::getInstance();
          DeviceAddress foundAddr;
          
          if (sm->checkCalibrationDelta((SensorIndex)calibItemIndex, foundAddr)) {
              // НАЙДЕН! checkCalibrationDelta уже сохранил адрес
              isCalibrating = false;
              calibItemIndex = -1;
              display(); // Обновим, покажем температуру
          } 
          else if (!sm->isCalibrating((SensorIndex)calibItemIndex)) {
              // Таймаут или ошибка
              isCalibrating = false;
              // Показываем NOT FOUND на время (можно добавить состояние, но проще просто обновить экран)
              lcd->setCursor(8, selectedItem); // Позиция после имени
              lcd->print("NOT FND "); 
              delay(1500); // Ждем полторы секунды
              calibItemIndex = -1;
              display();
          }
      } 
      else if (*appState == STATE_SENSORS_MENU) {
        // Обычное обновление температур
        display();
      }
    }
  }
  
private:
  void updateSensorDisplay() {
    // Обновление не требуется, берем данные напрямую из SensorManager при отрисовке
  }
};

#endif