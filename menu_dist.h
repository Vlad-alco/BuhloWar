#ifndef MENU_DIST_H
#define MENU_DIST_H

#include <LiquidCrystal_I2C.h>
#include "config.h"
#include "common.h"
#include "menu_dist_setup.h"
#include "ProcessEngine.h" 

extern ProcessEngine processEngine;

class ConfigManager;

enum DistState {
  DIST_MAIN_MENU,
  DIST_WATER_TEST,
  DIST_SETUP_MENU,
  DIST_PROCESS_SCREEN,
  DIST_SENSOR_ERROR    // Экран блокировки: один или несколько датчиков не готовы
};

class DistMenu {
private:
  LiquidCrystal_I2C* lcd;
  ConfigManager* config;
  DistState distState = DIST_MAIN_MENU;
  AppState* appState;
  MainMenu* mainMenu;
  
  DistSetupMenu* distSetupMenu = NULL;
  
  int selectedItem = 0;
  const char* distMenuItems[3] = {
    "DIST START",
    "SETUP",
    "STOP"
  };
  
public:
  DistMenu(LiquidCrystal_I2C* lcdPtr, ConfigManager* cfg, AppState* statePtr, MainMenu* mainMenuPtr) {
    lcd = lcdPtr;
    config = cfg;
    appState = statePtr;
    mainMenu = mainMenuPtr;
    distSetupMenu = new DistSetupMenu(lcd, cfg);
  }
  
  void display() {
    if (distState == DIST_SETUP_MENU && distSetupMenu) {
      distSetupMenu->display();
      return;
    }

    lcd->clear();
    
    switch(distState) {
      case DIST_MAIN_MENU: displayMainMenu(); break;
      case DIST_WATER_TEST: displayWaterTestScreen(); break;
      case DIST_PROCESS_SCREEN: displayProcessScreen(); break;
      case DIST_SENSOR_ERROR: displaySensorErrorScreen(); break;
      case DIST_SETUP_MENU: break;
    }
  }

  void update() {
  // === СИНХРОНИЗАЦИЯ СОСТОЯНИЯ (для Web) ===
  // Если мы на экране теста воды, а процесс уже пошел (нажали ДА на Web) -> переключаем экран
  if (distState == DIST_WATER_TEST) {
      const SystemStatus& status = processEngine.getStatus();
      if (status.stageName != "WATER_TEST") {
          distState = DIST_PROCESS_SCREEN;
      }
  }

  // Обычное обновление экрана процесса
  if (distState == DIST_PROCESS_SCREEN) {
    const SystemStatus& status = processEngine.getStatus();
    if (status.stageName == "REPLACEMENT") {
      if (selectedItem != 0 && selectedItem != 1) selectedItem = 0; 
    }
    display();
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
      lcd->print(distMenuItems[i]);
      
      if (i == 0 && config->isDistProcessRunning()) lcd->print(" (RUN)");
      if (i == 2 && !config->isDistProcessRunning()) lcd->print(" [N/A]");
    }
  }

  // ---------------------------------------------------------------------------
  // displaySensorErrorScreen() — экран блокировки запуска процесса.
  //
  // Строка 0: заголовок "SENSOR NOT READY:"
  // Строка 1: имена проблемных датчиков из processEngine.getStatus().sensorErrorMsg
  //           (например "TSA TANK" или "TSAR AQUA TSA")
  // Строка 2: подсказка обратиться в меню датчиков
  // Строка 3: подсказка нажать BACK для возврата
  //
  // Оператор остаётся на этом экране до нажатия BACK.
  // ---------------------------------------------------------------------------
  void displaySensorErrorScreen() {
    const SystemStatus& status = processEngine.getStatus();

    lcd->setCursor(0, 0); lcd->print("SENSOR NOT READY:   ");
    // Строка 1: имена датчиков с проблемой, обрезаем до 20 символов
    char line1[21];
    snprintf(line1, sizeof(line1), "%-20s", status.sensorErrorMsg.c_str());
    lcd->setCursor(0, 1); lcd->print(line1);
    lcd->setCursor(0, 2); lcd->print("GO TO SENSORS MENU  ");
    lcd->setCursor(0, 3); lcd->print("BACK - return       ");
  }
  
   void displayProcessScreen() {
    const SystemStatus& status = processEngine.getStatus();
    
    // Используем конструкцию if - else if - else
    if (status.stageName == "WATER_TEST") {
      // --- ВОДЯНОЙ ТЕСТ ---
      lcd->setCursor(0, 0); lcd->print("WATER TEST        ");
      lcd->setCursor(0, 1); lcd->print(selectedItem == 0 ? "> YES             " : "  YES             ");
      lcd->setCursor(0, 2); lcd->print(selectedItem == 1 ? "> NO              " : "  NO              ");
      lcd->setCursor(0, 3); lcd->print("                    ");
    } 
    else if (status.stageName == "REPLACEMENT") {
      // --- СМЕНА ПОСУДЫ ---
      lcd->setCursor(0, 0); lcd->print("REPLACEMENT        ");
      lcd->setCursor(0, 1); lcd->print(selectedItem == 0 ? "> YES             " : "  YES             ");
      lcd->setCursor(0, 2); lcd->print(selectedItem == 1 ? "> NO              " : "  NO              ");
      lcd->setCursor(0, 3); lcd->print("                    ");
    } 
    else {
      // --- ОБЫЧНЫЙ ЭКРАН ПРОЦЕССА ---
      lcd->setCursor(0, 0); lcd->print(status.line0);
      lcd->setCursor(0, 1); lcd->print(status.line1);
      lcd->setCursor(0, 2); lcd->print(status.line2);
      lcd->setCursor(0, 3); lcd->print(status.line3);
    }
  }
  
