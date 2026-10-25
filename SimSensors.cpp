#include "SimSensors.h"

// ============================================================================
// ЭТАП 2: реализация симулятора датчиков. Подробный дизайн — в SimSensors.h.
// Все обращения к состоянию — из ядра 1 (опрос) и ядра 0 (веб-команды) —
// обёрнуты в короткие portMUX-критсекции только над копированием полей.
// Вся «тяжёлая» математика (кривые, sin/shimmer) выполняется ВНЕ секций.
// ============================================================================

SimSensors* SimSensors::instance = nullptr;

const float SimSensors::DS18B20_STEP   = 0.0625f;
const float SimSensors::DISCONNECTED_C = -127.0f;

SimSensors* SimSensors::getInstance() {
    if (!instance) instance = new SimSensors();
    return instance;
}

SimSensors::SimSensors() {
    mux = portMUX_INITIALIZER_UNLOCKED;
    for (int i = 0; i < CH_COUNT; i++) state[i] = 20.0f;
}

// ---------------------------------------------------------------------------
// Сим-время: t_sim = simSecAtChange + (now - changeMs) * speed / 1000
// Смена скорости/сценария пересчитывает базис, чтобы часы не прыгали.
// ---------------------------------------------------------------------------
float SimSensors::simTimeSec(unsigned long nowMs) const {
    if (!active || changeMs == 0) return 0.0f;
    float dt = (float)(nowMs - changeMs) / 1000.0f;
    if (dt < 0.0f) dt = 0.0f;
    return simSecAtChange + dt * speed;
}

float SimSensors::quantize(float v) {
    // DS18B20 отдаёт значение с шагом 0.0625C — симулятор квантуется так же,
    // иначе stale-детектор (бит-в-бит сравнение сырья) теряет смысл.
    return roundf(v / DS18B20_STEP) * DS18B20_STEP;
}

// ---------------------------------------------------------------------------
// Целевые кривые сценариев (чистая функция сим-времени).
// ВРЕМЕННЫЕ ЯКОРЯ (сек сим-времени):
//   t=3240 — пар дошёл до царги (куб ~72C) — начало прогрева верха
//   t=3600 — куб 78C — РАЗГОН завершён (дефолтная уставка razgonTemp)
//   RECT: t=4140 — царга вышла 79.5, t=5040 — «на себя» 78.25, дальше ТЕЛО
// ---------------------------------------------------------------------------
static float clamp01(float x) { return (x < 0.0f) ? 0.0f : (x > 1.0f) ? 1.0f : x; }
static float smooth(float x)  { x = clamp01(x); return x * x * (3.0f - 2.0f * x); }

// Мелкое дрожание «живого» датчика: два медленных синуса + детерминированный
// псевдошум. Амплитуда ~0.05C — после квантования младший бит (0.0625)
// дёргается каждые несколько секунд, поэтому в НОРМЕ stale-детектор молчит.
static float wobble(uint8_t ch, float t, float scale) {
    float a = 0.03f * sinf(t / 7.3f  + ch * 1.7f);
    float b = 0.02f * sinf(t / 13.1f + ch * 2.9f);
    float seed = sinf(t * (0.717f + ch * 0.113f) + ch * 3.7f) * 43758.5453f;
    float r = (seed - floorf(seed)) * 0.04f - 0.02f;
    return (a + b + r) * scale;
}

