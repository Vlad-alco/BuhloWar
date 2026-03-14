# Журнал изменений BuhloWar

> **Исходник web-интерфейса:** `indexbig.html` — использовать как базу для всех изменений

---

## [2025-03-14] — Сессия 13

### Задача
Web UI для настроек уведомлений + исправление критического бага.

### Решение

#### 1. Web UI уведомлений
Добавлена кнопка "🔔 УВЕДОМЛЕНИЯ" в "Общие настройки", открывающая диалоговую панель:
- Переключатели Telegram/VK (вкл/выкл)
- Категории уведомлений (Система, Дистилляция, Ректификация, Датчики)
- Сохранение через `/api/settings`

#### 2. Циклическая зависимость
При компиляции возникла ошибка: forward declaration `enum class NotifyCategory;` в AppNetwork.h не давал доступа к значениям enum.

**Решение**: вынести `NotifyCategory` в отдельный файл `NotifyCategory.h`.

#### 3. ⚠️ КРИТИЧЕСКИЙ БАГ — частичное обновление настроек
**Проблема**: `handleApiSettings()` присваивал значения ВСЕМ полям, даже если их нет в JSON. При сохранении уведомлений (6 полей) остальные ~40 полей затирались нулями/false.

**Причина**: `cfg.field = getInt("field")` возвращает 0, если ключ отсутствует.

**Решение**: добавлена функция `hasKey()`, все присваивания обёрнуты в `if (hasKey("field"))`.

#### 4. AI_RULES.md
Добавлено главное правило (Приоритет №1):
> **ЛЮБЫЕ изменения НЕ ДОЛЖНЫ ломать логику и принципы работы других блоков кода!**

### Изменённые файлы
- **indexbig.html**: кнопка уведомлений, диалоговая панель, JS функции
- **NotifyCategory.h** (новый): enum NotifyCategory
- **ProcessEngine.h**: include NotifyCategory.h вместо enum
- **AppNetwork.h**: include NotifyCategory.h
- **AppNetwork.cpp**: hasKey() + проверки перед присваиванием
- **AI_RULES.md**: главное правило + пример нарушения
- **preferences.cpp**: include изменения

### Урок
Перед изменением общего кода (handleApiSettings) проверять все пути вызова и форматы данных.

---

## [2025-03-14] — Сессия 12

### Задача
VK интеграция: перенос настроек уведомлений из SD карты в Preferences (EEPROM).

### Решение
Настройки категорий уведомлений (notify_system, notify_dist, notify_rect, notify_sensors) и флаги включения мессенджеров (tgEnabled, vkEnabled) хранились в SD карте (/wifi_config.txt), что приводило:
- Износ SD карты при частых перезаписях
- Медленный доступ
- Низкая надёжность

**Правильное решение**: хранить эти настройки в Preferences (EEPROM ESP32).

### Изменённые файлы
- **preferences.h**:
  - Добавлены адреса EEPROM: `ADDR_TG_ENABLED` ... `ADDR_NOTIFY_SENSORS`
  - Добавлены поля в `SystemConfig`: `tgEnabled`, `vkEnabled`, `notifySystem`, `notifyDistillation`, `notifyRectification`, `notifySensors`
  - Добавлен метод `saveNotifyConfig()`
- **preferences.cpp**:
  - Загрузка 6 полей настроек уведомлений в `loadConfig()`
  - Реализация `saveNotifyConfig()` — сохранение в EEPROM
- **AppNetwork.h**:
  - Удалены дублирующие поля (notifySystem, etc.)
  - Добавлен комментарий о переносе настроек в SystemConfig
- **AppNetwork.cpp**:
  - Убрано чтение `notify_*` из `wifi_config.txt`
  - `shouldSendCategory()` — читает из `configManager->getConfig()`
  - `sendNotification()` — учитывает `tgEnabled/vkEnabled` из SystemConfig
  - `buildCfgJson()` — добавлены 6 полей настроек
  - `handleApiSettings()` — добавлено чтение и сохранение настроек

### Категории уведомлений
| Категория | Поведение |
|-----------|----------|
| ALARM, PROCESS_BASIC, ATTENTION | **ВСЕГДА** отправляются |
| SYSTEM | Настраиваемый (notifySystem) |
| DISTILLATION | Настраиваемый (notifyDistillation) |
| RECTIFICATION | Настраиваемый (notifyRectification) |
| SENSORS | Настраиваемый (notifySensors) |

