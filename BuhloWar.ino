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
#include <esp_task_wdt.h>       // Сторожевой таймер задач (этап 2: авто-перезагрузка при зависании loop)
#include <esp_arduino_version.h> // Определяет ESP_ARDUINO_VERSION_MAJOR (совместимость API ядра 2.x / 3.x)
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
  
  // === ИНИЦИАЛИЗАЦИЯ ЛОГЕРА (сразу после SD) ===
  logger.init();
  // =============================================

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
    char tzSign = (tzOffset >= 0) ? '+' : '-';
    int absOffset = (tzOffset >= 0) ? tzOffset : -tzOffset;
    String tzStr = "UTC" + tzSign + String(absOffset);
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

  delay(100);  // пауза для старта Core 0 задачи
  
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
              auto sendCmd = [&](UiCommand cmd) {
                  CommandMessage msg = { cmd, 0 };
                  if (xQueueSend(commandQueue, &msg, 0) != pdTRUE) {
                      Serial.printf("[CmdQueue] FULL! Dropped: %d\n", (int)cmd);
                  }
              };
              Serial.printf("[Cloud] Command: %s\n", command.c_str());
              if (command == "START_DIST") sendCmd(UiCommand::START_DIST);
              else if (command == "START_RECT") sendCmd(UiCommand::START_RECT);
              else if (command == "STOP") sendCmd(UiCommand::STOP_PROCESS);
              else if (command == "UP") sendCmd(UiCommand::UP);
              else if (command == "DOWN") sendCmd(UiCommand::DOWN);
              else if (command == "YES" || command == "DIALOG_YES") sendCmd(UiCommand::DIALOG_YES);
              else if (command == "NO" || command == "DIALOG_NO") sendCmd(UiCommand::DIALOG_NO);
              else if (command == "NEXT_STAGE") sendCmd(UiCommand::NEXT_STAGE);
          });
          logger.log("Cloud: Connected to " + cloudUrl);
      }
      // ==========================================
      
      delay(800);
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
      delay(800);
  } 
  else {
      lcd.setCursor(0, 1); lcd.print("OFFLINE Mode");
      Serial.println("[System] Full OFFLINE - LCD only");
      logger.log("Network: OFFLINE");
      delay(500);
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
  lcd.print("V 2803 ESP32 S3 ");
  delay(800);
  
  mainMenu->display();

  // 8. РАЗРЕШАЕМ ОБРАБОТКУ HTTP ЗАПРОСОВ
  // После этого NetworkTask может безопасно обрабатывать API handlers
  // (processEngine.begin() уже вызван, sensorAdapter != nullptr)
  appNetwork.setSystemReady(true);

  // ================================================================
  // 9. СТОРОЖЕВОЙ ТАЙМЕР loopTask (этап 2 аудита, фикс «зависание без перезагрузки»)
  // ----------------------------------------------------------------
  // Как это работает (для новичка): сторожевой таймер (Watchdog, «сторожевая
  // собака») — это таймер ESP32, который непрерывно тикает. Задача, за которой
  // он следит, обязана периодически его «кормить» (esp_task_wdt_reset). Если
  // кормления нет дольше таймаута — значит задача зависла (бесконечный цикл,
  // ожидание мёртвого датчика и т.п.) — и ESP32 перезагружается сама.
  // Подписываем loopTask (задачу, в которой крутится наш loop() на ядре 1):
  // сейчас зависание процесса никто не замечает — веб на ядре 0 продолжает
  // отвечать, а клапаны остаются в непредсказуемом состоянии.
  // Таймаут 30 с: самая длинная ЛЕГАЛЬНАЯ блокировка в loop() ≈1.5–2 с
  // (запись SD до ~1 с, сообщение в меню датчиков 1.5 с, прогрев при старте
  // процесса 1 с) — запас 15-кратный, ложных срабатываний не будет.
  // API различается между версиями ядра ESP32, поэтому выбор через #if.
  // ================================================================
  #if ESP_ARDUINO_VERSION_MAJOR >= 3
  {
      // Ядро ESP32 3.x (IDF 5.x): таймаут задаётся структурой конфигурации (мс)
      esp_task_wdt_config_t wdtCfg;           // Структура настроек сторожевого таймера
      wdtCfg.timeout_ms = 30000;              // 30 секунд без кормления = зависание
      wdtCfg.idle_core_mask = 0;              // Не следить за idle-задачами — ТОЛЬКО loopTask
      wdtCfg.trigger_panic = true;            // При срабатывании — паника и перезагрузка
      // WDT уже инициализирован системой: меняем его настройки; если вдруг нет — инициализируем
      if (esp_task_wdt_reconfigure(&wdtCfg) != ESP_OK) {
          esp_task_wdt_init(&wdtCfg);
      }
  }
  #else
  {
      // Ядро ESP32 2.x (IDF 4.4): таймаут задаётся числом секунд
      // Система инициализировала WDT сама (5 с, без перезагрузки) — переинициализируем:
      if (esp_task_wdt_init(30, true) != ESP_OK) { // 30 с, panic=true → перезагрузка
          esp_task_wdt_deinit();                   // Не вышло: снимаем системную инициализацию
          esp_task_wdt_init(30, true);             // И ставим свою (30 с, с перезагрузкой)
      }
  }
  #endif
  esp_task_wdt_add(NULL);  // Подписываем текущую задачу (loopTask) на сторожевой таймер
  logger.log("[WDT] loopTask watchdog armed: 30s timeout");
  Serial.println("[WDT] loopTask watchdog armed: 30s timeout");
}

