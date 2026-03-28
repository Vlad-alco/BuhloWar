#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>
#include "AppNetwork.h"
#include "config.h"
#include "common.h"
#include "preferences.h"
#include "ProcessCommon.h"
#include "SensorManager.h"
#include "SensorAdapter.h"
#include "menu_main.h"
#include "menu_dist.h"
#include "menu_rect.h"
#include "menu_settings.h"
#include "menu_sensors.h"
#include "ProcessEngine.h"
#include "OutputManager.h"
#include "SDLogger.h"
#include "CloudManager.h"
#include <RTClib.h>
#include <esp_system.h> // Для получения причины сброса
#include <freertos/semphr.h> // Для мьютекса SD карты

SDLogger logger; // Создание глобального объекта

// === ГЛОБАЛЬНЫЙ МЬЮТЕКС ДЛЯ SD КАРТЫ ===
// Защищает SPI шину от одновременного доступа с разных ядер
SemaphoreHandle_t sdMutex = nullptr;
// ========================================

bool needMainMenuRedraw = false;

// ================= ГЛОБАЛЬНЫЕ ОБЪЕКТЫ =================
LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_COLS, LCD_ROWS);
AppState currentState = STATE_MAIN_MENU;

// Менеджеры
SensorManager* sensorManager = nullptr;
SensorAdapter sensorAdapter;
ProcessEngine processEngine;
OutputManager outputManager;
AppNetwork appNetwork;
CloudManager cloudManager;
// Очередь команд: AppNetwork (Core 0) → loop() (Core 1)
QueueHandle_t commandQueue = nullptr;
// Меню
MainMenu* mainMenu = NULL;
DistMenu* distMenu = NULL;
RectMenu* rectMenu = NULL;
SettingsMenu* settingsMenu = NULL;
SensorsMenu* sensorsMenu = NULL;

// Прототипы функций
void checkButtons();
void handleUpButton();
void handleDownButton();
void handleSetButton();
void handleBackButton();

// ================= НАСТРОЙКА =================
void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println("BuhloWar System v2.0");
  
  // === СОЗДАНИЕ МЬЮТЕКСА SD КАРТЫ (ПЕРВЫМ ДЕЛОМ!) ===
  // Должен быть создан ДО любого обращения к SD
  sdMutex = xSemaphoreCreateMutex();
  if (sdMutex == nullptr) {
      Serial.println("[ERROR] Failed to create SD mutex!");
  } else {
      Serial.println("[System] SD mutex created OK");
  }
  // =================================================
  
  // Создаём очередь команд (AppNetwork → loop)
  commandQueue = xQueueCreate(32, sizeof(CommandMessage));

  // === РАННЯЯ ИНИЦИАЛИЗАЦИЯ SD КАРТЫ ===
  // Важно: SD должна быть инициализирована ДО первого logger.log()
  appNetwork.initSD();
  // =====================================