float SimSensors::targetCurve(uint8_t ch, float tSec) const {
    const float t = tSec;
    // Ключевые точки кривой куба
    const float tVapor = 3240.0f;   // пар дошёл до царги
    const float tRazgon = 3600.0f;  // куб 78C — конец РАЗГОНА

    switch (ch) {
    case 3: { // TANK — куб
        if (scenario == SCEN_DIST) {
            if (t < tRazgon) return 18.0f + 60.0f * (t / tRazgon);          // 18->78 за 60 мин
            float body = clamp01((t - tRazgon) / 5400.0f);                   // ТЕЛО 90 мин
            return 78.0f + 21.0f * body;                                     // 78->99 (СМЕНА ТАРЫ ~92, БАКСТОП 99)
        } else {
            if (t < tRazgon) return 18.0f + 60.0f * (t / tRazgon);           // 18->78 за 60 мин
            float body = clamp01((t - tRazgon) / 7200.0f);                   // спирт-сырец греется медленнее
            return 78.0f + 8.0f * body;                                      // 78->86
        }
    }
    case 2: { // TSAR — царга
        if (scenario == SCEN_DIST) {
            if (t < tVapor) return 20.0f;                                    // холодная царга
            // Пар пришёл: за 240 с догоняет куб до TANK-4 и следует за ним
            float s = smooth((t - tVapor) / 240.0f);
            float tank = 18.0f + 60.0f * (min(t, tRazgon) / tRazgon);
            if (t >= tRazgon) tank = 78.0f + 21.0f * clamp01((t - tRazgon) / 5400.0f);
            return 20.0f + ((tank - 4.0f) - 20.0f) * s;                      // WAITING: +5C от входа — быстро
        } else {
            if (t < tVapor) return 20.0f;                                    // холодная колонна
            // Прогрев колонны (WAITING требует +5C от точки входа — успевает)
            float warm = 20.0f + 59.5f * smooth((t - tVapor) / 900.0f);      // -> 79.5 к t=4140
            if (t < 4140.0f) return warm;
            // Выход «на себя»: 79.5 -> 78.25 к t=5040 (NASEBYA)
            float settle = 79.5f - 1.25f * smooth((t - 4140.0f) / 900.0f);
            if (t < 5040.0f) return settle;
            return 78.25f; // ТЕЛО: база; дрейф добавляется в rawFor (tsarDrift)
        }
    }
    case 0: { // TSA — верх колонны / паровая зона (лимит 40-55C — не задеваем)
        if (t < tVapor) return 20.0f;
        float top = (scenario == SCEN_DIST) ? 16.0f : 16.0f;                 // 20 -> 36
        float rise = 20.0f + top * smooth((t - tVapor) / 300.0f);
        return rise + 2.0f * sinf(t / 600.0f);                               // «дыхание» 32-38
    }
    case 1: { // AQUA — вода охлаждения
        float warm = clamp01(t / 5400.0f);
        return 14.0f + 14.0f * warm + 1.2f * sinf(t / 47.0f);                // 14 -> 28
    }
    default: return 20.0f;
    }
}

// ---------------------------------------------------------------------------
// Старт/стоп
// ---------------------------------------------------------------------------
void SimSensors::start(uint8_t scen, const float curTemps[CH_COUNT]) {
    portENTER_CRITICAL(&mux);
    scenario = (scen == SCEN_RECT) ? SCEN_RECT : SCEN_DIST;
    active = true;
    changeMs = millis();
    simSecAtChange = 0.0f;
    tsarDrift = 0.0f;
    // Стартуем с ТЕКУЩИХ отфильтрованных температур (если датчики что-то видели):
    // иначе после реального куба 78C первая же синтетика 18C навечно застряла
    // бы в гейте правдоподобия (скачок 60C, медиана не спасает).
    for (int i = 0; i < CH_COUNT; i++) {
        float v = curTemps[i];
        state[i] = (v > -120.0f && v < 120.0f) ? v : 20.0f;
        anom[i].type = ANOM_NONE;
        anom[i].untilMs = 0;
    }
    stateInit = true;
    lastStateMs = changeMs;
    portEXIT_CRITICAL(&mux);
    Serial.println("[SIM] START: сценарий " + String(scenario == SCEN_DIST ? "DIST" : "RECT"));
}

void SimSensors::stop() {
    portENTER_CRITICAL(&mux);
    active = false;
    stateInit = false;
    for (int i = 0; i < CH_COUNT; i++) { anom[i].type = ANOM_NONE; anom[i].untilMs = 0; }
    portEXIT_CRITICAL(&mux);
    Serial.println("[SIM] STOP: возврат к реальным датчикам");
}

bool SimSensors::setScenario(uint8_t scen) {
    uint8_t s = (scen == SCEN_RECT) ? SCEN_RECT : SCEN_DIST;
    portENTER_CRITICAL(&mux);
    if (!active) { portEXIT_CRITICAL(&mux); return false; }
    bool changed = (s != scenario);
    if (changed) {
        // Перебазируем сим-время и сбрасываем дрейф: новый прогон с текущих
        // температур — инерция state плавно уведёт каналы к новой кривой,
        // гейт правдоподобия не застрянет (скачков цели нет).
        simSecAtChange = simTimeSecUnsafe(millis());
        changeMs = millis();
        scenario = s;
        tsarDrift = 0.0f;
    }
    portEXIT_CRITICAL(&mux);
    return changed;
}

