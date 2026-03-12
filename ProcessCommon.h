#ifndef PROCESS_COMMON_H
#define PROCESS_COMMON_H

#include <Arduino.h>
#include "common.h"

// ==================== СТАТУСЫ ДАТЧИКОВ ====================
enum class SensorStatus {
    OK,         // Данные валидны
    OFF,        // Датчик не отвечает (потеря связи)
    NOT_FOUND,  // Датчик не обнаружен при инициализации
    N_USE       // Не используется в текущем процессе
};

// ==================== СОСТОЯНИЯ БЕЗОПАСНОСТИ ====================
enum class SafetyState {
    NORMAL,             // Все в норме
    WARNING_TSA,        // TSA > tsaLimit, идет отсчет vreac
    WARNING_BOX,        // boxTemp > boxMaxTemp
    EMERGENCY           // Авария (истек vreac, TSA все еще превышена)
};

// ==================== ТИПЫ АВАРИЙ ====================
enum class AlarmType {
    NONE,
    ALARM_TSA,          // 3 коротких сигнала каждые 30 сек
    ATTENTION_BOX       // 2 коротких сигнала каждые 30 сек
};

// ==================== СТРУКТУРА ДАННЫХ ДАТЧИКОВ ====================
struct SensorData {
    struct TempSensor {
        float value = -127.0f;         // Фильтрованное значение температуры
        SensorStatus status = SensorStatus::NOT_FOUND;
        String name = "";
    };
    
    // DS18B20 датчики
    TempSensor tsa;     // Температура в царге (верх)
    TempSensor aqua;    // Температура дистиллята/воды
    TempSensor tsar;    // Температура в царге (для ректификации)
    TempSensor tank;    // Температура в кубе
    
    // BME280 данные
    float boxTemp = -127.0f;    // Температура окружающей среды
    float pressure = 0.0f;      // Атмосферное давление (гПа)
    float humidity = 0.0f;      // Влажность (%)
    SensorStatus bmeStatus = SensorStatus::NOT_FOUND;
    
    unsigned long timestamp = 0; // Время считывания (мс)
    
    // Методы валидации
    bool isTsaValid() const { return tsa.status == SensorStatus::OK; }
    bool isTsarValid() const { return tsar.status == SensorStatus::OK; }
    bool isTankValid() const { return tank.status == SensorStatus::OK; }
    bool isAquaValid() const { return aqua.status == SensorStatus::OK; }
    bool isBmeValid() const { return bmeStatus == SensorStatus::OK; }
};

// ==================== СТАТУС СИСТЕМЫ ДЛЯ UI ====================
struct SystemStatus {
    // Текущий процесс
    ProcessType activeProcess = PROCESS_NONE;
    bool isRunning = false;
    bool isPaused = false;
    
    // Этап процесса
    String stageName = "IDLE";         // "RAZGON", "WAITING", "OTBOR", "TELO" и т.д.
    String stageInfo = "";             // Доп. информация: "ST", "1", "1.33", "300"
    
    // Отформатированные строки для LCD (20 символов каждая)
    String line0 = ""; // Пример: "36.19 DIST  -     40"
    String line1 = ""; // Пример: "23.81 OTBOR       85"
    String line2 = ""; // Пример: "34.06 NORMA       "
    String line3 = ""; // Пример: "23.81 00:56       98"
    
    // Безопасность
    SafetyState safety = SafetyState::NORMAL;
    AlarmType alarm = AlarmType::NONE;
    int alarmTimerSec = 0;             // Остаток времени vreac (секунды)
    
     // === НОВОЕ: Флаг доступности BME280 ===
    bool bmeAvailable = true; 
    // ====================================
    
    // Сеть
    bool isOnline = false;
    String networkSymbol = "-";        // "+" онлайн, "-" оффлайн

    // Блокировка запуска процесса (заполняется при отказе checkSensorsReady)
    String sensorErrorMsg = "";        // Список датчиков с проблемой, например "TSA TANK"
        // === Статус калибровки датчиков (для Web) ===
    int webCalibStatus = 0;      // 0=IDLE, 1=ПОИСК, 2=НАЙДЕН, 3=ОШИБКА
    String webCalibSensorName = ""; // Имя датчика
    bool headTestActive = false;
int headTestRemainingSec = 0;
int headTestTotalSec = 0;

bool bodyTestActive = false;
int bodyTestRemainingSec = 0;
int bodyTestTotalSec = 0;

// Ожидание ввода мл после теста
bool testAwaitingInput = false;   // true = тест завершён, ждём ввода мл от оператора
String testAwaitingType = "";     // "head", "body_nc", "body_no"