// === АНАЛИЗ ПРИЧИНЫ СБРОСА ===
  esp_reset_reason_t reason = esp_reset_reason();
  
  if (reason == ESP_RST_PANIC) {
      logger.log("!!! SYSTEM CRASH: Kernel Panic (Code Error) !!!");
  } 
  else if (reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT) {
      logger.log("!!! SYSTEM CRASH: Watchdog Timeout (Hang/Infinite Loop) !!!");
  } 
  else if (reason == ESP_RST_POWERON) {
      logger.log("System Boot: Power On (Normal)");
  }
  else if (reason == ESP_RST_SW) {
      logger.log("System Boot: Software Reset (User Command)");
  }
  else {
      logger.log("System Boot: Other Reset");
  }
  logger.log("System Boot Start");
  logger.log("Firmware: " + String(FIRMWARE_VERSION));
  
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

   // === СИНХРОНИЗАЦИЯ ВРЕМЕНИ С DS3231 ===
  RTC_DS3231 rtc;
  if (rtc.begin(&Wire)) {
    DateTime now = rtc.now();
    
    // Получаем настройку часового пояса из конфига
    // Часовой пояс хранится как смещение в часах (например, 3 для Москвы)
    int tzOffset = configManager.getConfig().timezoneOffset;
    
    // ВАЖНО: DS3231 хранит локальное время, а системе нужно UTC.
    // Вычитаем часы, чтобы получить UTC (например, 17:07 - 3ч = 14:07 UTC)
    time_t utcTime = now.unixtime() - (tzOffset * 3600);
    
    struct timeval tv;
    tv.tv_sec = utcTime;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);
    
    // Устанавливаем переменную окружения для часового пояса (чтобы localtime() работал верно)
    // Формат: "UTC-3" для Москвы (знак минус, так как UTC = Local - Offset)
    String tzStr = "UTC" + String(-tzOffset);
    setenv("TZ", tzStr.c_str(), 1);
    tzset();
    
    Serial.println("[System] Time synced from DS3231 (adjusted for TZ)");
    logger.log("Time synced from RTC");
  } else {
    Serial.println("[System] DS3231 not found");
    logger.log("DS3231 RTC not found!");
  }

  
  pinMode(BUTTON_UP_PIN, INPUT_PULLUP);
  pinMode(BUTTON_DOWN_PIN, INPUT_PULLUP);
  pinMode(BUTTON_SET_PIN, INPUT_PULLUP);
  pinMode(BUTTON_BACK_PIN, INPUT_PULLUP);
  
  lcd.init();
  lcd.backlight();
  lcd.clear();
  
  // 1. Загрузка конфигурации (ОБЯЗАТЕЛЬНО ПЕРВЫМ)
  configManager.begin();
  logger.log("Config loaded OK");
  
  // Проверка "призрачного" процесса
  if (configManager.isProcessRunning()) {
    configManager.stopProcess(); 
    Serial.println("Cleared ghost process state on startup");
  }
  
  // 2. Инициализация датчиков
  sensorManager = SensorManager::getInstance();
  sensorManager->begin();
  
  if (sensorAdapter.begin(sensorManager, &Wire)) {
    Serial.println("Sensors: OK");
    logger.log("Sensors: OK");
  } else {
    Serial.println("Sensors: INIT ERROR");
    logger.log("Sensors: INIT ERROR");
  }
  // === ИНИЦИАЛИЗАЦИЯ ЛОГЕРА ===
  logger.init();
  logger.log("System Boot Start");
  // 3. Инициализация сети (WEB и WiFi)
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Starting Web...");
  
  // === ВАЖНО: Запускаем WebServer СРАЗУ (в AP режиме) ===
  // Web доступен по 192.168.4.1 даже до подключения к WiFi!
  appNetwork.startWebServerEarly();
  Serial.println("[System] WebServer started (AP mode)");
  lcd.setCursor(0, 1); lcd.print("Web: 192.168.4.1");

  // === ПРИВЯЗЫВАЕМ processEngine ПЕРЕД ЗАПУСКОМ TASK ===
  // Это должно быть ДО startTask(), иначе processEngine = nullptr
  appNetwork.setEngine(&processEngine, &configManager);
  // =================================================
  
  // === ЗАПУСК NETWORK TASK НА CORE 0 ===
  appNetwork.startTask();
  Serial.println("[System] Network Task started on Core 0");
  // =================================

  delay(500);  // минимальная пауза для старта Core 0 задачи
  
  SystemConfig cfg = configManager.getConfig();
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Connecting WiFi...");
  appNetwork.begin(cfg.chekwifi);

  // === B. ВЫВОД ИНФОРМАЦИИ О СЕТИ ===
  NetworkMode netMode = appNetwork.getNetworkMode();
  Serial.printf("[Cloud] Debug: netMode=%d, cloudUrl='%s', cloudKey='%s'\n", 
      (int)netMode, cfg.cloudUrl, cfg.cloudApiKey);
  
  if (netMode == NetworkMode::STA_MODE) {
      String ip = WiFi.localIP().toString();
      lcd.setCursor(0, 1); 
      lcd.print("IP: " + ip);
      Serial.print("System IP: "); Serial.println(ip);
      logger.log("WiFi Connected: " + ip);
      
      // === ИНИЦИАЛИЗАЦИЯ ОБЛАЧНОГО ПОДКЛЮЧЕНИЯ ===
      String cloudUrl = cfg.cloudUrl;
      String cloudKey = cfg.cloudApiKey;
      if (cloudUrl.length() > 0 && cloudKey.length() > 0) {
          cloudManager.begin(cloudUrl, cloudKey);
          cloudManager.onCommandReceived([](const String& command, const String& params) {
              Serial.printf("[Cloud] Command: %s\n", command.c_str());
              if (command == "START_DIST") {
                  CommandMessage msg = { UiCommand::START_DIST, 0 };
                  xQueueSend(commandQueue, &msg, 0);
              } else if (command == "START_RECT") {
                  CommandMessage msg = { UiCommand::START_RECT, 0 };
                  xQueueSend(commandQueue, &msg, 0);
              } else if (command == "STOP") {
                  CommandMessage msg = { UiCommand::STOP_PROCESS, 0 };
                  xQueueSend(commandQueue, &msg, 0);
              } else if (command == "UP") {
                  CommandMessage msg = { UiCommand::UP, 0 };
                  xQueueSend(commandQueue, &msg, 0);
              } else if (command == "DOWN") {
                  CommandMessage msg = { UiCommand::DOWN, 0 };
                  xQueueSend(commandQueue, &msg, 0);
              } else if (command == "YES" || command == "DIALOG_YES") {
                  CommandMessage msg = { UiCommand::DIALOG_YES, 0 };
                  xQueueSend(commandQueue, &msg, 0);
              } else if (command == "NO" || command == "DIALOG_NO") {
                  CommandMessage msg = { UiCommand::DIALOG_NO, 0 };
                  xQueueSend(commandQueue, &msg, 0);
              } else if (command == "NEXT_STAGE") {
                  CommandMessage msg = { UiCommand::NEXT_STAGE, 0 };
                  xQueueSend(commandQueue, &msg, 0);
              }
          });
          logger.log("Cloud: Connected to " + cloudUrl);
      }
      // ==========================================
      
      delay(1500);  // 1.5 сек вместо 5 сек — достаточно чтобы прочитать IP на LCD
  } 
  else if (netMode == NetworkMode::AP_MODE) {
      lcd.clear();
      lcd.setCursor(0, 0); lcd.print("AP: ");
      lcd.print(AP_SSID);
      lcd.setCursor(0, 1); lcd.print("IP: ");
      lcd.print(AP_IP_ADDR);
      Serial.println("[System] AP Mode active");
      Serial.print("[System] Connect to: "); Serial.println(AP_SSID);
      Serial.print("[System] Password: "); Serial.println(AP_PASS);
      Serial.print("[System] Web: http://"); Serial.println(AP_IP_ADDR);
      logger.log("AP Mode: " + String(AP_SSID));
      delay(1500);  // 1.5 сек вместо 5 сек
  } 
  else {
      lcd.setCursor(0, 1); lcd.print("OFFLINE Mode");
      Serial.println("[System] Full OFFLINE - LCD only");
      logger.log("Network: OFFLINE");
      delay(1000);
  }
  
  // 5. Инициализация ProcessEngine (основная логика)
  processEngine.begin(&lcd, &sensorAdapter, &outputManager, &configManager);
  
  // 6. Инициализация меню
  mainMenu = new MainMenu(&lcd, &configManager, &currentState);
  distMenu = new DistMenu(&lcd, &configManager, &currentState, mainMenu);
  rectMenu = new RectMenu(&lcd, &configManager, &currentState, mainMenu);
  settingsMenu = new SettingsMenu(&lcd, &configManager, &currentState);
  sensorsMenu = new SensorsMenu(&lcd, &configManager, &currentState);

  // 7. Стартовый экран
  lcd.setCursor(0, 0);
  lcd.print("BUHLOWAR SYSTEM");
  lcd.setCursor(0, 1);
  lcd.print("V 2302 ESP32 S3 ");
  delay(1500);
  
  mainMenu->display();
}