bool SimSensors::setSpeed(float spd) {
    if (spd < 1.0f) spd = 1.0f;
    if (spd > 30.0f) spd = 30.0f;
    portENTER_CRITICAL(&mux);
    if (!active) { portEXIT_CRITICAL(&mux); return false; }
    simSecAtChange = simTimeSecUnsafe(millis());
    changeMs = millis();
    speed = spd;
    portEXIT_CRITICAL(&mux);
    return true;
}

bool SimSensors::isActive() const {
    portENTER_CRITICAL(&mux);
    bool a = active;
    portEXIT_CRITICAL(&mux);
    return a;
}

uint8_t SimSensors::getScenario() const {
    portENTER_CRITICAL(&mux);
    uint8_t s = scenario;
    portEXIT_CRITICAL(&mux);
    return s;
}

float SimSensors::getSpeed() const {
    portENTER_CRITICAL(&mux);
    float s = speed;
    portEXIT_CRITICAL(&mux);
    return s;
}

unsigned long SimSensors::getSimTimeSec() const {
    portENTER_CRITICAL(&mux);
    unsigned long t = (unsigned long)simTimeSec(millis());
    portEXIT_CRITICAL(&mux);
    return t;
}

// ВНИМАНИЕ: вызывать ТОЛЬКО под мьютексом (используется в setScenario/setSpeed)
float SimSensors::simTimeSecUnsafe(unsigned long nowMs) const {
    if (changeMs == 0) return 0.0f;
    float dt = (float)(nowMs - changeMs) / 1000.0f;
    if (dt < 0.0f) dt = 0.0f;
    return simSecAtChange + dt * speed;
}

// ---------------------------------------------------------------------------
// Сырой отсчёт канала — точка подмены реального DS18B20.
// Пайплайн: целевая кривая (+дрейф ТЕЛО) -> инерция state (лимит скорости,
// чтобы скачки цели не попадали в гейт навечно) -> шум -> аномалии ->
// квантование 0.0625.
// ---------------------------------------------------------------------------
float SimSensors::rawFor(uint8_t ch, unsigned long nowMs) {
    if (ch >= CH_COUNT) return DISCONNECTED_C;

    portENTER_CRITICAL(&mux);
    if (!active) { portEXIT_CRITICAL(&mux); return DISCONNECTED_C; }
    float spd = speed;
    uint8_t scen = scenario;
    float drift = tsarDrift;
    unsigned long last = lastStateMs;
    portEXIT_CRITICAL(&mux);

    float realDelta = (float)(nowMs - last) / 1000.0f;
    if (realDelta < 0.0f) realDelta = 0.0f;
    if (realDelta > 5.0f) realDelta = 5.0f;   // защита от пауз/дебага

    float t = simTimeSec(nowMs);

    // --- Целевая кривая (чистая математика вне критсекции) ---
    float target = targetCurve(ch, t);
    // Дрейф царги в ТЕЛО (RECT): копится в РЕАЛЬНОМ времени с множителем speed
    if (scen == SCEN_RECT && ch == 2 && t > 5040.0f) {
        target += drift;
    }

    // --- Инерция state: движение к цели с ограничением скорости ---
    // Лимит: 0.6C/с * speed, потолок 1.5C/с. За цикл опроса ~1.6 с максимум
    // 2.4C — гейт правдоподобия (3.0C) не срабатывает на нормальной кривой.
    float maxRate = 0.6f * spd;
    if (maxRate > 1.5f) maxRate = 1.5f;
    float maxStep = maxRate * realDelta;

    portENTER_CRITICAL(&mux);
    float diff = target - state[ch];
    if (diff > maxStep) diff = maxStep;
    if (diff < -maxStep) diff = -maxStep;
    state[ch] += diff;
    lastStateMs = nowMs;
    // Накопление дрейфа ТЕЛО (RECT, TSAR): +0.012C за сим-минуту, ускоряется
    // speed. Залёт (histeresis ~0.5-1C) наступает сам, как на реальном процессе.
    if (scen == SCEN_RECT && ch == 2 && t > 5040.0f) {
        tsarDrift += (0.012f / 60.0f) * spd * realDelta;
    }
    float val = state[ch];
    portEXIT_CRITICAL(&mux);

    // --- Шум живого датчика (амплитуда падает в ТЕЛО RECT, чтобы дельта
    // шпоры 0.06C оставалась осмысленной и Залёт не был вызван шумом) ---
    float noiseScale = 1.0f;
    if (scen == SCEN_RECT && ch == 2 && t > 5040.0f) noiseScale = 0.3f;
    val += wobble(ch, t, noiseScale);

    // --- Аномалии (в реальном времени) ---
    portENTER_CRITICAL(&mux);
    Anomaly& a = anom[ch];
    if (a.type != ANOM_NONE && (long)(nowMs - a.untilMs) >= 0) {
        a.type = ANOM_NONE;   // срок вышел — очищаем (безопасно из опроса)
    }
    if (a.type == ANOM_SPIKE) {
        val = val + a.amplitude;          // выброс поверх значения
    } else if (a.type == ANOM_FREEZE) {
        val = a.frozenValue;              // бит-в-бит замороженный отсчёт
    } else if (a.type == ANOM_DROP) {
        portEXIT_CRITICAL(&mux);
        return DISCONNECTED_C;            // «обрыв»: SensorManager уйдёт в resetFilter
    }
    portEXIT_CRITICAL(&mux);

    return quantize(val);
}