// ================= ОСНОВНОЙ ЦИКЛ =================
void loop() {
  static unsigned long loopStart = 0;
  loopStart = micros();  // Замер времени выполнения

  // Кормим сторожевого пса (этап 2): loop() жив и работает.
  // Вызывается каждый оборот — если loop() где-то зависнет, кормление
  // прекратится и через 30 с ESP32 перезагрузится (см. setup(), шаг 9).
  esp_task_wdt_reset();

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

  // === ПЕРЕХОД AP→STA: инициализация CloudManager при фоновом подключении ===
  // Если WiFi подключился в фоне (после неудачного begin()), AppNetwork
  // устанавливает флаг switchedToSTA. Здесь мы инициализируем CloudManager,
  // который в setup() не был создан (т.к. тогда был AP_MODE).
  if (appNetwork.didSwitchToSTA()) {
      SystemConfig cfg = configManager.getConfig();
      String cloudUrl = cfg.cloudUrl;
      String cloudKey = cfg.cloudApiKey;
      if (cloudUrl.length() > 0 && cloudKey.length() > 0 && !cloudManager.isConfigured()) {
          cloudManager.begin(cloudUrl, cloudKey);
          cloudManager.onCommandReceived([](const String& command, const String& params) {
              auto sendCmd = [&](UiCommand cmd) {
                  CommandMessage msg = { cmd, 0 };
                  if (xQueueSend(commandQueue, &msg, 0) != pdTRUE) {
                      Serial.printf("[CmdQueue] FULL! Dropped: %d\n", (int)cmd);
                  }
              };
              Serial.printf("[Cloud] Command: %s\n", command.c_str());
              if (command == "START_DIST") sendCmd(UiCommand::START_DIST);
              else if (command == "START_RECT") sendCmd(UiCommand::START_RECT);
              else if (command == "STOP") sendCmd(UiCommand::STOP_PROCESS);
              else if (command == "UP") sendCmd(UiCommand::UP);
              else if (command == "DOWN") sendCmd(UiCommand::DOWN);
              else if (command == "YES" || command == "DIALOG_YES") sendCmd(UiCommand::DIALOG_YES);
              else if (command == "NO" || command == "DIALOG_NO") sendCmd(UiCommand::DIALOG_NO);
              else if (command == "NEXT_STAGE") sendCmd(UiCommand::NEXT_STAGE);
          });
          Serial.println("[System] CloudManager initialized (background WiFi connect)");
          logger.log("Cloud: Initialized after background WiFi connect");
      }
  }
  // ==========================================================================

  // 2. Потом ДВИЖОК (обработал команду, обновил температуры, сформировал строки line0-line3)
  processEngine.update(); 

  // 2.5 Облачная синхронизация
  // ----------------------------------------------------------------
  // ФИКС C3 (этап 2): сбор телеметрии НЕ чаще 1 раза в 2 секунды.
  // Как было: buildTelemetryJson() вызывался на КАЖДОМ обороте loop()
  // (сотни раз в секунду). Внутри она собирает JSON из ~40 строк через
  // String: каждый «+=» выделяет память в куче заново и копирует данные.
  // Тысячи аллокаций в секунду фрагментируют кучу — после часов/дней
  // работы это приводило к нестабильности. При этом CloudManager всё
  // равно отправляет данные раз в 2 секунды (telemetryIntervalMs = 2000),
  // т.е. собирать JSON чаще — чистый расход кучи без пользы.
  // ----------------------------------------------------------------
  static unsigned long lastTelemetryMs = 0; // Момент последнего сбора телеметрии (0 = ещё не собирали)
  if (cloudManager.isConfigured() && appNetwork.isOnline()
      && (millis() - lastTelemetryMs >= 2000)) {   // Прошло >= 2 секунд с прошлого сбора?
      lastTelemetryMs = millis();                  // Запоминаем момент текущего сбора
      String telemetry = appNetwork.buildTelemetryJson(); // Собираем JSON (~1КБ)
      cloudManager.update(telemetry);              // Кладём в буфер cloud-задаче + читаем очереди команд/настроек
  }

  // 3. Синхронизация Web -> LCD (переключение меню)
  
  // === Обновление символа сети на LCD (ВСЕГДА, не только при процессе) ===
  // Без этого при фоновом переходе AP→STA символ 'A' на LCD не менялся на 'W'
  static char lastNetSymbol = 0;
  char currentNetSymbol = appNetwork.getNetworkSymbol();
  if (currentNetSymbol != lastNetSymbol) {
      lastNetSymbol = currentNetSymbol;
      processEngine.updateNetworkStatus(currentNetSymbol);
      Serial.printf("[System] Network symbol changed: %c\n", currentNetSymbol);
  }
  // =====================================================================

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