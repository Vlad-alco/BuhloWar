#ifndef CONFIG_H
#define CONFIG_H

// ================= SYSTEM =================
#define FIRMWARE_VERSION "BuhloWar v2.0"
#define SERIAL_BAUDRATE 115200

// ================= BUTTON PINS (Input) =================
// ВНИМАНИЕ: Требуют внешние pull-up резисторы (~10кОм) на +3.3В!
#define BUTTON_DOWN_PIN  4   // GPIO4
#define BUTTON_UP_PIN    5   // GPIO5
#define BUTTON_SET_PIN   6   // GPIO6
#define BUTTON_BACK_PIN  7   // GPIO7

// ================= I2C BUS (LCD и BME280) =================
// LCD 2004, DS3231 часы, AT24C32 доп память и датчик BME280  используют одну шину I2C.
#define LCD_ADDRESS      0x27    // Адрес I2C конвертера дисплея (может быть 0x3F)
#define BME280_ADDRESS   0x76    // Адрес датчика BME280 SDO = GND(может быть 0x77 SDO = 3.3V), CSB = GND
#define DS3231_ADDRESS   0x68    // адрес Модуль часов реального времени DS3231, Используйте библиотеку RTClib для работы с часами. Она автоматически найдёт DS3231 по адресу 0x68.
#define AT24C32_ADDRESS  0x57 // Используйте адрес, который увидел сканер, Для работы с памятью AT24C32 используйте отдельную библиотеку (например, Adafruit_EEPROM_I2C), передав ей адрес AT24C32_ADDRESS.

#define LCD_COLS 20
#define LCD_ROWS 4
#define I2C_SDA_PIN      8       // GPIO8 для данных (SDA)I2C Шина №1   I2C_SDA_PIN, I2C_SCL_PIN Совместное использование LCD и BME280.
#define I2C_SCL_PIN      9       // GPIO9 для тактов (SCL)

// ================= SENSORS (1-Wire для DS18B20) =================
// Шина для 4x датчиков DS18B20. Резистор 4.7кОм pull-up на +3.3В.
#define ONE_WIRE_BUS     3       // GPIO3

// ================= OUTPUT PINS (Relay Control) =================
// ВСЕ пины подключаются к реле с опторазвязкой.
// Типы клапанов (НО/НЗ) настраиваются в OutputManager.cpp

// ---- Управление нагревом (Регулятор мощности) ----
#define HEATER_PIN1      46      // GPIO46 - ТЭН, канал 1 / режим предустановленного нагрева
#define HEATER_PIN2      18      // GPIO18 - ТЭН, канал 2 / режим полной мощности

// ---- Управление клапанами и контактором ----
#define CONTACTOR_PIN    42      // GPIO42 - Главный силовой контактор SSR № 1 220В
#define VALVE_HEAD_PIN   37      // GPIO37 - Клапан отбора голов 220В силовое реле  № 1
#define VALVE_BODY_PIN   38      // GPIO38 - Клапан отбора тела 220В силовое реле  № 2
#define SPARE_RELAY3     48      // GPIO48 - Запасной пин управления 220В силовое реле  № 3

#define BUZZER_PIN       21     // GPIO21 - Зуммер/сигнализация

// ---- Клапан подачи воды ----
#define VALVE_WATER_PIN  2       // GPIO2  - SSR Реле Клапан подачи воды 12В № 1 нижний ряд
#define MIXER_PIN        47      // GPIO47 - SSR Реле мешалки 12В № 2 нижний ряд 
// Резервные пины (2 шт., гарантированно свободны)
#define SPARE_RELAY1     16      // GPIO16  -  SSR Реле 12В № 3 верхний ряд
#define SPARE_RELAY2     17      // GPIO17  -  SSR Реле 12В № 4 верхний ряд

// ================= SPI для MicroSD Card (HSPI) =================
#define SD_SPI_SCK       14      // GPIO14 - Тактовый (SCK)
#define SD_SPI_MISO      12      // GPIO12 - Данные от карты (MISO)
#define SD_SPI_MOSI      13      // GPIO13 - Данные к карте (MOSI)
#define SD_SPI_CS        15      // GPIO15 - Выбор карты (CS)

// ================= AP MODE (Точка доступа) =================
#define AP_SSID          "ESP32"           // Имя точки доступа
#define AP_PASS          "12345678"        // Пароль (минимум 8 символов)
#define AP_IP_ADDR       "192.168.4.1"     // Фиксированный IP в AP режиме
#define AP_CHANNEL       1                 // WiFi канал
// =============================================================

// ================= MENU & TIMING =================
#define MAIN_MENU_ITEMS  4
#define VISIBLE_LINES    4
#define DEBOUNCE_DELAY   200     // ms
#define PROCESS_UPDATE_INTERVAL 100   // ms
#define SENSOR_UPDATE_INTERVAL  1000  // ms
#define DISPLAY_UPDATE_INTERVAL 500   // ms

// ================= SAFETY LIMITS =================
//#define ABSOLUTE_MAX_TEMP     120.0   // Абсолютный макс. температура (°C)
//#define ABSOLUTE_MIN_TEMP     -10.0   // Абсолютный мин. температура (°C)
//#define MAX_HEATER_ON_TIME    3600000 // Макс. время работы ТЭНа (1 час, ms)

#endif // CONFIG_H