---

## [2025-03-14] — Сессия 11

### Проблема
Неверный расчёт отображаемого объёма голов в блоке РАСЧЕТЫ:
- Spit: расчётный объём не соответствовал реальному отбору
- Standard: скорость всегда показывала 225 мл/час, независимо от реального расхода
- AkaTelo: скорость показывала правильно (2250 мл/час)

### Причина
Время и объём рассчитывались по разным формулам:

| Подэтап | Время рассчитывалось по | Объём рассчитывался по |
|---------|------------------------|------------------------|
| KSS_SPIT | `valve_head_capacity` ✓ | `speedGolovy` (эмпирика) ❌ |
| KSS_STANDARD | `speedGolovy` (эмпирика) | `speedGolovy` (эмпирика) ❌ |
| KSS_AKATELO | `speedTelo` (эмпирика) | `speedTelo` (эмпирика) |
| ST_MAIN | `speedGolovy` (эмпирика) | `speedGolovy` (эмпирика) ❌ |

Где:
- `speedGolovy = koff * 50` мл/час (koff = power / 1000)
- `speedTelo = koff * 500` мл/час

**Проблема:** Эти эмпирические формулы не учитывают реальный расход клапана и duty cycle при циклировании.

### Решение — Вариант A (реализован)

Исправлен только расчёт отображаемого объёма. Время этапов осталось как есть.

#### Новые формулы расчёта скорости:

```cpp
// KSS_SPIT: клапан открыт постоянно → полный расход
speed = valve_head_capacity * 60  // мл/мин → мл/час

// KSS_STANDARD: клапан циклирует с таймингами голов
dutyCycle = (headOpenMs * koff) / (headOpenMs * koff + headCloseMs)
speed = valve_head_capacity * dutyCycle * 60  // мл/час

// KSS_AKATELO: клапан циклирует с таймингами тела
dutyCycle = (bodyOpenMs * koff) / (bodyOpenMs * koff + bodyCloseMs)
speed = valve_body_capacity * dutyCycle * 60  // мл/час

// ST_MAIN (Standard метод, не KSS): как KSS_STANDARD
dutyCycle = (headOpenMs * koff) / (headOpenMs * koff + headCloseMs)
speed = valve_head_capacity * dutyCycle * 60  // мл/час
```

#### Пример расчёта (koff = 4.5, valve_head_capacity = 9 мл/мин, headOpenMs = 1, headCloseMs = 9):

| Подэтап | Duty Cycle | Скорость (было) | Скорость (стало) |
|---------|------------|-----------------|------------------|
| SPIT | 100% | 225 мл/час | 540 мл/час |
| STANDARD | 33% (4.5/13.5) | 225 мл/час | 180 мл/час |

### Вариант B (запасной, на будущее)

Если Вариант A не даст точного результата — пересчитать также **время этапов** по реальному расходу:

```cpp
// KSS_SPIT
float speedMlMin = valve_head_capacity;  // мл/мин
golovyTargetTime = (headVol / speedMlMin) * 60;  // секунды
accumSpeed = speedMlMin * 60;  // мл/час

// KSS_STANDARD
float dutyCycle = (headOpenMs * koff) / (headOpenMs * koff + headCloseMs);
float speedMlMin = valve_head_capacity * dutyCycle;
golovyTargetTime = (headVol / speedMlMin) * 60;
accumSpeed = speedMlMin * 60;
```

**Плюсы B:** Полная согласованность — время и объём совпадают.
**Минусы B:** Время этапов изменится, нужно заново калибровать capacity.

### Изменённые файлы
- **ProcessEngine.cpp**: функция `handleGolovy()` — новый расчёт `accumSpeed` для каждого подэтапа

---

## [2025-03-13] — Сессия 10

### Проблемы
1. Web интерфейс не открывается в AP режиме (192.168.4.1)
2. Система в AP режиме постоянно пытается подключиться к роутеру → ошибки `wifi:sta is connecting, cannot set config`
3. Медленный Web интерфейс в AP режиме
4. LCD показывает "X" вместо "A" в AP режиме

### Решение

