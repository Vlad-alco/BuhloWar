# BuhloWar - Журнал изменений
Дата: 22.03.2026

## Облачная интеграция (Cloud Integration)

### Созданные файлы:
- `cloud/api.php` - PHP бэкенд для облачного управления
- `CloudManager.h` - клиент для ESP32

### Изменённые файлы:

#### BuhloWar.ino
- Добавлена инициализация CloudManager при старте
- Добавлен callback для обработки команд из облака (START_DIST, START_RECT, STOP, UP, DOWN, YES/NO, NEXT_STAGE)
- Добавлен вызов cloudManager.update() в главном цикле

#### AppNetwork.h / AppNetwork.cpp
- Добавлен метод buildTelemetryJson() для отправки полных данных в облако
- Добавлена обработка cloudUrl и cloudApiKey в handleApiSettings()
- Добавлены поля cloudUrl и cloudApiKey в buildCfgJson()

#### preferences.h / preferences.cpp
- Добавлены адреса EEPROM для облачных настроек (ADDR_CLOUD_ENABLED, ADDR_CLOUD_URL, ADDR_CLOUD_API_KEY)
- Добавлены поля cloudUrl, cloudApiKey, cloudEnabled в SystemConfig
- Добавлены методы writeString() и readString() для строк

### Веб-интерфейс (index.html)
- Добавлена поддержка облачного режима (чтение с хостинга)
- Добавлен переключатель "ОБЛАЧНЫЙ РЕЖИМ" в настройках
- Добавлен баннер "☁️ ОБЛАЧНЫЙ РЕЖИМ" в шапке
- Добавлен индикатор режима сети (W/A/C)
- Исправлена защита от спама команд
- Исправлено автоматическое обновление после команды
- Исправлена проблема с красным индикатором при таймауте

## Исправления багов

### ProcessEngine.cpp
1. **KSS Spit объём** - speedGolovy теперь рассчитывается ВСЕГДА в начале handleGolovy() перед накоплением объёма (ранее был 0 в первой итерации)

2. **Stage name mismatch** - menu_rect.h проверял "VALVE CAL" (с пробелами), ProcessEngine устанавливал "VALVE_CAL" (с подчёркиванием)

3. **ValveCalMenu LCD flicker** - display() вызывался каждый тик во время калибровки

4. **Profile loading** - отсутствовало расширение .json при загрузке профиля

5. **Partial config save** - getInt() возвращал 0 для отсутствующих ключей, стирая остальные настройки

6. **Profile list parsing** - фронтенд ожидал массив, API возвращал {profiles: [...]}

7. **Valve calibration status** - "не калибровался" никогда не обновлялся после сохранения

8. **Auto-open calibration wizard** - мастер калибровки появлялся повторно после сохранения

9. **ESP32 config not reloaded** - loadConfig() не вызывался после сохранения через веб

### AppNetwork.cpp
- Исправлен парсинг облачных настроек (getString lambda)

## Оптимизация

### CloudManager.h
- Интервалы обновления: телеметрия 1 сек, проверка команд 1 сек
- Исправлен JSON парсер команд (корректный поискClosing bracket)
- Добавлен WiFiClientSecure с setInsecure() для HTTPS

### index.html
- Защита от спама команд (0.8 сек между командами)
- Принудительное обновление данных после отправки команды

## Конфигурация облака

### PHP бэкенд (api.php)
- API ключ: BUHLO_SECRET_KEY_2026
- Файл данных: buhlo_data.json
- Файл команд: buhlo_commands.json

### URL для облака
```
https://control.alcodist.ru/api/api.php
```

### Команды облака
- START_DIST - запустить дистилляцию
- START_RECT - запустить ректификацию
- STOP - остановить процесс
- UP/DOWN - навигация
- YES/DIALOG_YES - подтверждение
- NO/DIALOG_NO - отмена
- NEXT_STAGE - следующий этап

---

[ДАТА] — Сессия N (Продолжение)
Задача
Добавить Android приложение с push уведомлениями об авариях. Замена Telegram (заблокирован в РФ).

Решение
Создано Android приложение на Kotlin + Firebase Cloud Messaging.

### Что сделано:

