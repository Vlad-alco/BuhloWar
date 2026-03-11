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
#endif