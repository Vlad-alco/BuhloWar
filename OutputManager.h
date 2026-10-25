#ifndef OUTPUT_MANAGER_H
#define OUTPUT_MANAGER_H

#include <Arduino.h>
#include "ProcessCommon.h"
// Убрали #include "config.h" - он здесь не нужен, это решит ошибку компиляции

class OutputManager {
public:
    // ИЗМЕНЕНО: передаем только bool isBodyValveNC
    void begin(bool isBodyValveNC); 
    void resetEmergency(); 
    void setBodyValveType(bool isNC);
    void powerOffBodyValve(); // Принудительное обесточивание реле клапана тела
    // Управление
    void setHeater(bool enable, bool fullPower = false);
    // Выключение нагрева с правильной последовательностью:
// heaterPin2 (разгонный) → heaterPin1 (рабочий) → contactor (РМ)
void setHeaterOff();

// Выключение только пинов ТЭНа без контактора (для BAKSTOP):
// heaterPin2 → heaterPin1, контактор остаётся включён
void setHeaterPowerOff();

    void openWaterValve();
    void closeWaterValve();
    void openHeadValve();
    void closeHeadValve();
    void openBodyValve();
    void closeBodyValve();
    
    void startMixer();
    void stopMixer();
    void setMixer(bool enable); 
    
    void beep(int count = 1, int durationMs = 100);
    void alarmBeep(AlarmType alarmType);
    void stopAlarm();
    
    // Режимы
    void startDistillationMode();
    void pauseDistillationMode();
    void startRectificationMode();
    void startHeadValveCycling(int openMs, int closeMs);
    void startBodyValveCycling(int openMs, int closeMs);
    void stopValveCycling();
    void stopHeadValveTest();
    void stopBodyValveTest();
    void startMixerCycling(int onTimeSec, int offTimeSec);
    
    void emergencyStop();
    void setSafeShutdownDuration(unsigned long durationMs) { safeShutdownDuration = durationMs; }
    void safeShutdown();
    
    // Состояние
    bool isHeaterOn() const;
    bool isWaterValveOpen() const;
    bool isHeadValveOpen() const;
    bool isBodyValveOpen() const;
    bool isMixerOn() const;
    bool isEmergency() const;
    bool isMixerCycling() const { return mixerCycling; }
    void update();

    // === Этап 2.1: СУХОЙ ХОД СИМУЛЯТОРА ===
    // В сухом ходе OutputChannel::write() обновляет ТОЛЬКО логическое
    // состояние (currentState) — digitalWrite пропускается: реле/клапаны/
    // ТЭН физически молчат, а весь UI (веб, LCD, /api/status) видит
    // «виртуальный» процесс как обычно. Баззер НЕ гейтится — звуковая
    // индикация работает всегда.
    // РАБОЧИЙ ход (по умолчанию) — как прежде, всё переключается физически.
    // При выходе из сухого хода физика отстаёт от логики — ближайший update()
    // пересинхронизирует пины (resyncPhysical).
    static void setSimDryRun(bool on) {
        if (simDryRunFlag == on) return;
        simDryRunFlag = on;
        if (!on) resyncRequested = true;   // физику догоним в ближайшем update()
    }
    static bool isSimDryRun() { return simDryRunFlag; }
    void resyncPhysical();   // записать физически текущие логические состояния

private:
    static bool simDryRunFlag;      // СУХОЙ ход активен (общий для всех каналов)
    static bool resyncRequested;    // запрошена пересинхронизация физики

    struct OutputChannel {
        uint8_t pin;
        bool isNC;           
        bool isActiveHigh;   
        bool currentState;   
        bool physGated;      // Этап 2.1: канал гейтится сухим ходом (баззер — нет)

        OutputChannel() : pin(0), isNC(true), isActiveHigh(true), currentState(false), physGated(false) {}

        void write(bool state) {
            currentState = state;   // Этап 2.1: логическое состояние живёт всегда (UI видит виртуальный процесс)
            if (pin == 0) return;
            // === Этап 2.1: СУХОЙ ход — физический выход не трогаем ===
            if (physGated && simDryRunFlag) return;
            bool physicalState;
            if (isNC) physicalState = state; 
            else physicalState = !state;
            if (!isActiveHigh) physicalState = !physicalState;
            digitalWrite(pin, physicalState ? HIGH : LOW);
        }
        bool read() const { return currentState; }
    };
    
    // Каналы
    OutputChannel heaterPin1;
    OutputChannel heaterPin2;
    OutputChannel contactor;
    OutputChannel valveWater;
    OutputChannel valveHead;
    OutputChannel valveBody;
    OutputChannel mixer;
    OutputChannel buzzer;
    
    bool emergencyState = false;
    bool safeShutdownActive = false;
    unsigned long safeShutdownStart = 0;
    unsigned long safeShutdownDuration = 300000; // Default 5 min

    bool headValveCycling = false;
    bool bodyValveCycling = false;
    bool mixerCycling = false;
    unsigned long headValveCycleStart = 0;
    unsigned long bodyValveCycleStart = 0;
    unsigned long mixerCycleStart = 0;
    int headValveOpenMs = 0;
    int headValveCloseMs = 0;
    int bodyValveOpenMs = 0;
    int bodyValveCloseMs = 0;
    int mixerOnTimeSec = 0;
    int mixerOffTimeSec = 0;

    AlarmType currentAlarm = AlarmType::NONE;

struct BuzzerSM {
    bool     active      = false;
    int      count       = 0;
    int      onTime      = 0;
    int      offTime     = 0;
    int      repeatDelay = 0;
    bool     buzzerOn    = false;
    unsigned long stepStart = 0;
    uint8_t  phase       = 0;  // 0=ON, 1=OFF_between, 2=REPEAT_DELAY
    int      totalCount  = 0;
    int      savedOnTime = 0;
    int      savedOffTime= 0;
    int      savedRepeat = 0;
} bsm;

void updateCycling();
void updateAlarm();
void startBeepPattern(int count, int onTime, int offTime, int repeatDelay);
};

#endif