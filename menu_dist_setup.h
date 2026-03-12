#ifndef MENU_DIST_SETUP_H
#define MENU_DIST_SETUP_H

#include <LiquidCrystal_I2C.h>
#include "config.h"
#include "preferences.h"
#include "common.h"

class ConfigManager;

class DistSetupMenu {
private:
  LiquidCrystal_I2C* lcd;
  ConfigManager* config;
  
  int selectedItem = 0;
  int scrollOffset = 0;
  bool editing = false;
  int tempEditValue = 0;
  bool configChanged = false;
  
  struct SettingsItem {
    const char* name;
    int minVal;
    int maxVal;
    int step;
    const char* unit;
  };
  
  // Всего 9 параметров
  SettingsItem settingsItems[9] = {
    {"RAZGON", 30, 80, 1, "C"},       // 0
    {"BAKSTOP", 50, 99, 1, "C"},      // 1
    {"MIDTERM", 35, 96, 1, "C"},      // 2 (НОВЫЙ ПУНКТ)
    {"NAGREV", 0, 1, 1, ""},          // 3 (0=TEN, 1=GAS/Индукция)
    {"FULL PWR", 0, 1, 1, ""},        // 4 (Bool)
    {"VALVE USE", 0, 1, 1, ""},       // 5 (Bool)
    {"MIXER EN", 0, 1, 1, ""},        // 6 (Bool)
    {"MIXER ON", 1, 300, 5, "s"},     // 7 (Секунды)
    {"MIXER OFF", 1, 300, 5, "s"}     // 8 (Секунды)
  };
  
  int getCurrentValue(int itemIndex) {
    SystemConfig& cfg = config->getConfig();
    switch(itemIndex) {
      case 0: return cfg.razgonTemp;
      case 1: return cfg.bakStopTemp;
      case 2: return cfg.midterm;      // НОВЫЙ CASE
      case 3: return cfg.heaterType;
      case 4: return cfg.fullPwr ? 1 : 0;
      case 5: return cfg.valveuse ? 1 : 0;
      case 6: return cfg.mixerEnabled ? 1 : 0;
      case 7: return cfg.mixerOnTime;
      case 8: return cfg.mixerOffTime;
      default: return 0;
    }
  }
  
  void setCurrentValue(int itemIndex, int value) {
    SystemConfig& cfg = config->getConfig();
    switch(itemIndex) {
      case 0: cfg.razgonTemp = value; break;
      case 1: cfg.bakStopTemp = value; break;
      case 2: cfg.midterm = value; break;      // НОВЫЙ CASE
      case 3: cfg.heaterType = value; break;
      case 4: cfg.fullPwr = (value == 1); break;
      case 5: cfg.valveuse = (value == 1); break;
      case 6: cfg.mixerEnabled = (value == 1); break;
      case 7: cfg.mixerOnTime = value; break;
      case 8: cfg.mixerOffTime = value; break;
    }
  }
  
public:
  DistSetupMenu(LiquidCrystal_I2C* lcdPtr, ConfigManager* cfg) {
    lcd = lcdPtr;
    config = cfg;
  }
  
  void display() {
    lcd->clear();
    
    for (int i = 0; i < 4; i++) {
      int itemIndex = scrollOffset + i;
      
      if (itemIndex < 9) { // Всего 9 пунктов
        lcd->setCursor(0, i);
        
        if (itemIndex == selectedItem) {
          lcd->print(">");
        } else {
          lcd->print(" ");
        }
        
        lcd->print(settingsItems[itemIndex].name);
        lcd->print(":");
        
        if (editing && itemIndex == selectedItem) {
          // Режим редактирования: [ЗНАЧЕНИЕ]
          lcd->print("[");
          // Для булевых выводим YES/NO внутри скобок
          if (isBoolItem(itemIndex)) {
             lcd->print(tempEditValue == 1 ? "YES" : "NO");
          } else {
             lcd->print(tempEditValue);
          }
          lcd->print("]");
        } else {
          // Обычный режим: ЗНАЧЕНИЕ ЕДИНИЦА
          int currentValue = getCurrentValue(itemIndex);
          lcd->print(" ");
          if (isBoolItem(itemIndex)) {
             lcd->print(currentValue == 1 ? "YES" : "NO");
          } else {
             lcd->print(currentValue);
             lcd->print(" ");
             lcd->print(settingsItems[itemIndex].unit);
          }
        }
      }
    }
  }
  
  // Вспомогательная функция определить булевый ли пункт
  bool isBoolItem(int index) {
    // item 4, 5, 6 - bool
    return (index == 4 || index == 5 || index == 6);
  }
  
  void handleUpButton() {
    if (editing) {
      tempEditValue += settingsItems[selectedItem].step;
      if (tempEditValue > settingsItems[selectedItem].maxVal) {
        tempEditValue = settingsItems[selectedItem].maxVal;
      }
      display();
    } else {
      selectedItem--;
      if (selectedItem < 0) selectedItem = 8; // Max index 8
      updateScroll();
      display();
    }
  }
  
  void handleDownButton() {
    if (editing) {
      tempEditValue -= settingsItems[selectedItem].step;
      if (tempEditValue < settingsItems[selectedItem].minVal) {
        tempEditValue = settingsItems[selectedItem].minVal;
      }
      display();
    } else {
      selectedItem++;
      if (selectedItem > 8) selectedItem = 0; // Max index 8
      updateScroll();
      display();
    }
  }
  
  void handleSetButton() {
    if (editing) {
      setCurrentValue(selectedItem, tempEditValue);
      configChanged = true;
      editing = false;
    } else {
      tempEditValue = getCurrentValue(selectedItem);
      editing = true;
    }
    display();
  }
  
  void handleBackButton() {
    if (editing) {
      editing = false;
      display();
    } else {
      if (configChanged) {
        config->saveDistConfig(); // Сохраняем при выходе
        configChanged = false;
      }
      // Сигнал родителю, что мы вышли (логика в menu_dist.h обработает)
    }
  }
  
  // Нужно для menu_dist.h чтобы знать режим
  EditMode getEditMode() { return editing ? EDIT_VALUE : EDIT_NONE; }

private:
  void updateScroll() {
    if (selectedItem < scrollOffset) {
      scrollOffset = selectedItem;
    } else if (selectedItem >= scrollOffset + 4) {
      scrollOffset = selectedItem - 3;
    }
    
    // Для 9 пунктов (индекс 8), макс смещение 9-4 = 5
    if (scrollOffset > 5) scrollOffset = 5;
    if (scrollOffset < 0) scrollOffset = 0;
  }
};

#endif