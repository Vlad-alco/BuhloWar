#ifndef VALVE_CAL_MENU_H
#define VALVE_CAL_MENU_H

#include <LiquidCrystal_I2C.h>
#include "config.h"
#include "preferences.h"
#include "common.h"
#include "OutputManager.h"
#include <cstring> 

enum class ValveCalState {
  MENU_MAIN,
  MENU_HEAD,
  MENU_BODY,
  RUNNING_TEST
};

class ValveCalMenu {
private:
  LiquidCrystal_I2C* lcd;
  ConfigManager* config;
  OutputManager* output;
  
  ValveCalState currentState = ValveCalState::MENU_MAIN;
  int selectedItem = 0;
  EditMode editMode = EDIT_NONE;
  int tempEditValue = 0;

  bool isTestRunning = false;
  unsigned long testStartTime = 0;
  int testDurationSec = 0;
  bool isHeadTest = false; 
   bool exitConfirmed = false; // Флаг подтверждения выхода

  struct MenuItem {
      const char* label;
      int* valuePtr;
      int minVal;
      int maxVal;
      int step;
      const char* unit;

      // Конструктор для удобства
      MenuItem(const char* l, int* v, int min, int max, int s, const char* u) 
        : label(l), valuePtr(v), minVal(min), maxVal(max), step(s), unit(u) {}
      // Пустой конструктор по умолчанию
      MenuItem() : label(""), valuePtr(nullptr), minVal(0), maxVal(0), step(0), unit("") {}
  };
  
  MenuItem headMenuItems[5];
  MenuItem bodyMenuItems[6];

public:
  ValveCalMenu(LiquidCrystal_I2C* lcdPtr, ConfigManager* cfg, OutputManager* out) {
    lcd = lcdPtr;
    config = cfg;
    output = out;
    
    SystemConfig& cfgData = config->getConfig();
    
    // Используем правильные имена переменных: headOpenMs, headCloseMs и т.д.
    headMenuItems[0] = MenuItem("OPEN", &cfgData.headOpenMs, 1, 60, 1, "sec");
    headMenuItems[1] = MenuItem("CLOSE", &cfgData.headCloseMs, 1, 60, 1, "sec");
    headMenuItems[2] = MenuItem("TESTTIME", &cfgData.active_test, 30, 300, 5, "sec");
    headMenuItems[3] = MenuItem("RUNTEST", nullptr, 0, 0, 0, "");
    headMenuItems[4] = MenuItem("HCAP", &cfgData.valve_head_capacity, 1, 500, 5, "ml");
    
    bodyMenuItems[0] = MenuItem("OPEN", &cfgData.bodyOpenMs, 1, 60, 1, "sec");
    bodyMenuItems[1] = MenuItem("CLOSE", &cfgData.bodyCloseMs, 1, 60, 1, "sec");
    bodyMenuItems[2] = MenuItem("TESTTIME", &cfgData.active_test, 30, 300, 5, "sec");
    bodyMenuItems[3] = MenuItem("RUNTEST", nullptr, 0, 0, 0, "");
    bodyMenuItems[4] = MenuItem("BCAP", &cfgData.valve_body_capacity, 1, 500, 5, "ml");
    bodyMenuItems[5] = MenuItem("B0CAP", &cfgData.valve0_body_capacity, 1, 500, 5, "ml");
  }

  void display() {
    exitConfirmed = false;
    if (isTestRunning) return; 
    
    lcd->clear();
    
    if (editMode != EDIT_NONE) {
        displayEditMode();
        return;
    }

    switch (currentState) {
      case ValveCalState::MENU_MAIN: displayMainMenu(); break;
      case ValveCalState::MENU_HEAD: displaySubMenu(headMenuItems, 5, "HEAD"); break;
      case ValveCalState::MENU_BODY: displaySubMenu(bodyMenuItems, 6, "BODY"); break;
      default: break;
    }
  }
  
  void displayMainMenu() {
    lcd->setCursor(7, 0); lcd->print("VALVE");
    lcd->setCursor(0, 1); lcd->print(selectedItem == 0 ? ">HEAD" : " HEAD");
    lcd->setCursor(0, 2); lcd->print(selectedItem == 1 ? ">BODY" : " BODY");
    lcd->setCursor(0, 3); lcd->print(selectedItem == 2 ? ">SET PW & AS" : " SET PW & AS");
  }
  
  void displaySubMenu(MenuItem* items, int itemCount, const char* title) {
     lcd->setCursor((20 - strlen(title))/2, 0); lcd->print(title);
     
     int startIdx = 0;
     if (selectedItem > 2) startIdx = selectedItem - 2;
     if (itemCount - startIdx < 3 && itemCount > 3) startIdx = itemCount - 3;
     
     for(int i=0; i<3; i++) {
       int idx = startIdx + i;
       if (idx >= itemCount) break;
       
       lcd->setCursor(0, i+1);
       lcd->print(idx == selectedItem ? ">" : " ");
       lcd->print(items[idx].label);
       
       // Если есть значение (не RUNTEST)
       if (items[idx].valuePtr != nullptr) {
         // Формируем строку "ЗНАЧЕНИЕ ЕДИННИЦА"
         char valBuf[10]; 
         snprintf(valBuf, sizeof(valBuf), "%d %s", *items[idx].valuePtr, items[idx].unit);
         
         // Выравниваем её по правому краю (колонка 20 - длина строки)
         // Максимум ширина значения с единицами: "1000 sec" (8 симв). 
         // Ставим курсор на 12 или 13 колонку в зависимости от длины
         int valLen = strlen(valBuf);
         int cursorPos = 20 - valLen; 
         if (cursorPos < 0) cursorPos = 0; // Защита
         
         lcd->setCursor(cursorPos, i+1);
         lcd->print(valBuf);
       }
     }
  }