1. **Android приложение** (`android/BuhloWarApp/`)
   - Kotlin, Firebase Messaging
   - Push уведомления об авариях
   - Полный мониторинг (температуры, крепость, объёмы, статусы)
   - Полное управление (ДИСТ, РЕКТ, СТОП, ДАЛЕЕ)
   - Минималистичный UI в тёмных тонах

2. **PHP сервер обновлён** (`cloud/api.php`)
   - Использует Firebase HTTP v1 API (Service Account авторизация)
   - Автоматическая отправка push при авариях (safety_code >= 2)
   - Регистрация устройств для push

3. **Firebase проект**
   - Project ID: buhlowar-a93cd
   - Package: ru.alcodist.buhlowar
   - Service Account файл: buhlowar-a93cd-firebase-adminsdk-fbsvc-*.json

### Изменённые файлы:
- `cloud/api.php` — добавлена отправка push через FCM HTTP v1
- `android/` — создан полный Android проект

### Файлы для хостинга:
```
/api/
├── api.php
└── buhlowar-a93cd-firebase-adminsdk-fbsvc-*.json
```

### Следующие шаги:
1. Android Studio — собрать APK
2. Установить на телефон
3. Ввести URL сервера в приложении

## Известные ограничения
- Интерфейс на SD карте и на хостинге одинаковый
- Для облачного режима нужно включить переключатель в настройках
- ESP32 должен быть подключен к интернету (WiFi STA mode)
- Telegram недоступен в РФ (push заменён на Android приложение)

---

## Исправления 23.03.2026

### ProcessEngine.cpp - Баг KSS Spit
- KSS Spit/Standard/AkaTelo устанавливали `headsVolTarget`, но проверяли `rectVolumeTarget` для завершения
- Добавлено `currentStatus.rectVolumeTarget = (int)headVol` во все функции startGoloovy*

- **Исправлена скорость на KSS Spit**: теперь использует реальную пропускную способность клапана (cap) вместо расчётной speedGolovy
  - Было: `accumSpeed = speedGolovy * corrFactor` (150 мл/ч)
  - Стало: `accumSpeed = cap * 60.0f` (6000 мл/ч при capacity 100 мл/мин)

### AppNetwork.cpp
- Добавлена передача `headsVolTarget` в телеметрию для отображения целевого объёма подэтапа

### ProcessEngine.cpp - minOpenTime
- Удалена захардкоженная константа `MIN_OPEN_TIME_MS = 300`
- Теперь `calcValveTiming()` использует `cfg.minOpenTime` из конфигурации
- Оператор может влиять на минимальное время открытия клапана через веб-интерфейс

### Android приложение
- Исправлен парсинг safety_code (поддержка обоих ключей "safety" и "safety_code")
- Добавлен показ целевого объёма (rectVolumeTarget)
- Обновлён layout для отображения целевого объёма

### Веб-интерфейс (index.html)
- Исправлено сохранение настроек в облачном режиме
- `saveSettings()` теперь отправляет в облако при `cloudMode=true`
- `autoSaveSetting()` теперь работает в облачном режиме

### cloud/api.php
- Добавлен эндпоинт `?settings=1` для сохранения настроек

### CloudManager.h
- Добавлен callback `onSettingsReceived()` для обработки настроек из облака
- Добавлен метод `checkSettings()` для проверки обновлений настроек

### CloudManager.h - Ревизия "тонких мест"
- WiFiClientSecure и HTTPClient перенесены в члены класса (переиспользование)
- `setInsecure()` вызывается один раз в `begin()` вместо каждого запроса
- Все статические строки в Serial.printf заменены на F() макрос
- Удалено создание локальных объектов в методах sendTelemetry, checkCommands, checkSettings

### ВНИМАНИЕ - REVERTED
- CloudManager.h изменения ОТКАТАНЫ — переиспользование WiFiClientSecure/HTTPClient 
  как членов класса вызывало зависание (Watchdog Timeout)
- Возвращена исходная версия с локальными объектами в каждом методе

### AppNetwork.cpp - Оптимизация загрузки веб-интерфейса
- Удалён retry loop с delay(10) при открытии index.html (было до 30мс задержки)
- Файл открывается сразу без повторных попыток
