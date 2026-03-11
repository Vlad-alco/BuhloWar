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

private:
    struct OutputChannel {
        uint8_t pin;
        bool isNC;           
        bool isActiveHigh;   
        bool currentState;   

        OutputChannel() : pin(0), isNC(true), isActiveHigh(true), currentState(false) {}

        void write(bool state) {
            if (pin == 0) return;
            bool physicalState;
            if (isNC) physicalState = state; 
            else physicalState = !state;
            if (!isActiveHigh) physicalState = !physicalState;
            digitalWrite(pin, physicalState ? HIGH : LOW);
            currentState = state;
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
    const unsigned long SAFE_SHUTDOWN_DURATION = 300000; 

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