#### 1. DNS сервер для AP режима (Captive Portal)
ESP32 в AP режиме не отвечал на DNS запросы — многие устройства не могли открыть страницу.
```cpp
// AppNetwork.h
#include <DNSServer.h>
DNSServer* dnsServer = nullptr;

// AppNetwork.cpp - startAPMode()
dnsServer = new DNSServer();
dnsServer->start(53, "*", WiFi.softAPIP());  // Перенаправляем все DNS на наш IP

// update()
if (dnsServer && networkMode == NetworkMode::AP_MODE) {
    while (dnsServer->processNextRequest() && dnsProcessed < 10) { dnsProcessed++; }
}
```

#### 2. AP режим без авто-переключения
Убраны попытки подключения к роутеру в AP режиме — ESP32 не может одновременно работать как AP и STA.
```cpp
} else if (networkMode == NetworkMode::AP_MODE) {
    // Режим AP: работаем до перезагрузки
    // Пользователь должен исправить настройки WiFi и перезагрузить систему
}
```

#### 3. Ускорение Web в AP режиме
```cpp
// Задержка в task:
if (networkMode == NetworkMode::AP_MODE) {
    vTaskDelay(pdMS_TO_TICKS(2));  // Быстро для AP
} else {
    vTaskDelay(pdMS_TO_TICKS(10)); // Нормально для STA
}

// DNS запросы обрабатываются пачкой (до 10 за цикл)
```

#### 4. Исправление символа сети на LCD
LCD получал `boolean isOnline` вместо символа режима.
```cpp
// ProcessEngine.h - было:
void updateNetworkStatus(bool online);

// ProcessEngine.h - стало:
void updateNetworkStatus(char networkSymbol);  // 'W' / 'A' / 'X'

// BuhloWar...ino:
processEngine.updateNetworkStatus(appNetwork.getNetworkSymbol());

// ProcessEngine.cpp - line0:
snprintf(buf, ..., currentStatus.networkSymbol.c_str());  // W/A/X
```

### Таблица режимов сети
| Режим | LCD | Web | Telegram | DNS |
|-------|-----|-----|----------|-----|
| STA (роутер) | W | ✅ | ✅ | ❌ |
| AP (точка) | A | ✅ | ❌ | ✅ |
| OFFLINE | X | ❌ | ❌ | ❌ |

### Изменённые файлы
- **AppNetwork.h**: добавлен `#include <DNSServer.h>`, `DNSServer* dnsServer`
- **AppNetwork.cpp**:
  - `startAPMode()` — создание DNS сервера
  - `update()` — обработка DNS запросов, убраны попытки подключения к роутеру
  - `networkTaskWrapper()` — динамическая задержка (2мс AP / 10мс STA)
- **ProcessEngine.h**: `updateNetworkStatus(char)` вместо `updateNetworkStatus(bool)`
- **ProcessEngine.cpp**: использует `networkSymbol` в line0
- **ProcessCommon.h**: `networkSymbol = "X"` по умолчанию
- **BuhloWar110326CL2core.ino**: передаёт `getNetworkSymbol()` в ProcessEngine

---

## [2025-03-13] — Сессия 9

### Проблема
1. Telegram сообщение "System BuhloWar v2.0 started" приходит каждую минуту
2. Web интерфейс отвечает очень медленно
3. ESP32 перезагружается каждые ~10 минут

### Причина
После изменений сессий 6-7 возникли проблемы:
- `checkInternet()` блокировал loop на 5-10 секунд без handleClient()
- `sendTelegramNow()` имел слишком короткий таймаут (1 сек), что приводило к retry
- Утечка памяти: `new UniversalTelegramBot` без delete старого при переключении AP→STA
- Очередь Telegram: сообщения повторялись бесконечно без лимита попыток

### Решение

#### 1. checkInternet() — неблокирующая проверка
```cpp
bool AppNetwork::checkInternet() {
    WiFiClient testClient;
    testClient.setTimeout(2000);  // 2 секунды максимум
    for (int i = 0; i < 20; i++) {  // 20 * 100ms = 2 сек
        if (testClient.connect("google.com", 80)) return true;
        if (server) server->handleClient();  // WebServer работает!
        delay(100);
        yield();
    }
    return false;
}
```
**Эффект**: Web остаётся отзывчивым во время проверки интернета