  // ================= ОБРАБОТКА КНОПОК =================

  void handleUpButton() {
    if (distState == DIST_SETUP_MENU && distSetupMenu) {
      distSetupMenu->handleUpButton();
      return;
    }

    if (distState == DIST_MAIN_MENU) {
      selectedItem--;
      if (selectedItem < 0) selectedItem = 2;
      display();
    } 
    else if (distState == DIST_WATER_TEST) {
      selectedItem = 0; 
      display();
    }
  }
  
  void handleDownButton() {
    if (distState == DIST_SETUP_MENU && distSetupMenu) {
      distSetupMenu->handleDownButton();
      return;
    }

    if (distState == DIST_MAIN_MENU) {
      selectedItem++;
      if (selectedItem > 2) selectedItem = 0;
      display();
    }
    else if (distState == DIST_WATER_TEST) {
      selectedItem = 1; 
      display();
    }
  }
  
  void handleSetButton() {
    if (distState == DIST_SETUP_MENU && distSetupMenu) { distSetupMenu->handleSetButton(); return; }

    if (distState == DIST_MAIN_MENU) {
      switch(selectedItem) {
        case 0: 
          if (processEngine.isProcessRunning()) {
            // Процесс уже идёт — просто открываем экран процесса
            distState = DIST_PROCESS_SCREEN;
          } else {
            // Пытаемся запустить — processEngine проверит датчики
            EngineResponse resp = processEngine.handleCommand(UiCommand::START_DIST);
            if (resp == EngineResponse::ERROR_INVALID_STATE) {
              // Датчики не готовы — переходим на экран ошибки
              distState = DIST_SENSOR_ERROR;
            } else {
              // Датчики OK, процесс запущен — переходим на Water Test
              distState = DIST_WATER_TEST;
              selectedItem = 0;
            }
          }
          display();
          break;
        case 1: distState = DIST_SETUP_MENU; break;
        case 2: 
          if (processEngine.isProcessRunning()) {
            processEngine.handleCommand(UiCommand::STOP_PROCESS);
            distState = DIST_PROCESS_SCREEN;
            display();
          }
          break;
      }
    } 
    else if (distState == DIST_WATER_TEST) {
        if (selectedItem == 0) processEngine.handleCommand(UiCommand::DIALOG_YES);
        else processEngine.handleCommand(UiCommand::DIALOG_NO);
        const SystemStatus& status = processEngine.getStatus();
        if (status.stageName == "RAZGON") distState = DIST_PROCESS_SCREEN;
        else distState = DIST_MAIN_MENU; 
        display();
    }
    else if (distState == DIST_PROCESS_SCREEN) {
      const SystemStatus& status = processEngine.getStatus();
      // ЛОГИКА ДЛЯ ДИАЛОГОВ (WATER_TEST и REPLACEMENT)
      if (status.stageName == "REPLACEMENT" || status.stageName == "WATER_TEST") {
        if (selectedItem == 0) processEngine.handleCommand(UiCommand::DIALOG_YES);
        else processEngine.handleCommand(UiCommand::DIALOG_NO);
        display();
      }
    }
  }
  
  void handleBackButton() {
    if (distState == DIST_SETUP_MENU && distSetupMenu) {
      // 1. Проверяем, были ли мы в режиме редактирования ДО нажатия
      bool wasEditing = (distSetupMenu->getEditMode() != EDIT_NONE);
      
      // 2. ВСЕГДА передаем кнопку BACK в меню настроек
      distSetupMenu->handleBackButton();
      
      // 3. Выходим в главное меню только если мы НЕ были в режиме редактирования
      if (!wasEditing) {
        distState = DIST_MAIN_MENU; 
        display();
      }
      return;
    }
    
    if (distState == DIST_MAIN_MENU) {
      *appState = STATE_MAIN_MENU; needMainMenuRedraw = true;
    } 
    else if (distState == DIST_SENSOR_ERROR) {
      // Оператор ознакомился с ошибкой — возвращаем в главное меню DIST
      distState = DIST_MAIN_MENU;
      display();
    }
    else if (distState == DIST_WATER_TEST) {
        processEngine.handleCommand(UiCommand::DIALOG_NO);
        distState = DIST_MAIN_MENU; display();
    }
    else if (distState == DIST_PROCESS_SCREEN) {
      const SystemStatus& status = processEngine.getStatus();
      if (status.stageName == "REPLACEMENT") {
         processEngine.handleCommand(UiCommand::DIALOG_NO);
         display();
      } else {
        distState = DIST_MAIN_MENU; display();
      }
    }
  }

   DistState getState() { return distState; }

  // === ВАЖНО: Только ОДИН раз ===
  void setState(DistState state) { distState = state; }
};

#endif