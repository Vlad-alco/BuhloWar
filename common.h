#ifndef COMMON_H
#define COMMON_H

#include <Arduino.h>

// Состояния приложения
enum AppState {
  STATE_MAIN_MENU,
  STATE_DIST_MENU,
  STATE_RECT_MENU,
  STATE_SETTINGS_MENU,
  STATE_SENSORS_MENU
};

// Процессы системы
enum ProcessType {
  PROCESS_NONE = 0,
  PROCESS_DIST,
  PROCESS_RECT
};

// Режимы редактирования (общие для всех меню настроек)
enum EditMode {
  EDIT_NONE,
  EDIT_VALUE,
  EDIT_SELECT,
  EDIT_FLOAT // Добавил для полноты, хотя в DIST было только VALUE
};

extern bool needMainMenuRedraw;

// === МЬЮТЕКСЫ ОБЩИХ ДАННЫХ (этап 4 аудита, C2) ===
// statusMutex  — защищает SystemStatus (пишет ядро 1, читает ядро 0 через копию)
// configMutex  — защищает SystemConfig (запись Web-потоком, чтение из процесса)
// Объявлены здесь, создаются в setup() (BuhloWar.ino) — как sdMutex.
#include <freertos/semphr.h>
extern SemaphoreHandle_t statusMutex;
extern SemaphoreHandle_t configMutex;
// =================================================
#endif