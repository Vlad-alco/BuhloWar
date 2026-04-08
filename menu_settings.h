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
  
  // Увеличен размер массива до 10 (добавлены MINOPEN, SP_H_COR, SP_B_COR)
  SettingsItem settingsItems[10] = {
    {"VREAC", 1, 10, 1, "min"},     // 0: emergencyTime
    {"NASEB", 1, 30, 1, "min"},     // 1: nasebTime
    {"VKLAP", 1, 30, 1, "min"},     // 2: reklapTime
    {"BOX_TPR", 50, 70, 1, "C"},    // 3: boxMaxTemp
    {"POWER", 100, 3500, 100, "W"}, // 4: power
    {"AS_VOL", 1000, 10000, 100, "ml"}, // 5: asVolume
    {"MINOPEN", 50, 500, 10, "ms"}, // 6: minOpenTime (НОВЫЙ)
    {"SP_H_COR", 10, 200, 1, "%"},  // 7: speedHeadCorr (НОВЫЙ)
    {"SP_B_COR", 10, 200, 1, "%"},  // 8: speedBodyCorr (НОВЫЙ)
    {"WIFI CHK", 1, 5, 1, "min"}    // 9: chekwifi
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
      case 6: return cfg.minOpenTime;      // НОВЫЙ
      case 7: return cfg.speedHeadCorr;    // НОВЫЙ
      case 8: return cfg.speedBodyCorr;    // НОВЫЙ
      case 9: return cfg.chekwifi;
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
      case 6: cfg.minOpenTime = value; break;      // НОВЫЙ
      case 7: cfg.speedHeadCorr = value; break;    // НОВЫЙ
      case 8: cfg.speedBodyCorr = value; break;    // НОВЫЙ
      case 9: cfg.chekwifi = value; break;
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
      
      // Изменено условие с < 7 на < 10
      if (itemIndex < 10) {
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
      // Изменено с 6 на 9 (макс индекс)
      if (selectedItem < 0) selectedItem = 9;
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
      // Изменено с 6 на 9 (макс индекс)
      if (selectedItem > 9) selectedItem = 0;
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
    
    // Изменено с 3 на 6 (т.к. 10 пунктов, 10-4=6)
    if (scrollOffset > 6) scrollOffset = 6;
    if (scrollOffset < 0) scrollOffset = 0;
  }
};

#endif
