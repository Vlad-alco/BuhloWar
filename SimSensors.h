#ifndef SIM_SENSORS_H
#define SIM_SENSORS_H

#include <Arduino.h>

// ============================================================================
// ЭТАП 2 (план «точные датчики»): СИМУЛЯТОР ДАТЧИКОВ
// ============================================================================
// Зачем: до полугода нет доступа к реальным процессам — все изменения
// (гарды Этапа 1, будущий маршрутизатор MAX31865 и предзапусковый экран)
// проверяются виртуально. SimSensors подменяет СЫРЫЕ отсчёты DS18B20
// синтетикой на входе SensorManager::updateSensorData() — дальше по
// пайплайну (медиана-5 -> гейт -> среднее -> движок/LCD/веб) всё идёт
// ровно как на реальном железе, включая SD-лог событий «SIGNAL: …».
//
// Сценарии (кривые параметризованы SIM-временем, ускоряются множителем):
//   DIST — дистилляция: куб 18->78C (РАЗГОН), пар на царгу, ТЕЛО 78->99C
//          (СМЕНА ТАРЫ/БАКСТОП по фактическим уставкам), TSA верх 30-38C
//          (ниже типового лимита 40-55), вода 14->28C.
//   RECT — ректификация: куб 18->78->86C, царга: прогрев +5C (WAITING),
//          выход «на себя» 79.5->78.25 (NASEBYA), ТЕЛО 78.25 + дрейф
//          +0.012C/мин — Залёт происходит сам, как в жизни.
//
// Аномалии (в РЕАЛЬНОМ времени, для проверки гардов Этапа 1):
//   SPIKE  — выброс +5..20C на 1 или 8 циклов опроса: одиночный целиком
//            убирается медианой-5, серия из 8 доходит до гейта и даёт
//            событие в SD-лог (5 подряд -> лог, счётчик отбросов растёт).
//   FREEZE — подвисание: сырое значение замирает бит-в-бит на 120 с —
//            stale-детектор даёт эпизод после 60 с и лог завершения.
//   DROP   — обрыв: датчик «отваливается» (-127) на 10 с — путь
//            isConnected=false + resetFilter (>5 с без данных).
//
// РЕЖИМ ХОДА (Этап 2.1, решение оператора):
//   СУХОЙ  — реле/клапаны/ТЭН физически НЕ переключаются (OutputManager
//            пропускает digitalWrite), логические состояния и весь UI живут:
//            полностью виртуальный процесс, ноль риска.
//   РАБОЧИЙ— выходы переключаются физически — проверка наводок от коммутации
//            на реальном стенде. ТОЛЬКО на стенде без подключения к аппарату:
//            движок видит синтетические температуры и не знает состояния куба.
//            На рабочем ходе действует АВТО-СТОП: 60 минут РЕАЛЬНОГО времени
//            (не сим-времени!) — затем штатный переход в ФИНИШ. Отключается
//            тумблером в панели.
// ПОЛНЫЙ УСКОРЕНИЕ (Этап 2.1): ProcessEngine берёт время из simNowMs()
// (сим-время растёт speed× быстрее реального) — ускоряются не только
// кривые датчиков, но и таймерные этапы (НАСЕБЯ, ГОЛОВЫ, задержки) и
// объёмные интеграторы. Диалоги оператора (ТЕСТ ВОДЫ, ГОЛОВЫ ОК, СМЕНА
// ТАРЫ) по-прежнему ждут человека — кнопка ДАЛЕЕ.
//
// Потокобезопасность: состояние крутится между ядром 1 (опрос датчиков,
// rawFor) и ядром 0 (веб-команды, snapshot) — короткие portMUX-критсекции
// только над чистой математикой, никаких аллокаций/Serial/SD внутри.
// SIM-состояние НЕ пишется в EEPROM: после перезагрузки симулятор всегда ВЫКЛ.
// ============================================================================
class SimSensors {
public:
    enum Scenario : uint8_t { SCEN_DIST = 0, SCEN_RECT = 1 };
    enum AnomalyType : uint8_t { ANOM_NONE = 0, ANOM_SPIKE = 1, ANOM_FREEZE = 2, ANOM_DROP = 3 };

    // Порядок каналов = SensorIndex (TSA, AQUA, TSAR, TANK)
    static const uint8_t CH_COUNT = 4;
    static const float   DS18B20_STEP;      // 0.0625C — квантование реального DS18B20
    static const float   DISCONNECTED_C;    // -127.0C — маркер обрыва DallasTemperature

    static SimSensors* getInstance();

    bool isActive() const;

