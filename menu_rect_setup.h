#ifndef MENU_RECT_SETUP_H
#define MENU_RECT_SETUP_H

#include <LiquidCrystal_I2C.h>
#include "config.h"
#include "preferences.h"
#include "common.h"

class ConfigManager;

class RectSetupMenu {
private:
  LiquidCrystal_I2C* lcd;
  ConfigManager* config;
  
  int selectedItem = 0;
  int scrollOffset = 0;
  bool editing = false;
  
  int tempEditValue = 0;
  float tempEditFloat = 0.0f;
  
  bool configChanged = false;

  struct SettingsItem {
    const char* name;
    float minVal;
    float maxVal;
    float step;
    const char* unit;
    bool isBool;   // Флаг: это bool
    bool isFloat;  // Флаг: это float
  };
  
  // Массив из 18 пунктов
  SettingsItem settingsItems[18] = {
    {"TSALIMIT", 40, 55, 1, "C", false, false},    // 0
    {"RAZGON", 30, 80, 1, "C", false, false},      // 1
    {"CYCLE", 1, 3, 1, "", false, false},          // 2
    {"HISTER", 0.18, 5.00, 0.06, "", false, true}, // 3 (Float)
    {"DELTA", 0.06, 0.18, 0.06, "", false, true},  // 4 (Float)
    {"NAGREV", 0, 1, 1, "", true, false},          // 5 (Bool: IND/TEN)
    {"HEADVALVE", 0, 1, 1, "", true, false},       // 6 (Bool: YES/NO)
    {"VALVEBODY", 0, 1, 1, "", true, false},       // 7 (Bool: NC/NO) - ИСПРАВЛЕНО
    {"HEADTYPE", 0, 1, 1, "", true, false},        // 8 (Bool: KSS/ST) - ИСПРАВЛЕНО
    {"CALIBRATE", 0, 1, 1, "", true, false},       // 9 (Bool)
    {"HEAD OPN", 1, 60, 1, "s", false, false},     // 10
    {"HEAD CLS", 1, 60, 1, "s", false, false},     // 11
    {"BODY OPN", 1, 60, 1, "s", false, false},     // 12
    {"BODY CLS", 1, 60, 1, "s", false, false},     // 13
    {"TESTTIME", 30, 300, 5, "s", false, false},   // 14
    {"HCAP", 0, 500, 5, "m", false, false},        // 15
    {"BCAP", 0, 500, 5, "m", false, false},        // 16
    {"B0CAP", 0, 500, 5, "m", false, false}        // 17
  };

  int getCurrentValueInt(int itemIndex) {
    SystemConfig& cfg = config->getConfig();
    switch(itemIndex) {
      case 0: return cfg.tsaLimit;
      case 1: return cfg.razgonTemp;
      case 2: return cfg.cycleLim;
      case 5: return cfg.heaterType;
      case 6: return cfg.useHeadValve ? 1 : 0;
      case 7: return cfg.bodyValveNC ? 1 : 0; // 1=NC, 0=NO
      case 8: return cfg.headsTypeKSS ? 1 : 0; // 1=KSS, 0=ST
      case 9: return cfg.calibration ? 1 : 0;
      case 10: return cfg.headOpenMs;
      case 11: return cfg.headCloseMs;
      case 12: return cfg.bodyOpenMs;
      case 13: return cfg.bodyCloseMs;
      case 14: return cfg.active_test;
      case 15: return cfg.valve_head_capacity;
      case 16: return cfg.valve_body_capacity;
      case 17: return cfg.valve0_body_capacity;
      default: return 0;
    }
  }

  float getCurrentValueFloat(int itemIndex) {
    SystemConfig& cfg = config->getConfig();
    switch(itemIndex) {
      case 3: return cfg.histeresis;
      case 4: return cfg.delta;
      default: return 0.0f;
    }
  }

  void setCurrentValue(int itemIndex, int value) {
    SystemConfig& cfg = config->getConfig();
    switch(itemIndex) {
      case 0: cfg.tsaLimit = value; break;
      case 1: cfg.razgonTemp = value; break;
      case 2: cfg.cycleLim = value; break;
      case 5: cfg.heaterType = value; break;
      case 6: cfg.useHeadValve = (value == 1); break;
      case 7: cfg.bodyValveNC = (value == 1); break; // Сохраняем NC/NO
      case 8: cfg.headsTypeKSS = (value == 1); break;
      case 9: cfg.calibration = (value == 1); break;
      case 10: cfg.headOpenMs = value; break;
      case 11: cfg.headCloseMs = value; break;
      case 12: cfg.bodyOpenMs = value; break;
      case 13: cfg.bodyCloseMs = value; break;
      case 14: cfg.active_test = value; break;
      case 15: cfg.valve_head_capacity = value; break;
      case 16: cfg.valve_body_capacity = value; break;
      case 17: cfg.valve0_body_capacity = value; break;
    }
  }