// ================= ОСНОВНОЙ ЦИКЛ =================
void loop() {
  static unsigned long loopStart = 0;
  loopStart = micros();  // Замер времени выполнения

  // === ВАЖНО: appNetwork.update() больше НЕ вызывается здесь! ===
  // Network Task запущен на Core 0 через startTask() в setup()
  // loop() работает на Core 1 и не блокируется Telegram
  // ===============================================================

  // 1. Обработка команд из очереди (AppNetwork → ProcessEngine)
  // Команды приходят из Network Task через FreeRTOS Queue
  CommandMessage msg;
  while (xQueueReceive(commandQueue, &msg, 0) == pdTRUE) {
      processEngine.handleCommand(msg.command, msg.param);
  }

  // 2. Потом ДВИЖОК (обработал команду, обновил температуры, сформировал строки line0-line3)
  processEngine.update(); 

  // 2.5 Облачная синхронизация
  if (cloudManager.isConfigured() && appNetwork.isOnline()) {
      String telemetry = appNetwork.buildTelemetryJson();
      cloudManager.update(telemetry);
  }

  // 3. Потом ОБНОВЛЕНИЕ ВЫХОДОВ (реле)
  outputManager.update();

  // 4. Синхронизация Web -> LCD (переключение меню)
  
  static bool wasRunning = false; // Запоминаем, работал ли процесс
  static String lastStageName = "";
  
  if (processEngine.isProcessRunning()) {
     processEngine.updateNetworkStatus(appNetwork.getNetworkSymbol());  // 'W' / 'A' / 'X'
     
     const SystemStatus& status = processEngine.getStatus();
     
     // === ЛОГИКА СТАРТА (Edge Trigger) ===
     if (!wasRunning) {
         wasRunning = true;
         
         ProcessType pt = processEngine.getActiveProcessType();
         
         if (pt == PROCESS_DIST) {
             currentState = STATE_DIST_MENU;
             if (distMenu) {
                 // В DIST ВСЕГДА сначала показываем WATER TEST, чтобы работали кнопки
                 distMenu->setState(DIST_WATER_TEST);
                 distMenu->display();
             }
         } 
         else if (pt == PROCESS_RECT) {
             currentState = STATE_RECT_MENU;
             if (rectMenu) {
                 // В RECT проверяем этап
                 if (status.stageName == "WATER_TEST") {
                     rectMenu->setState(RECT_WATER_TEST);
                 } else {
                     rectMenu->setState(RECT_PROCESS_SCREEN);
                 }
                 rectMenu->display();
             }
         }
     }
     // === ЛОГИКА ПЕРЕХОДА НА VALVE_CAL / SET_PW_AS ===
     else if (currentState == STATE_RECT_MENU && rectMenu) {
         // Обновляем состояние rectMenu при переходе на VALVE_CAL или SET_PW_AS
         if ((status.stageName == "VALVE_CAL" || status.stageName == "SET_PW_AS") && 
             lastStageName != status.stageName) {
             rectMenu->setState(RECT_PROCESS_SCREEN);
             lastStageName = status.stageName;
         }
     }
     
     // Запоминаем текущий этап для следующего цикла
     lastStageName = status.stageName;
  } else {
     // === ЛОГИКА ОСТАНОВКИ ===
     if (wasRunning) {
         wasRunning = false;
         lastStageName = "";  // Сброс при остановке
         
         // Возвращаемся в меню ТОГО процесса, который был активен
         if (currentState == STATE_DIST_MENU && distMenu) {
             distMenu->setState(DIST_MAIN_MENU); 
             distMenu->display();
             Serial.println("[System] DIST stopped. Returning to menu.");
         } 
         else if (currentState == STATE_RECT_MENU && rectMenu) {
             rectMenu->setState(RECT_MAIN_MENU);
             rectMenu->display();
             Serial.println("[System] RECT stopped. Returning to menu.");
         }
     }
  }
  

  // 5. Датчики (не критично, можно и тут)
  sensorAdapter.update();
  
  // 6. Кнопки
  checkButtons();
  
    // 7. Обновление экрана по таймеру (500ms - баланс между отзывчивостью и нагрузкой)
  static unsigned long lastUpdate = 0;
  if (millis() - lastUpdate > 500) {
    lastUpdate = millis();
    
    // === ВАЖНО: Блокируем обновление RectMenu на этапе SET_PW_AS ===
    // Иначе он затирает экран меню SetPwAsMenu
    bool skipRectUpdate = false;
    if (currentState == STATE_RECT_MENU && rectMenu) {
        const SystemStatus& status = processEngine.getStatus();
        // Проверяем оба варианта написания (на всякий случай)
        if (status.stageName == "SET PW & AS" || status.stageName == "SET_PW_AS") {
            skipRectUpdate = true;
        }
    }
    
    if (currentState == STATE_DIST_MENU && distMenu) {
      distMenu->update();
    }
    // Обновляем RectMenu только если разрешено
    if (currentState == STATE_RECT_MENU && rectMenu && !skipRectUpdate) {
      rectMenu->update();
    }
    if (currentState == STATE_SENSORS_MENU && sensorsMenu) {
      sensorsMenu->update();
    }
  }
  
  // === ДИАГНОСТИКА: время выполнения loop ===
  static unsigned long slowLoopCount = 0;
  unsigned long loopTime = micros() - loopStart;
  if (loopTime > 10000) {  // > 10ms - считаем медленным
    slowLoopCount++;
    if (slowLoopCount % 100 == 0) {  // выводим каждые 100 медленных итераций
      Serial.printf("[LOOP] Slow: %u us (count: %u)\n", loopTime, slowLoopCount);
    }
  }
}

