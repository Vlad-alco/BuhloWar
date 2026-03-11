#ifndef MENU_SETTINGS_H
#define MENU_SETTINGS_H

#include <LiquidCrystal_I2C.h>
#include "config.h"
#include "common.h"

class ConfigManager;

class SettingsMenu {
private:
  LiquidCrystal_I2C* lcd;
  ConfigManager* config;
  AppState* appState;
  
  int selectedItem = 0;
  int scrollOffset = 0;
  bool editing = false;
  int tempEditValue = 0;
  bool settingsConfigChanged = false;
  
  struct SettingsItem {
    const char* name;
    int minVal;
    int maxVal;
    int step;
    const char* unit;
  };
  
  // Увеличен размер массива до 7
  SettingsItem settingsItems[7] = {
    {"VREAC", 1, 5, 1, "min"},      // emergencyTime
    {"NASEB", 1, 30, 1, "min"},     // nasebTime
    {"VKLAP", 1, 30, 1, "min"},     // reklapTime
    {"BOX_TPR", 50, 70, 1, "C"},    // boxMaxTemp
    {"POWER", 100, 3500, 100, "W"}, // power
    {"AS_VOL", 1000, 10000, 100, "ml"}, // asVolume
    {"WIFI CHK", 1, 5, 1, "min"}    // chekwifi (НОВЫЙ ПУНКТ)
  };
  
  int getCurrentValue(int itemIndex) {
    SystemConfig& cfg = config->getConfig();
    switch(itemIndex) {
      case 0: return cfg.emergencyTime;
      case 1: return cfg.nasebTime;
      case 2: return cfg.reklapTime;
      case 3: return cfg.boxMaxTemp;
      case 4: return cfg.power;
      case 5: return cfg.asVolume;
      case 6: return cfg.chekwifi; // НОВЫЙ CASE
      default: return 0;
    }
  }
  
  void setCurrentValue(int itemIndex, int value) {
    SystemConfig& cfg = config->getConfig();
    switch(itemIndex) {
      case 0: cfg.emergencyTime = value; break;
      case 1: cfg.nasebTime = value; break;
      case 2: cfg.reklapTime = value; break;
      case 3: cfg.boxMaxTemp = value; break;
      case 4: cfg.power = value; break;
      case 5: cfg.asVolume = value; break;
      case 6: cfg.chekwifi = value; break; // НОВЫЙ CASE
    }
  }
  
public:
  SettingsMenu(LiquidCrystal_I2C* lcdPtr, ConfigManager* cfg, AppState* statePtr) {
    lcd = lcdPtr;
    config = cfg;
    appState = statePtr;
  }
  
  void display() {
    lcd->clear();
    
    for (int i = 0; i < 4; i++) {
      int itemIndex = scrollOffset + i;
      
      // Изменено условие с < 6 на < 7
      if (itemIndex < 7) {
        lcd->setCursor(0, i);
        
        if (itemIndex == selectedItem) {
          lcd->print(">");
        } else {
          lcd->print(" ");
        }
        
        lcd->print(settingsItems[itemIndex].name);
        lcd->print(":");
        
        if (editing && itemIndex == selectedItem) {
          lcd->print("[");
          lcd->print(tempEditValue);
          lcd->print("]");
          
          int printed = 3 + (tempEditValue < 10 ? 1 : (tempEditValue < 100 ? 2 : 3));
          for (int j = printed; j < 20; j++) {
            lcd->print(" ");
          }
        } else {
          int currentValue = getCurrentValue(itemIndex);
          lcd->print(" ");
          lcd->print(currentValue);
          lcd->print(" ");
          lcd->print(settingsItems[itemIndex].unit);
        }
      }
    }
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
      // Изменено с 5 на 6 (макс индекс)
      if (selectedItem < 0) selectedItem = 6;
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
      // Изменено с 5 на 6 (макс индекс)
      if (selectedItem > 6) selectedItem = 0;
      updateScroll();
      display();
    }
  }
  
  void handleSetButton() {
    if (editing) {
      setCurrentValue(selectedItem, tempEditValue);
      settingsConfigChanged = true;
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
      if (settingsConfigChanged) {
        config->saveConfig();
        settingsConfigChanged = false;
      }
      *appState = STATE_MAIN_MENU;
      needMainMenuRedraw = true;
    }
  }
  
private:
  void updateScroll() {
    if (selectedItem < scrollOffset) {
      scrollOffset = selectedItem;
    } else if (selectedItem >= scrollOffset + 4) {
      scrollOffset = selectedItem - 3;
    }
    
    // Изменено с 2 на 3 (т.к. 7 пунктов, 7-4=3)
    if (scrollOffset > 3) scrollOffset = 3;
    if (scrollOffset < 0) scrollOffset = 0;
  }
};

#endif