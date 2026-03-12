#ifndef MENU_MAIN_H
#define MENU_MAIN_H

#include <LiquidCrystal_I2C.h>
#include "config.h"
#include "common.h"

// Временное объявление ConfigManager
class ConfigManager;

class MainMenu {
private:
  LiquidCrystal_I2C* lcd;
  ConfigManager* config;
  AppState* currentState;
  int selectedItem = 0;
  int scrollOffset = 0;
  int lastSelected = -1;
  int lastOffset = -1;
  
  // Размер массива уменьшен до 4
  const char* mainMenuItems[4] = {
    "DIST",
    "RECT", 
    "SETTINGS",
    "SENSORS"
  };
  
public:
  MainMenu(LiquidCrystal_I2C* lcdPtr, ConfigManager* cfg, AppState* statePtr) {
    lcd = lcdPtr;
    config = cfg;
    currentState = statePtr;
  }
 
   void display() {
   
    static int lastSelected = -1;
    static int lastOffset = -1;
    
    // Блок сброса кэша
    if (needMainMenuRedraw) {
      lastSelected = -1; 
      lastOffset = -1;
      needMainMenuRedraw = false; 
      lcd->clear(); 
    }
    // Если ничего не изменилось — ничего не пишем на экран
    if (lastSelected == selectedItem && lastOffset == scrollOffset) {
      return;
    }
    lastSelected = selectedItem; 
    lastOffset = scrollOffset;
    char buf[21];  // 20 символов + '\0'
    
    for (int i = 0; i < 4; i++) {
      int itemIndex = scrollOffset + i;
      
      // Проверяем выход за пределы массива (теперь 4 пункта)
      if (itemIndex >= 4) {
        lcd->setCursor(0, i);
        lcd->print("                    ");  // Очистка пустой строки
        continue;
      }
      
      // Форматируем строку целиком
      const char* status = "";
      if (config->isProcessRunning() && itemIndex == getProcessMenuItem()) {
        status = " (RUN)";
      } else if (!isMenuItemAvailable(itemIndex)) {
        status = " [LOCK]";
      }
      
      snprintf(buf, sizeof(buf), "%s%-10s%s",
               (itemIndex == selectedItem ? ">" : " "),
               mainMenuItems[itemIndex],
               status);
      
      lcd->setCursor(0, i);
      lcd->print(buf);
      
      // Очищаем остаток строки, если текст короче 20 символов
      int len = strlen(buf);
      if (len < 20) {
        lcd->setCursor(len, i);
        for (int j = len; j < 20; j++) {
          lcd->print(" ");
        }
      }
    }
    
    // Запоминаем текущее состояние
    lastSelected = selectedItem;
    lastOffset = scrollOffset;
   
  }
  
  void handleUpButton() {
    selectedItem--;
    // Граница цикла теперь 3 (0..3)
    if (selectedItem < 0) {
      selectedItem = 3;
    }
    updateScroll();
    display();
  }
  
  void handleDownButton() {
    selectedItem++;
    // Граница цикла теперь 3 (0..3)
    if (selectedItem > 3) {
      selectedItem = 0;
    }
    updateScroll();
    display();
  }
  
  void handleSetButton() {
    if (config->isProcessRunning()) {
      // Если процесс запущен, проверяем можно ли войти в этот пункт
      if (!isMenuItemAvailable(selectedItem)) {
        return; // Просто игнорируем, без сообщений
      }
    }
    
    switch(selectedItem) {
      case 0: *currentState = STATE_DIST_MENU; break;
      case 1: *currentState = STATE_RECT_MENU; break;
      // case 2 и 3 удалены (Zator/Autoclave)
      case 2: *currentState = STATE_SETTINGS_MENU; break; // Сдвиг с 4 на 2
      case 3: *currentState = STATE_SENSORS_MENU; break;  // Сдвиг с 5 на 3
    }
  }
  
  void handleBackButton() {
    // В главном меню BACK не делает ничего
  }
  
  int getSelectedItem() {
    return selectedItem;
  }
  
private:
  void updateScroll() {
    if (selectedItem < scrollOffset) {
      scrollOffset = selectedItem;
    } else if (selectedItem >= scrollOffset + 4) {
      scrollOffset = selectedItem - 3;
    }
    
    // Ограничиваем скролл
    // Так как пунктов всего 4, а строк 4, скролла фактически не будет, 
    // но логика остается корректной.
    if (scrollOffset > 0) scrollOffset = 0;
    if (scrollOffset < 0) scrollOffset = 0;
  }
  
  int getProcessMenuItem() {
    ProcessType active = config->getActiveProcess();
    switch(active) {
      case PROCESS_DIST: return 0;
      case PROCESS_RECT: return 1;
      // PROCESS_ZATOR и PROCESS_AUTOCLAVE удалены
      default: return -1;
    }
  }
  
  bool isMenuItemAvailable(int itemIndex) {
    if (!config->isProcessRunning()) return true;
    
    ProcessType active = config->getActiveProcess();
    int processItem = getProcessMenuItem();
    
    // Доступны:
    // 1. Тот же процесс что и запущенный
    // 2. SETTINGS (теперь индекс 2)
    // 3. SENSORS (теперь индекс 3)
    return (itemIndex == processItem) || (itemIndex == 2) || (itemIndex == 3);
  }
};

#endif