// ================= ПРОВЕРКА КНОПОК =================
void checkButtons() {
  static unsigned long lastPress = 0;
  unsigned long currentTime = millis();
  if (currentTime - lastPress < DEBOUNCE_DELAY) return;
  
  if (digitalRead(BUTTON_UP_PIN) == LOW) { lastPress = currentTime; handleUpButton(); }
  if (digitalRead(BUTTON_DOWN_PIN) == LOW) { lastPress = currentTime; handleDownButton(); }
  if (digitalRead(BUTTON_SET_PIN) == LOW) { lastPress = currentTime; handleSetButton(); }
  if (digitalRead(BUTTON_BACK_PIN) == LOW) { lastPress = currentTime; handleBackButton(); }
}

// ================= ОБРАБОТЧИКИ КНОПОК =================
void handleUpButton() {
  const SystemStatus& status = processEngine.getStatus();
  bool isProcessScreen = false;
  if (currentState == STATE_DIST_MENU && distMenu && distMenu->getState() == DIST_PROCESS_SCREEN) isProcessScreen = true;
  if (currentState == STATE_RECT_MENU && rectMenu && rectMenu->getState() == RECT_PROCESS_SCREEN) isProcessScreen = true;
  
  // === ВАЖНО: VALVE_CAL и SET_PW_AS всегда передают управление в ProcessEngine ===
  if (status.stageName == "VALVE_CAL" || status.stageName == "SET_PW_AS") {
      isProcessScreen = true;
  }

  if (processEngine.isProcessRunning() && isProcessScreen) {
      if (status.stageName == "VALVE_CAL" || status.stageName == "SET_PW_AS") {
          processEngine.handleUiUp();
          return;
      }
  }
  switch(currentState) {
    case STATE_MAIN_MENU: if (mainMenu) mainMenu->handleUpButton(); break;
    case STATE_DIST_MENU: if (distMenu) distMenu->handleUpButton(); break;
    case STATE_RECT_MENU: if (rectMenu) rectMenu->handleUpButton(); break;
    case STATE_SETTINGS_MENU: if (settingsMenu) settingsMenu->handleUpButton(); break;
    case STATE_SENSORS_MENU: if (sensorsMenu) sensorsMenu->handleUpButton(); break;
    default: break;
  }
}