#### 2. sendTelegramNow() — таймаут 2 секунды
```cpp
client.setTimeout(2000);  // Было 1 сек, нужно 2000 мс
```

#### 3. Лимит попыток на сообщение
```cpp
#define TG_MAX_RETRIES 3  // Новая константа

struct TgMessage {
    String text;
    unsigned long timestamp;
    int retryCount = 0;  // Счётчик попыток
};
```
**Эффект**: После 3 неудач сообщение удаляется из очереди

#### 4. Исправление утечки памяти
```cpp
if (bot) {
    delete bot;  // Удаляем старый перед созданием нового
    bot = nullptr;
}
bot = new UniversalTelegramBot(tgToken, client);
```

#### 5. Индикатор режима сети в Web
- Добавлено поле `network_mode` в JSON (W/A/X)
- В шапке Web интерфейса появился бейдж с режимом сети
- Зелёный W = роутер, жёлтый A = AP, серый X = офлайн

### Изменённые файлы
- **AppNetwork.h**: добавлен `TG_MAX_RETRIES`, поле `retryCount` в TgMessage
- **AppNetwork.cpp**:
  - `checkInternet()` — неблокирующая проверка с handleClient()
  - `sendTelegramNow()` — таймаут 2000 мс
  - `processMessageQueue()` — учёт retryCount, удаление после 3 попыток
  - `update()` — delete старого bot перед new
  - `handleApiStatus()` — добавлено поле network_mode
- **index.html**: индикатор network_mode в шапке

---

## [2025-03-13] — Сессия 8

### Задача
Создать документ со схемой подключения системы на основе кода.

### Решение
Создан файл `CONNECTION_SCHEMA.md` с полной схемой подключения:
- Таблица всех GPIO пинов
- I2C устройства с адресами
- Датчики DS18B20
- Реле 220В и 12В
- SPI для SD карты
- Схема питания
- Типы клапанов

### Добавленные файлы
- **CONNECTION_SCHEMA.md**: полная документация по подключению

---

## [2025-03-13] — Сессия 7

### Задача
Если WiFi подключён к роутеру, но интернета нет — Web должен продолжать работать. Раньше система отключала WiFi при потере интернета.

### Решение
- Разделены понятия: `wifiConnected` (WiFi к роутеру) и `online` (интернет)
- Проверка WiFi и интернета теперь происходит отдельно
- При потере интернета: Web работает, Telegram/NTP отключены
- При потере WiFi: переключение в AP режим

### Таблица состояний
| WiFi | Интернет | Режим | Web | Telegram |
|------|----------|-------|-----|----------|
| ✅ | ✅ | STA (W) | ✅ | ✅ |
| ✅ | ❌ | STA (W) | ✅ | ❌ |
| ❌ | ❌ | AP (A) или OFFLINE (X) | зависит | ❌ |

### Изменённые файлы
- **AppNetwork.h**: добавлена переменная `wifiConnected`
- **AppNetwork.cpp**: переписана функция `update()` с раздельной проверкой WiFi и интернета

---

## [2025-03-13] — Сессия 6

### Задача
При невозможности подключения к WiFi запустить точку доступа (AP) для доступа к Web-интерфейсу. Если и AP не удалось — работать только на LCD + кнопки.

### Решение
- Добавлен enum `NetworkMode` с тремя состояниями: OFFLINE / AP_MODE / STA_MODE
- Метод `startAPMode()` запускает точку доступа с параметрами из config.h
- WebServer работает в AP режиме по фиксированному IP 192.168.4.1
- LCD показывает символ режима: W (роутер), A (AP), X (офлайн)

### Изменённые файлы
- **config.h**: `AP_SSID="ESP32"`, `AP_PASS="12345678"`, `AP_IP_ADDR="192.168.4.1"`
- **AppNetwork.h**: enum `NetworkMode`, методы `getNetworkMode()`, `getNetworkSymbol()`, `startAPMode()`
- **AppNetwork.cpp**: реализация AP режима, запуск WebServer в AP
- **BuhloWar110326CL2core.ino**: вывод информации о режиме сети на LCD

---

## [2025-03-13] — Сессия 5

### Задача
Система уходила в перезагрузку если SD карта не обнаружена при старте.

