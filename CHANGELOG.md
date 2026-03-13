# Журнал изменений BuhloWar

> **Исходник web-интерфейса:** `indexbig.html` — использовать как базу для всех изменений

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