void handleDownButton() {
  const SystemStatus& status = processEngine.getStatus();
  bool isProcessScreen = false;
  if (currentState == STATE_DIST_MENU && distMenu && distMenu->getState() == DIST_PROCESS_SCREEN) isProcessScreen = true;
  if (currentState == STATE_RECT_MENU && rectMenu && rectMenu->getState() == RECT_PROCESS_SCREEN) isProcessScreen = true;
  
  // === ВАЖНО: VALVE_CAL и SET_PW_AS всегда передают управление в ProcessEngine ===
  if (status.stageName == "VALVE_CAL" || status.stageName == "SET_PW_AS") {
      isProcessScreen = true;
  }

  if (processEngine.isProcessRunning() && isProcessScreen) {
      if (status.stageName == "VALVE_CAL" || status.stageName == "SET_PW_AS") {
          processEngine.handleUiDown();
          return;
      }
  }
  switch(currentState) {
    case STATE_MAIN_MENU: if (mainMenu) mainMenu->handleDownButton(); break;
    case STATE_DIST_MENU: if (distMenu) distMenu->handleDownButton(); break;
    case STATE_RECT_MENU: if (rectMenu) rectMenu->handleDownButton(); break;
    case STATE_SETTINGS_MENU: if (settingsMenu) settingsMenu->handleDownButton(); break;
    case STATE_SENSORS_MENU: if (sensorsMenu) sensorsMenu->handleDownButton(); break;
    default: break;
  }
}