    // === НОВЫЕ ПОЛЯ ДЛЯ ИНФОРМАЦИИ О ПРОЦЕССЕ (WEB) ===
    String rectMethodName = "";      // "KSS" или "STANDARD"
    String rectSubStage = "";        // "Spit", "Standard", "AkaTelo"
    int rectTimeRemaining = 0;       // Секунды до конца этапа
    int rectVolumeTarget = 0;        // Объем текущего под-этапа (мл)

    String bodyMethodName = "";      // "Шпора" или "Стандарт"
    float bodySpeed = 0.0f;          // Текущая скорость тела (мл/час)
    float headsSpeed = 0.0f;         // Текущая скорость голов (мл/час) — отдельно от тела
    float headsVolDone = 0.0f;       // Накопленный объём голов (мл)
    int bodyCycle = 0;               // Номер цикла (для Стандарта)
    // ================================================
    // Завершение процесса
    unsigned long finishingRemainSec = 0; // Остаток таймера охлаждения (сек)
    // Время
    unsigned long processTimeSec = 0;  // COUNTALL (общее время процесса)
    unsigned long stageTimeSec = 0;    // Время текущего этапа
    
    // Текущие значения (для расчетов в UI)
    float currentTsa = 0.0f;
    float currentAqua = 0.0f;
    float currentTsar = 0.0f;
    float currentTank = 0.0f;
    float currentStrength = 0.0f;      // Текущая крепость
    float currentStrengthBak = 0.0f;   // Крепость в баке
    bool strengthOutValid = false;    // Крепость в струе определена (температура >= 78.5°C)
    bool strengthBakValid = false;    // Крепость в баке определена (температура >= 78.5°C)

    // Статус теста клапанов (для Web)
    struct {
        bool active = false;
        int type = 0;  // 0=none, 1=head, 2=body
        int remainingSec = 0;
        int totalSec = 0;
    } valveTest;
};

// ==================== КОМАНДЫ ОТ UI ====================
enum class UiCommand {
    // Нет команды
    NONE,
    UP,
    DOWN,
    // Запуск процессов
    START_DIST,
    START_RECT,
    
    // Управление процессами
    STOP_PROCESS,
    PAUSE_PROCESS,
    RESUME_PROCESS,
    
    // Ответы на диалоги
    DIALOG_YES,         // Общий ответ "ДА"
    DIALOG_NO,          // Общий ответ "НЕТ"
    NEXT_STAGE,
    TEST_HEAD,   // <--- Добавить
    TEST_BODY,
    STOP_TEST,
    CALC_VALVE, 
    // Специальные меню (RECT)
    ENTER_VALVE_CAL,
    ENTER_SET_PW_AS,
    SAVE_VALVE_CAL,
    SAVE_SET_PW_AS,
    
    // Ручное управление (только когда процесс не запущен)
    MANUAL_HEATER_ON,
    MANUAL_HEATER_OFF,
    MANUAL_VALVE_WATER_OPEN,
    MANUAL_VALVE_WATER_CLOSE,
    MANUAL_VALVE_HEAD_OPEN,
    MANUAL_VALVE_HEAD_CLOSE,
    MANUAL_VALVE_BODY_OPEN,
    MANUAL_VALVE_BODY_CLOSE,
    MANUAL_MIXER_ON,
    MANUAL_MIXER_OFF,
    
    // Команды для Web калибровки
    IDENTIFY_TSA,
    IDENTIFY_AQUA,
    IDENTIFY_TSAR,
    IDENTIFY_TANK,
    CANCEL_CALIBRATION,
    FINISH_CALIBRATION
};

// ==================== ОТВЕТЫ PROCESSENGINE ====================
enum class EngineResponse {
    // Результат выполнения
    OK,
    ERROR_BUSY,             // Уже запущен другой процесс
    ERROR_NO_PROCESS,       // Нет активного процесса для команды
    ERROR_INVALID_STATE,    // Команда недопустима в текущем состоянии
    ERROR_EMERGENCY,        // Система в аварийном состоянии
    
    // Требуется показать UI-состояние
    SHOW_WATER_TEST,
    SHOW_REPLACEMENT,
    SHOW_GOLOVY_OK,
    SHOW_VALVE_CAL,
    SHOW_SET_PW_AS,
    