// ---------------------------------------------------------------------------
// Аномалии (вызываются из веб-ядра 0)
// ---------------------------------------------------------------------------
bool SimSensors::injectSpike(uint8_t ch, float amplitudeC, int cycles) {
    if (ch >= CH_COUNT || cycles < 1) return false;
    if (amplitudeC < 5.0f) amplitudeC = 5.0f;
    if (amplitudeC > 20.0f) amplitudeC = 20.0f;
    portENTER_CRITICAL(&mux);
    if (!active) { portEXIT_CRITICAL(&mux); return false; }
    anom[ch].type = ANOM_SPIKE;
    // 1 цикл опроса ~2 с (800 мс конвертация + интервал 1 с + запас):
    // x8 => >= 5 отбросов подряд => событие в SD-лог и счётчик гейта
    anom[ch].untilMs = millis() + (unsigned long)cycles * 2000UL;
    anom[ch].amplitude = amplitudeC;
    portEXIT_CRITICAL(&mux);
    Serial.println("[SIM] SPIKE ch=" + String(ch) + " +" + String(amplitudeC, 1) + "C x" + String(cycles));
    return true;
}

bool SimSensors::injectFreeze(uint8_t ch, float seconds) {
    if (ch >= CH_COUNT || seconds <= 0.0f) return false;
    portENTER_CRITICAL(&mux);
    if (!active) { portEXIT_CRITICAL(&mux); return false; }
    anom[ch].type = ANOM_FREEZE;
    anom[ch].untilMs = millis() + (unsigned long)(seconds * 1000.0f);
    // Замораживаем УЖЕ квантованное значение — бит-в-бит одинаковое сырье,
    // ровно как возвращает подвисшая библиотека 1-Wire (старый буфер)
    anom[ch].frozenValue = quantize(state[ch]);
    portEXIT_CRITICAL(&mux);
    Serial.println("[SIM] FREEZE ch=" + String(ch) + " на " + String(seconds, 0) + "с (stale-детектор сработает после 60с)");
    return true;
}

bool SimSensors::injectDrop(uint8_t ch, float seconds) {
    if (ch >= CH_COUNT || seconds <= 0.0f) return false;
    portENTER_CRITICAL(&mux);
    if (!active) { portEXIT_CRITICAL(&mux); return false; }
    anom[ch].type = ANOM_DROP;
    anom[ch].untilMs = millis() + (unsigned long)(seconds * 1000.0f);
    portEXIT_CRITICAL(&mux);
    Serial.println("[SIM] DROP ch=" + String(ch) + " на " + String(seconds, 0) + "с (проверка resetFilter)");
    return true;
}

void SimSensors::clearAnomalies() {
    portENTER_CRITICAL(&mux);
    for (int i = 0; i < CH_COUNT; i++) { anom[i].type = ANOM_NONE; anom[i].untilMs = 0; }
    portEXIT_CRITICAL(&mux);
}

bool SimSensors::snapshot(float outState[CH_COUNT], uint8_t& scen, float& spd,
                          unsigned long& tSec, uint8_t& anomMask) const {
    portENTER_CRITICAL(&mux);
    if (!active) { portEXIT_CRITICAL(&mux); return false; }
    for (int i = 0; i < CH_COUNT; i++) {
        outState[i] = state[i];
        if (anom[i].type != ANOM_NONE) anomMask |= (1 << i);
    }
    scen = scenario;
    spd = speed;
    tSec = (unsigned long)simTimeSec(millis());
    portEXIT_CRITICAL(&mux);
    return true;
}
