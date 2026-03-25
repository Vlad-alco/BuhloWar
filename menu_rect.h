#ifndef MENU_RECT_H
#define MENU_RECT_H

#include <LiquidCrystal_I2C.h>
#include "config.h"
#include "common.h"
#include "menu_rect_setup.h"
#include "ProcessEngine.h" 

extern ProcessEngine processEngine;

class ConfigManager;

enum RectState {
  RECT_MAIN_MENU,
  RECT_WATER_TEST,
  RECT_SETUP_MENU,
  RECT_PROCESS_SCREEN,
  RECT_SENSOR_ERROR    // Экран блокировки: один или несколько датчиков не готовы
};

class RectMenu {
private:
  LiquidCrystal_I2C* lcd;
  ConfigManager* config;
  RectState rectState = RECT_MAIN_MENU;
  AppState* appState;
  MainMenu* mainMenu;
  
  RectSetupMenu* rectSetupMenu = NULL;
  int selectedItem = 0;
  const char* rectMenuItems[3] = {"RECT START", "SETUP", "STOP"};
  
public:
  RectMenu(LiquidCrystal_I2C* lcdPtr, ConfigManager* cfg, AppState* statePtr, MainMenu* mainMenuPtr) {
    lcd = lcdPtr;
    config = cfg;
    appState = statePtr;
    mainMenu = mainMenuPtr;
    rectSetupMenu = new RectSetupMenu(lcd, cfg);
  }
  
  void display() {
    if (rectState == RECT_SETUP_MENU && rectSetupMenu) { rectSetupMenu->display(); return; }
    lcd->clear();
    switch(rectState) {
      case RECT_MAIN_MENU: displayMainMenu(); break;
      case RECT_WATER_TEST: displayWaterTestScreen(); break;
      case RECT_PROCESS_SCREEN: displayProcessScreen(); break;
      case RECT_SENSOR_ERROR: displaySensorErrorScreen(); break;
      case RECT_SETUP_MENU: break;
    }
  }
  
  void displayWaterTestScreen() {
    lcd->setCursor(0, 0); lcd->print("WATER TEST");
    lcd->setCursor(0, 1); lcd->print(selectedItem == 0 ? "> YES" : "  YES");
    lcd->setCursor(0, 2); lcd->print(selectedItem == 1 ? "> NO" : "  NO");
    lcd->setCursor(0, 3); lcd->print("                    ");
  }

  void displayMainMenu() {
    for (int i = 0; i < 3; i++) {
      lcd->setCursor(0, i);
      lcd->print(i == selectedItem ? ">" : " ");
      lcd->print(rectMenuItems[i]);
      if (i == 0 && processEngine.isProcessRunning() && processEngine.getActiveProcessType() == PROCESS_RECT) lcd->print(" (RUN)");
      if (i == 2 && !processEngine.isProcessRunning()) lcd->print(" [N/A]");
    }
  }

  // ---------------------------------------------------------------------------
  // displaySensorErrorScreen() — экран блокировки запуска процесса RECT.
  //
  // Идентичен версии в menu_dist.h — показывает какие датчики не готовы
  // и удерживает экран до нажатия BACK оператором.
  // ---------------------------------------------------------------------------
  void displaySensorErrorScreen() {
    const SystemStatus& status = processEngine.getStatus();

    lcd->setCursor(0, 0); lcd->print("SENSOR NOT READY:   ");
    char line1[21];
    snprintf(line1, sizeof(line1), "%-20s", status.sensorErrorMsg.c_str());
    lcd->setCursor(0, 1); lcd->print(line1);
    lcd->setCursor(0, 2); lcd->print("GO TO SENSORS MENU  ");
    lcd->setCursor(0, 3); lcd->print("BACK - return       ");
  }
  
  void displayProcessScreen() {
    const SystemStatus& status = processEngine.getStatus();
    lcd->setCursor(0, 0); lcd->print(status.line0);
    lcd->setCursor(0, 1); lcd->print(status.line1);
    lcd->setCursor(0, 2); lcd->print(status.line2);
    lcd->setCursor(0, 3); lcd->print(status.line3);
  }
  
  // --- Кнопки ---

  void handleUpButton() {
    if (rectState == RECT_SETUP_MENU && rectSetupMenu) { rectSetupMenu->handleUpButton(); return; }

    const SystemStatus& status = processEngine.getStatus();
    if (rectState == RECT_PROCESS_SCREEN && 
       (status.stageName == "VALVE_CAL" || status.stageName == "SET_PW_AS" || status.stageName == "GOLOVY_OK")) {
        processEngine.handleUiUp(); 
        return;
    }
    
    if (rectState == RECT_MAIN_MENU) {
      selectedItem--; if (selectedItem < 0) selectedItem = 2;
      display();
    } else if (rectState == RECT_WATER_TEST) {
      selectedItem = 0; display();
    }
  }
  
