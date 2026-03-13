# Журнал изменений BuhloWar

> **Исходник web-интерфейса:** `indexbig.html` — использовать как базу для всех изменений

---

## [2025-03-13] — Сессия 6

### Добавлено
- **config.h**: Настройки AP режима
  - `AP_SSID = "ESP32"` — имя точки доступа
  - `AP_PASS = "12345678"` — пароль (8 символов минимум)
  - `AP_IP_ADDR = "192.168.4.1"` — фиксированный IP
  - `AP_CHANNEL = 1` — WiFi канал

- **AppNetwork.h**: Enum `NetworkMode` для трёх состояний сети
  - `OFFLINE` — X — нет WiFi, нет AP (только LCD)
  - `AP_MODE` — A — точка доступа, Web работает
  - `STA_MODE` — W — подключено к роутеру

- **AppNetwork.cpp**: Реализация AP режима
  - Метод `startAPMode()` — запуск точки доступа
  - Метод `getNetworkMode()` — получение текущего режима
  - Метод `getNetworkSymbol()` — символ для LCD (W/A/X)
  - WebServer запускается в AP режиме

- **BuhloWar110326CL2core.ino**: Вывод AP информации на LCD
  - STA: `IP: 192.168.x.x`
  - AP: `AP: ESP32` / `IP: 192.168.4.1`
  - OFFLINE: `OFFLINE Mode`

### Логика запуска сети
```
1. Проба подключения к WiFi (STA)
   ↓ неудача
2. Запуск точки доступа (AP)
   ↓ неудача  
3. Полный OFFLINE (только LCD + кнопки)
```

---

## [2025-03-13] — Сессия 5

### Исправлено
- **BuhloWar110326CL2core.ino, AppNetwork.cpp, AppNetwork.h, SDLogger.h**: Перезагрузка при отсутствии SD карты
  - Проблема: `logger.log()` вызывался ДО инициализации SD карты в `setup()`
  - SD карта инициализировалась только в `appNetwork.begin()` (строка 151)
  - Но логи писались уже на строках 63-78
  - Вызов `SD.cardSize()` до `SD.begin()` вызывал краш
  - Решение: добавлен публичный метод `initSD()` для ранней инициализации SD
  - SD инициализируется теперь в начале `setup()` перед первым логом
  - SDLogger улучшен: проверка SD выполняется один раз, при отсутствии SD пишет в Serial

### Изменено
- **AppNetwork.h**: Добавлен публичный метод `initSD()`, переменная `sdInitialized`
- **AppNetwork.cpp**: Вынесена инициализация SD в отдельный метод
- **SDLogger.h**: Добавлен флаг `sdChecked`, логи в Serial при недоступной SD

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