    // Навигация
    SHOW_PROCESS_SCREEN,
    SHOW_PROCESS_MENU,      // НОВОЕ: Возврат в меню процесса
    SHOW_MAIN_MENU,
    
    // Аварии
    SHOW_EMERGENCY_SCREEN,
    
    // Ручное управление
    MANUAL_MODE_ENABLED,
    MANUAL_MODE_DISABLED
};

// ==================== КОНФИГУРАЦИОННЫЕ СТРУКТУРЫ ====================

// Общая глобальная конфигурация (меню SETTINGS)
struct GlobalConfig {
    float tsaLimit = 95.0f;          // Макс. температура TSA
    float boxMaxTemp = 45.0f;        // Макс. температура бокса
    int vreacDelay = 300;            // Время аварийной задержки (сек)
    float razgonTemp = 60.0f;        // Температура разгона
    int checkWifiInterval = 5;       // Интервал проверки WiFi (мин)
    bool wifiEnabled = true;         // Включить WiFi
    bool telegramEnabled = true;     // Включить уведомления
    bool soundEnabled = true;        // Включить звук
    int timezoneOffset = 3;          // Часовой пояс (UTC+3)
    bool use24hFormat = true;        // 24-часовой формат времени
    bool autoSyncTime = true;        // Автосинхронизация времени
    float pressureCorrection = 0.0f; // Коррекция давления
};

// Конфигурация процесса DIST (меню DIST_SETUP)
struct DistConfig {
    float bakStopTemp = 98.0f;       // Температура остановки бака
    float midtermTemp = 92.0f;       // Температура смены посуды
    bool useValve = true;            // Использовать клапан отбора
    bool fullPower = false;          // Полная мощность ТЭНа
    bool mixerEnabled = false;       // Включить мешалку
    int mixerOnTime = 60;            // Время работы мешалки (сек)
    int mixerOffTime = 180;          // Время паузы мешалки (сек)
    bool useHeadValve = false;       // Использовать клапан голов
    int headCollectTime = 10;        // Время сбора голов (мин)
    bool autoStop = false;           // Автоостановка при достижении крепости
    float stopStrength = 40.0f;      // Крепость для автоостановки
};

// Конфигурация процесса RECT (меню RECT_SETUP)
struct RectConfig {
    // Основные настройки
    bool useHeadValve = true;        // Использовать отдельный клапан голов
    bool bodyValveNC = true;         // Тип клапана тела: true=НЗ, false=НО
    bool headsTypeKSS = false;       // Метод отбора голов: false=Standard, true=KSS
    bool calibration = true;         // Требовать калибровку клапанов
    int cycleLim = 3;                // Макс. количество циклов стабилизации
    int nasebTime = 30;              // Время насыщения/стабилизации (мин)
    int reklapTime = 10;             // Время рекламации (мин)
    
    // Температурные параметры
    float histeresis = 0.5f;         // Гистерезис для метода Overhist
    float delta = 0.2f;              // Дельта для метода Shpora
    float correlation = 1.5f;        // Коэффициент корреляции для Shpora
    
    // Калибровка клапанов
    int headOpenMs = 1000;           // Время открытия клапана голов (мс)
    int headCloseMs = 5000;          // Время закрытия клапана голов (мс)
    int bodyOpenMs = 2000;           // Время открытия клапана тела (мс)
    int bodyCloseMs = 10000;         // Время закрытия клапана тела (мс)
    float valveHeadCapacity = 0.0f;  // Пропускная способность клапана голов (мл/мин)
    float valveBodyCapacity = 0.0f;  // Пропускная способность НЗ клапана тела
    float valve0BodyCapacity = 0.0f; // Пропускная способность НО клапана тела
    
    // Расчетные параметры (заполняются в меню SET PW & AS)
    float power = 2500.0f;           // Мощность нагрева (Вт)
    float asVolume = 5000.0f;        // Объем абсолютного спирта (мл)
};
// ==================== МЕЖЗАДАЧНАЯ ОЧЕРЕДЬ КОМАНД ====================
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

struct CommandMessage {
    UiCommand command;
};

// Глобальная очередь: AppNetwork (Core 0) → loop() (Core 1)
// Объявляем extern, определяем в .ino
extern QueueHandle_t commandQueue;
#endif // PROCESS_COMMON_H