  void setCurrentValueFloat(int itemIndex, float value) {
    SystemConfig& cfg = config->getConfig();
    switch(itemIndex) {
      case 3: cfg.histeresis = value; break;
      case 4: cfg.delta = value; break;
    }
  }

public:
  RectSetupMenu(LiquidCrystal_I2C* lcdPtr, ConfigManager* cfg) {
    lcd = lcdPtr;
    config = cfg;
  }
  
  void display() {
    lcd->clear();
    
    for (int i = 0; i < 4; i++) {
      int itemIndex = scrollOffset + i;
      
      if (itemIndex < 18) {
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
          printValue(itemIndex, true);
          lcd->print("]");
        } else {
          printValue(itemIndex, false);
        }
      }
    }
  }

  void printValue(int itemIndex, bool isEditing) {
    SettingsItem& item = settingsItems[itemIndex];
    
    if (item.isBool) {
      int val = isEditing ? tempEditValue : getCurrentValueInt(itemIndex);
      // Специфичные надписи
      if (itemIndex == 5) lcd->print(val == 1 ? "IND" : "TEN");      // NAGREV
      else if (itemIndex == 7) lcd->print(val == 1 ? "NC" : "NO");   // VALVEBODY
      else if (itemIndex == 8) lcd->print(val == 1 ? "KSS" : "ST");  // HEADTYPE
      else lcd->print(val == 1 ? "YES" : "NO");
    } 
    else if (item.isFloat) {
      float val = isEditing ? tempEditFloat : getCurrentValueFloat(itemIndex);
      lcd->print(val, 2);
      if (!isEditing) {
        lcd->print(" ");
        lcd->print(item.unit);
      }
    } 
    else {
      int val = isEditing ? tempEditValue : getCurrentValueInt(itemIndex);
      lcd->print(val);
      if (!isEditing) {
        lcd->print(" ");
        lcd->print(item.unit);
      }
    }
  }
  
  void handleUpButton() {
    if (editing) {
      SettingsItem& item = settingsItems[selectedItem];
      if (item.isBool) {
        tempEditValue = (tempEditValue == 0) ? 1 : 0;
      } else if (item.isFloat) {
        tempEditFloat += item.step;
        if (tempEditFloat > item.maxVal) tempEditFloat = item.maxVal;
      } else {
        tempEditValue += (int)item.step;
        if (tempEditValue > (int)item.maxVal) tempEditValue = (int)item.maxVal;
      }
      display();
    } else {
      selectedItem--;
      if (selectedItem < 0) selectedItem = 17;
      updateScroll();
      display();
    }
  }
  
  void handleDownButton() {
    if (editing) {
      SettingsItem& item = settingsItems[selectedItem];
      if (item.isBool) {
        tempEditValue = (tempEditValue == 0) ? 1 : 0;
      } else if (item.isFloat) {
        tempEditFloat -= item.step;
        if (tempEditFloat < item.minVal) tempEditFloat = item.minVal;
      } else {
        tempEditValue -= (int)item.step;
        if (tempEditValue < (int)item.minVal) tempEditValue = (int)item.minVal;
      }
      display();
    } else {
      selectedItem++;
      if (selectedItem > 17) selectedItem = 0;
      updateScroll();
      display();
    }
  }
  
  void handleSetButton() {
    if (editing) {
      SettingsItem& item = settingsItems[selectedItem];
      if (item.isFloat) setCurrentValueFloat(selectedItem, tempEditFloat);
      else setCurrentValue(selectedItem, tempEditValue);
      
      configChanged = true;
      editing = false;
    } else {
      SettingsItem& item = settingsItems[selectedItem];
      if (item.isFloat) tempEditFloat = getCurrentValueFloat(selectedItem);
      else tempEditValue = getCurrentValueInt(selectedItem);
      
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
        config->saveRectConfig();
        configChanged = false;
      }
    }
  }
  
  EditMode getEditMode() { return editing ? EDIT_VALUE : EDIT_NONE; }
  
private:
  void updateScroll() {
    if (selectedItem < scrollOffset) scrollOffset = selectedItem;
    else if (selectedItem >= scrollOffset + 4) scrollOffset = selectedItem - 3;
    
    if (scrollOffset > 14) scrollOffset = 14;
    if (scrollOffset < 0) scrollOffset = 0;
  }
};

#endif