#ifndef SET_PW_AS_MENU_H
#define SET_PW_AS_MENU_H

#include <LiquidCrystal_I2C.h>
#include "config.h"
#include "preferences.h"
#include "common.h"
#include <cstring>

class SetPwAsMenu {
private:
  LiquidCrystal_I2C* lcd;
  ConfigManager* config;
  
  int selectedItem = 0;
  EditMode editMode = EDIT_NONE;
  int tempEditValue = 0;
  bool runConfirmed = false; // Флаг подтверждения запуска

  // Пункты меню: 0=POWER, 1=AS_VOL, 2=GOLOVYRUN
  // Используем float* для связи с конфигом, но отображаем как int
  
public:
  SetPwAsMenu(LiquidCrystal_I2C* lcdPtr, ConfigManager* cfg) {
    lcd = lcdPtr;
    config = cfg;
  }

    void display() {
    runConfirmed = false; // Сброс при любой отрисовке
    lcd->clear();
    
    const char* title = "SET PW & AS";
    lcd->setCursor((20 - strlen(title))/2, 0);
    lcd->print(title);
    
    SystemConfig& cfg = config->getConfig();
    
    displayItem(1, "POWER", (int)cfg.power, "W", 0);
    displayItem(2, "AS_VOL", (int)cfg.asVolume, "ml", 1);
    displayAction(3, "GOLOVYRUN", 2);
  }

  void displayItem(int row, const char* label, int value, const char* unit, int index) {
    // Курсор
    lcd->setCursor(0, row);
    lcd->print(selectedItem == index ? ">" : " ");
    
    // Название
    lcd->print(label);
    
    // Значение (справа)
    char buf[12];
    if (editMode == EDIT_VALUE && selectedItem == index) {
      snprintf(buf, sizeof(buf), "<%d %s", tempEditValue, unit);
    } else {
      snprintf(buf, sizeof(buf), "%d %s", value, unit);
    }
    
    int len = strlen(buf);
    lcd->setCursor(20 - len, row);
    lcd->print(buf);
  }

  void displayAction(int row, const char* label, int index) {
    lcd->setCursor(0, row);
    lcd->print(selectedItem == index ? ">" : " ");
    lcd->print(label);
  }

  void handleUpButton() {
    if (editMode == EDIT_NONE) {
      selectedItem--;
      if (selectedItem < 0) selectedItem = 2;
      display();
    } else {
      // Редактирование
      SystemConfig& cfg = config->getConfig();
      if (selectedItem == 0) { // Power
        tempEditValue += 100;
        if (tempEditValue > 3500) tempEditValue = 3500;
      } else if (selectedItem == 1) { // AS_VOL
        tempEditValue += 100;
        if (tempEditValue > 10000) tempEditValue = 10000;
      }
      display();
    }
  }
  
  void handleDownButton() {
    if (editMode == EDIT_NONE) {
      selectedItem++;
      if (selectedItem > 2) selectedItem = 0;
      display();
    } else {
      // Редактирование
       if (selectedItem == 0) { // Power
        tempEditValue -= 100;
        if (tempEditValue < 100) tempEditValue = 100;
      } else if (selectedItem == 1) { // AS_VOL
        tempEditValue -= 100;
        if (tempEditValue < 1000) tempEditValue = 1000;
      }
      display();
    }
  }
  
   void handleSetButton() {
    if (editMode == EDIT_NONE) {
      if (selectedItem == 2) {
        // Нажали SET на GOLOVYRUN
        runConfirmed = true;
      } else {
        // Вход в редактирование
        SystemConfig& cfg = config->getConfig();
        if (selectedItem == 0) tempEditValue = (int)cfg.power;
        if (selectedItem == 1) tempEditValue = (int)cfg.asVolume;
        editMode = EDIT_VALUE;
        display();
      }
    } else {
      // Сохранение
      SystemConfig& cfg = config->getConfig();
      if (selectedItem == 0) cfg.power = (float)tempEditValue;
      if (selectedItem == 1) cfg.asVolume = (float)tempEditValue;
      
      config->saveRectConfig();
      editMode = EDIT_NONE;
      display();
    }
  }
  
  void handleBackButton() {
    if (editMode != EDIT_NONE) {
      editMode = EDIT_NONE;
      display();
    }
  }
  
  // Сигнал готовности к переходу (выбран GOLOVYRUN и нажат SET)
  bool isReadyToRun() {
    return runConfirmed;
  }
};

#endif