  void displayEditMode() {
     MenuItem* item = getCurrentItem();
     if (!item) return;
     
     lcd->setCursor(0, 1);
     lcd->print(item->label);
     lcd->print(" [");
     lcd->print(tempEditValue);
     lcd->print("] ");
     lcd->print(item->unit);
  }
  
  MenuItem* getCurrentItem() {
      if (currentState == ValveCalState::MENU_HEAD && selectedItem < 5) return &headMenuItems[selectedItem];
      if (currentState == ValveCalState::MENU_BODY && selectedItem < 6) return &bodyMenuItems[selectedItem];
      return nullptr;
  }

  void handleUpButton() {
    if (isTestRunning) return;
    
    if (editMode == EDIT_NONE) {
      int limit = 0;
      if (currentState == ValveCalState::MENU_MAIN) limit = 2;
      else if (currentState == ValveCalState::MENU_HEAD) limit = 4;
      else if (currentState == ValveCalState::MENU_BODY) limit = 5;
      
      selectedItem--;
      if (selectedItem < 0) selectedItem = limit;
      display();
    } else {
      MenuItem* item = getCurrentItem();
      if (item && item->valuePtr) {
        tempEditValue += item->step;
        if (tempEditValue > item->maxVal) tempEditValue = item->maxVal;
        display();
      }
    }
  }
  
  void handleDownButton() {
    if (isTestRunning) return;
    
    if (editMode == EDIT_NONE) {
      int limit = 0;
      if (currentState == ValveCalState::MENU_MAIN) limit = 2;
      else if (currentState == ValveCalState::MENU_HEAD) limit = 4;
      else if (currentState == ValveCalState::MENU_BODY) limit = 5;
      
      selectedItem++;
      if (selectedItem > limit) selectedItem = 0;
      display();
    } else {
      MenuItem* item = getCurrentItem();
      if (item && item->valuePtr) {
        tempEditValue -= item->step;
        if (tempEditValue < item->minVal) tempEditValue = item->minVal;
        display();
      }
    }
  }
  
    void handleSetButton() {
    if (isTestRunning) return;
    
    if (editMode == EDIT_NONE) {
        if (currentState == ValveCalState::MENU_MAIN) {
            if (selectedItem == 0) { currentState = ValveCalState::MENU_HEAD; selectedItem = 0; display(); }
            else if (selectedItem == 1) { currentState = ValveCalState::MENU_BODY; selectedItem = 0; display(); }
            else if (selectedItem == 2) { 
               // Нажали SET на "SET PW & AS" -> подтверждаем выход
               exitConfirmed = true; 
            }
        } else {
            MenuItem* item = getCurrentItem();
            if (item && item->valuePtr == nullptr) {
                startTest();
            } else if (item && item->valuePtr) {
                editMode = EDIT_VALUE;
                tempEditValue = *item->valuePtr;
                display();
            }
        }
    } else {
        MenuItem* item = getCurrentItem();
        if (item && item->valuePtr) {
            *item->valuePtr = tempEditValue;
            config->saveRectConfig();
        }
        editMode = EDIT_NONE;
        display();
    }
  }
  
  void handleBackButton() {
    if (isTestRunning) { stopTest(); display(); return; }
    if (editMode != EDIT_NONE) { editMode = EDIT_NONE; display(); return; }
    if (currentState != ValveCalState::MENU_MAIN) { currentState = ValveCalState::MENU_MAIN; selectedItem = 0; display(); }
  }
  
  bool isReadyToExit() {
    return exitConfirmed;
  }
  
  void update() {
    if (isTestRunning) {
        unsigned long elapsed = (millis() - testStartTime) / 1000;
        char buf[16];
        sprintf(buf, "Time: %03d / %d", (int)elapsed, testDurationSec);
        lcd->setCursor(0, 2); lcd->print(buf);
        if (elapsed >= testDurationSec) { stopTest(); display(); }
    }
  }

private:
  void startTest() {
    SystemConfig& cfg = config->getConfig();
    testDurationSec = cfg.active_test;
    isHeadTest = (currentState == ValveCalState::MENU_HEAD);
    
    // Используем правильные имена переменных
    if (isHeadTest) {
        output->startHeadValveCycling(cfg.headOpenMs * 1000, cfg.headCloseMs * 1000);
    } else {
        output->startBodyValveCycling(cfg.bodyOpenMs * 1000, cfg.bodyCloseMs * 1000);
    }
    
    isTestRunning = true;
    testStartTime = millis();
    
    lcd->clear();
    lcd->setCursor(0,0); lcd->print(isHeadTest ? "HEAD TEST" : "BODY TEST");
    lcd->setCursor(0,1); lcd->print("RUNNING...");
  }
  
  void stopTest() {
    output->stopValveCycling();
    isTestRunning = false;
  }
};

#endif