void handleSetButton() {
  const SystemStatus& status = processEngine.getStatus();
  bool isProcessScreen = false;
  if (currentState == STATE_DIST_MENU && distMenu && distMenu->getState() == DIST_PROCESS_SCREEN) isProcessScreen = true;
  if (currentState == STATE_RECT_MENU && rectMenu && rectMenu->getState() == RECT_PROCESS_SCREEN) isProcessScreen = true;
  
  // === ВАЖНО: VALVE_CAL и SET_PW_AS всегда передают управление в ProcessEngine ===
  // Даже если rectMenu->state не RECT_PROCESS_SCREEN
  if (status.stageName == "VALVE_CAL" || status.stageName == "SET_PW_AS") {
      isProcessScreen = true;
  }

  if (processEngine.isProcessRunning() && isProcessScreen) {
      // VALVE_CAL, SET_PW_AS - свои меню, GOLOVY_OK - диалог подтверждения
      if (status.stageName == "VALVE_CAL" || status.stageName == "SET_PW_AS" || status.stageName == "GOLOVY_OK") {
          processEngine.handleUiSet();
          return;
      }
  }
  switch(currentState) {
    case STATE_MAIN_MENU:
      if (mainMenu) mainMenu->handleSetButton();
      break;
    case STATE_DIST_MENU: 
      if (distMenu) distMenu->handleSetButton(); 
      break;
    case STATE_RECT_MENU: 
      if (rectMenu) rectMenu->handleSetButton(); 
      break;
    case STATE_SETTINGS_MENU: 
      if (settingsMenu) settingsMenu->handleSetButton(); 
      break;
    case STATE_SENSORS_MENU: 
      if (sensorsMenu) sensorsMenu->handleSetButton(); 
      break;
    default: break;
  }
}

void handleBackButton() {
  // Особый случай: WATER_TEST
  // Если процесс запущен и мы на этапе WATER_TEST, Back должен отменить процесс.
  const SystemStatus& status = processEngine.getStatus();
  
  // Проверяем, находимся ли мы в контексте экрана процесса
  bool isProcessScreen = false;
  if (currentState == STATE_DIST_MENU && distMenu && distMenu->getState() == DIST_PROCESS_SCREEN) isProcessScreen = true;
  if (currentState == STATE_RECT_MENU && rectMenu && rectMenu->getState() == RECT_PROCESS_SCREEN) isProcessScreen = true;
  
  // === ВАЖНО: VALVE_CAL и SET_PW_AS всегда передают управление в ProcessEngine ===
  if (status.stageName == "VALVE_CAL" || status.stageName == "SET_PW_AS") {
      isProcessScreen = true;
  }

  if (processEngine.isProcessRunning() && isProcessScreen) {
      // Если это этап с диалогом -> передаем движку
      if (status.stageName == "WATER_TEST" || status.stageName == "REPLACEMENT" || 
          status.stageName == "VALVE_CAL" || status.stageName == "SET_PW_AS" || status.stageName == "GOLOVY_OK") {
          processEngine.handleUiBack();
          return;
      }
      // Если это обычный этап (RAZGON, OTBOR, TELO) -> просто выходим в меню, процесс продолжает работать
  }

  // Стандартная логика меню
  switch(currentState) {
    case STATE_MAIN_MENU: if (mainMenu) mainMenu->handleBackButton(); break;
    case STATE_DIST_MENU:
      if (distMenu) { distMenu->handleBackButton(); if (currentState == STATE_MAIN_MENU && mainMenu) mainMenu->display(); }
      break;
    case STATE_RECT_MENU:
      if (rectMenu) { rectMenu->handleBackButton(); if (currentState == STATE_MAIN_MENU && mainMenu) mainMenu->display(); }
      break;
    case STATE_SETTINGS_MENU:
      if (settingsMenu) { settingsMenu->handleBackButton(); if (currentState == STATE_MAIN_MENU && mainMenu) mainMenu->display(); }
      break;
    case STATE_SENSORS_MENU:
      if (sensorsMenu) { sensorsMenu->handleBackButton(); if (currentState == STATE_MAIN_MENU && mainMenu) mainMenu->display(); }
      break;
    default: break;
  }
}