    // Старт/стоп. start() гвардируется в AppNetwork (нет процесса/калибровки).
    // curTemps — последние отфильтрованные температуры (SensorManager::getTemperature),
    // чтобы стартовать с реальных значений, а не с холода: иначе после горячего
    // куба первая синтетика 18C навечно застряла бы в гейте правдоподобия.
    void start(uint8_t scenario, const float curTemps[CH_COUNT]);
    void stop();
    bool setScenario(uint8_t scenario);
    bool setSpeed(float speed);

    uint8_t getScenario() const;
    float   getSpeed() const;
    unsigned long getSimTimeSec() const;    // сим-время с начала запуска (для часов в вебе)

    // Режим хода и авто-стоп (Этап 2.1). Устанавливаются из веб-команд
    // SIM_DRY / SIM_AUTOSTOP; гварды (SIM активен, вне процесса) — в AppNetwork.
    bool isDryRun() const;                  // true = СУХОЙ ход (выходы не переключаются)
    void setDryRun(bool d);                 // при выключении сухого хода AppNetwork
                                            // просит OutputManager пересинхронизировать физику
    bool isAutoStop() const;                // авто-стоп рабочего хода (по умолчанию ВКЛ)
    void setAutoStop(bool a);

    // Ускоренные часы процесса (Этап 2.1): возвращают СИМ-время в мс,
    // пока SIM активен, иначе millis(). Источник времени для ProcessEngine:
    // все таймеры этапов и объёмные интеграторы идут от него — поэтому
    // полный разгон охватывает и таймерные/объёмные этапы, а не только кривые.
    unsigned long simNowMs() const;

    // Сырой отсчёт канала для SensorManager::updateSensorData().
    // Возвращает квантованное значение с шумом; при активных аномалиях —
    // выброс/заморозку/обрыв. nowMs — millis() вызывающего (ядро 1).
    float rawFor(uint8_t ch, unsigned long nowMs);

    // Аномалии. cycles для SPIKE — сколько циклов опроса держать выброс
    // (1 цикл ~2 с; 8 циклов гарантируют серию >= 5 -> событие в SD-лог).
    bool injectSpike(uint8_t ch, float amplitudeC, int cycles);
    bool injectFreeze(uint8_t ch, float seconds);
    bool injectDrop(uint8_t ch, float seconds);
    void clearAnomalies();

    // Снимок для /api/status «sim»: state каналов (без аномалий), сценарий,
    // скорость, сим-время, маска активных аномалий (бит ch),
    // режим хода (dry) и флаг авто-стопа (as).
    bool snapshot(float outState[CH_COUNT], uint8_t& scen, float& speed,
                  unsigned long& tSec, uint8_t& anomMask,
                  bool& dry, bool& as) const;

private:
    SimSensors();

    static SimSensors* instance;

    // Спин-блокировка для доступа ядра 0 (веб-команды) и ядра 1 (опрос).
    // mutable — snapshot() константный, но берёт блокировку.
    mutable portMUX_TYPE mux;

    // --- Конфигурация (за мьютексом) ---
    bool  active = false;
    uint8_t scenario = SCEN_DIST;
    float speed = 1.0f;

    // --- Базис сим-времени: t_sim = simSecAtChange + (now - changeMs)*speed ---
    unsigned long changeMs = 0;
    float simSecAtChange = 0.0f;
    unsigned long simMsAtChange = 0;   // тот же базис в СИМ-миллисекундах (для simNowMs)

    // --- Режим хода / авто-стоп (Этап 2.1) ---
    bool dryRun = false;     // СУХОЙ ход: физика выходов отключена (дефолт — РАБОЧИЙ, как до 2.1)
    bool autoStop = true;    // авто-стоп рабочего хода через 60 мин реального времени

    // --- «Физическое» состояние каналов (с инерцией к целевой кривой) ---
    float state[CH_COUNT];
    float lastStateMs = 0;      // millis() последнего расчёта инерции
    bool  stateInit = false;

    // --- Дрейф царги в ТЕЛО (RECT): копится в РЕАЛЬНОМ времени с множителем
    // speed, иначе long-этапы реального времени (NASEBYA/ГОЛОВЫ) рассинхро-
    // нили бы кривую. 0.012C/сим-мин * speed.
    float tsarDrift = 0.0f;

    // --- Аномалии по каналам ---
    struct Anomaly {
        uint8_t type = ANOM_NONE;
        unsigned long untilMs = 0;
        float amplitude = 0.0f;    // SPIKE: +C поверх значения
        float frozenValue = 0.0f;  // FREEZE: зафиксированный отсчёт
    } anom[CH_COUNT];

    // Целевая кривая сценария (чистая функция сим-времени, без состояния).
    float targetCurve(uint8_t ch, float tSec) const;
    float simTimeSec(unsigned long nowMs) const;
    // То же, но БЕЗ взятия блокировки — вызывать только под мьютексом.
    float simTimeSecUnsafe(unsigned long nowMs) const;
    static float quantize(float v);
};

#endif // SIM_SENSORS_H