  void handleDownButton() {
    if (rectState == RECT_SETUP_MENU && rectSetupMenu) { rectSetupMenu->handleDownButton(); return; }

    const SystemStatus& status = processEngine.getStatus();
    if (rectState == RECT_PROCESS_SCREEN && 
       (status.stageName == "VALVE_CAL" || status.stageName == "SET_PW_AS" || status.stageName == "GOLOVY_OK")) {
        processEngine.handleUiDown(); 
        return;
    }
    
    if (rectState == RECT_MAIN_MENU) {
      selectedItem++; if (selectedItem > 2) selectedItem = 0;
      display();
    } else if (rectState == RECT_WATER_TEST) {
      selectedItem = 1; display();
    }
  }
  
  void handleSetButton() {
    if (rectState == RECT_SETUP_MENU && rectSetupMenu) { rectSetupMenu->handleSetButton(); return; }

    const SystemStatus& status = processEngine.getStatus();
    if (rectState == RECT_PROCESS_SCREEN && 
       (status.stageName == "VALVE_CAL" || status.stageName == "SET_PW_AS" || status.stageName == "GOLOVY_OK")) {
        processEngine.handleUiSet(); 
        return;
    }
    
    if (rectState == RECT_MAIN_MENU) {
      switch(selectedItem) {
        case 0:
          if (processEngine.isProcessRunning()) {
            // Процесс уже идёт — просто открываем экран процесса
            rectState = RECT_PROCESS_SCREEN;
          } else {
            // Пытаемся запустить — processEngine проверит датчики
            EngineResponse resp = processEngine.handleCommand(UiCommand::START_RECT);
            if (resp == EngineResponse::ERROR_INVALID_STATE) {
              // Датчики не готовы — переходим на экран ошибки
              rectState = RECT_SENSOR_ERROR;
            } else {
              // Датчики OK, процесс запущен — переходим на Water Test
              rectState = RECT_WATER_TEST;
              selectedItem = 0;
            }
          }
          display();
          break;
        case 1: rectState = RECT_SETUP_MENU; display(); break;
       case 2:
          if (processEngine.isProcessRunning()) {
            processEngine.handleCommand(UiCommand::STOP_PROCESS);
            rectState = RECT_PROCESS_SCREEN;
            display();
          }
          break;
      }
    } 
    else if (rectState == RECT_WATER_TEST) {
        if (selectedItem == 0) processEngine.handleCommand(UiCommand::DIALOG_YES);
        else processEngine.handleCommand(UiCommand::DIALOG_NO);
        const SystemStatus& st = processEngine.getStatus();
        if (st.stageName == "RAZGON") rectState = RECT_PROCESS_SCREEN; else rectState = RECT_MAIN_MENU;
        display();
    }
  }
  
  void handleBackButton() {
    if (rectState == RECT_SETUP_MENU && rectSetupMenu) {
      // 1. Проверяем, были ли мы в режиме редактирования ДО нажатия
      bool wasEditing = (rectSetupMenu->getEditMode() != EDIT_NONE);
      
      // 2. ВСЕГДА передаем кнопку BACK в меню настроек (это вызовет сохранение!)
      rectSetupMenu->handleBackButton();
      
      // 3. Выходим в главное меню только если мы НЕ были в режиме редактирования
      if (!wasEditing) {
        rectState = RECT_MAIN_MENU;
        display();
      }
      return;
    }

    const SystemStatus& status = processEngine.getStatus();
    if (rectState == RECT_PROCESS_SCREEN && 
       (status.stageName == "VALVE_CAL" || status.stageName == "SET_PW_AS" || status.stageName == "GOLOVY_OK")) {
        processEngine.handleUiBack(); 
        return;
    }
        
    if (rectState == RECT_MAIN_MENU) {
      *appState = STATE_MAIN_MENU; needMainMenuRedraw = true;
    }
    else if (rectState == RECT_SENSOR_ERROR) {
      // Оператор ознакомился с ошибкой — возвращаем в главное меню RECT
      rectState = RECT_MAIN_MENU;
      display();
    }
    else if (rectState == RECT_WATER_TEST) {
        processEngine.handleCommand(UiCommand::DIALOG_NO);
        rectState = RECT_MAIN_MENU; display();
    }
    else if (rectState == RECT_PROCESS_SCREEN) {
      rectState = RECT_MAIN_MENU; display();
    }
  }

    void update() {
    // === СИНХРОНИЗАЦИЯ СОСТОЯНИЯ (для Web) ===
    // Если мы на экране теста воды, а процесс уже пошел -> переключаем экран
    if (rectState == RECT_WATER_TEST) {
        const SystemStatus& status = processEngine.getStatus();
        if (status.stageName != "WATER_TEST") {
            rectState = RECT_PROCESS_SCREEN;
        }
    }

    // Всегда обновляем экран (а не только в PROCESS_SCREEN)
    display();
  }
  RectState getState() { return rectState; }
  void setState(RectState state) { rectState = state; }
};

#endif