### Решение
- Проблема: `logger.log()` вызывался до инициализации SD карты
- SD инициализировалась в `appNetwork.begin()`, но логи писались раньше
- Вызов `SD.cardSize()` до `SD.begin()` вызывал краш
- Решение: метод `initSD()` вызывается в начале `setup()` перед первым логом

### Изменённые файлы
- **AppNetwork.h**: добавлен публичный метод `initSD()`, переменная `sdInitialized`
- **AppNetwork.cpp**: вынесена инициализация SD в отдельный метод
- **SDLogger.h**: флаг `sdChecked`, логи в Serial с префиксом `[NO SD]` при недоступной SD
- **BuhloWar110326CL2core.ino**: `initSD()` вызывается первым в `setup()`

---

## [2025-03-12] — Сессия 4

### Исправлено
- **index_v2.html**: Диалог SET_PW_AS не появлялся после WATER_TEST
  - Проблема: `closeDialogPanel()` устанавливал `userClosedSetPwAs = true` при закрытии ЛЮБОГО диалога
  - Решение: Разделил функции `closeDialogPanel()` и `closeSetPwAsPanel()`
  - Флаг `userClosedSetPwAs` теперь устанавливается только при нажатии "ОТМЕНА" в диалоге SET_PW_AS
  - Добавлен автоматический сброс флага при смене stage с SET_PW_AS на другой

---

## [2025-03-12] — Сессия 3

### Добавлено
- **AppNetwork.cpp**: Добавлено поле `headsTotalTarget` в JSON
  - Расчёт: KSS метод = 20% от AS, Standard метод = 10% от AS
  - Используется для отображения общего объёма голов в блоке РАСЧЕТЫ

- **index_v2.html**: Новый файл web-интерфейса
  - Модальные окна (.modal-overlay + .hidden) заменены на диалоговые панели (dialog-panel)
  - Панели управляются через `style.display = 'flex'/'none'` вместо класса hidden
  - Цель: избежать проблем с кешированием браузером

- **ProcessEngine.cpp**: Добавлен расчёт `rectTimeRemaining` для этапа TELO
  - Формула: `predictVol / speedShpora` в часах, конвертация в секунды
  - Вычитается уже пройденное время `stageTimeSec`

### Исправлено
- **index.html, index_v2.html**: Скорость отбора голов (headsSpeed) всегда показывала 0
  - Причина: использовался `bodySpeed` вместо `headsSpeed`
  - Исправлено на `currentData.headsSpeed`

- **index.html, index_v2.html**: ГОЛОВЫ в блоке РАСЧЕТЫ показывали объём подэтапа вместо общего
  - Теперь отображается `headsTotalTarget` — общий объём голов на весь этап

---

## [2025-03-12] — Сессия 2

### Добавлено
- **index.html**: Обновлён блок РАСЧЕТЫ в мониторе
  - Добавлены поля для отображения метода, подэтапа, объёма, оставшегося времени

### Исправлено
- **index.html**: Мини-индикаторы в шапке (нагрев/мешалка/вода)
  - Корректное отображение статусов устройств

---

## [2025-03-12] — Сессия 1

### Добавлено
- **index_v2.html**: Создан альтернативный web-интерфейс
  - 5 диалоговых панелей вместо модальных окон:
    - `dialog-panel` — универсальный диалог
    - `alert-panel` — тревога
    - `save-profile-panel` — сохранение профиля
    - `load-profile-panel` — загрузка профиля
    - `calc-panel` — расчёт клапана

### Изменено
- CSS стили для `.dialog-panel` и вложенных элементов
- JavaScript: функции `showPage()`, `checkModals()` адаптированы для работы с панелями

---

## Примечания

### Структура диалоговых панелей
```html
<div id="dialog-panel" class="dialog-panel">
    <div class="dialog-backdrop" onclick="closeDialogPanel()"></div>
    <div class="dialog-content">
        <h3 id="dialog-title"></h3>
        <p id="dialog-text"></p>
        <div id="dialog-buttons"></div>
    </div>
</div>
```

### Переменные состояния
- `userClosedSetPwAs` — флаг закрытия диалога SET_PW_AS пользователем
- `lastStage` — предыдущее значение stage для отслеживания переходов

---
