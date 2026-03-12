#include "OutputManager.h"
#include "config.h" // Здесь нужен для пинов (HEATER_PIN1 и т.д.)
#include "SDLogger.h" // Подключаем заголовок логера

extern SDLogger logger; // Объявляем, что переменная logger существует глобально где-то в другом месте

// ==================== ИНИЦИАЛИЗАЦИЯ ====================

void OutputManager::begin(bool isBodyValveNC) {
    // 1. Нагрев и Контактор
    heaterPin1.pin = HEATER_PIN1; heaterPin1.isActiveHigh = true; heaterPin1.isNC = true;
    heaterPin2.pin = HEATER_PIN2; heaterPin2.isActiveHigh = true; heaterPin2.isNC = true;
    contactor.pin = CONTACTOR_PIN; contactor.isActiveHigh = true; contactor.isNC = true;
    
    // 2. Вода и Головы
    valveWater.pin = VALVE_WATER_PIN; valveWater.isActiveHigh = true; valveWater.isNC = true;
    valveHead.pin = VALVE_HEAD_PIN; valveHead.isActiveHigh = true; valveHead.isNC = true;
    
    // 3. КЛАПАН ТЕЛА - БЕРЕМ ИЗ АРГУМЕНТА
    valveBody.pin = VALVE_BODY_PIN;
    valveBody.isActiveHigh = true; 
    valveBody.isNC = isBodyValveNC; // Используем переданный параметр
    
    // 4. Мешалка и Баззер
    mixer.pin = MIXER_PIN; mixer.isActiveHigh = true; mixer.isNC = true;
    buzzer.pin = BUZZER_PIN; buzzer.isActiveHigh = true; buzzer.isNC = true;

    pinMode(HEATER_PIN1, OUTPUT);
    pinMode(HEATER_PIN2, OUTPUT);
    pinMode(CONTACTOR_PIN, OUTPUT);
    pinMode(VALVE_WATER_PIN, OUTPUT);
    pinMode(VALVE_HEAD_PIN, OUTPUT);
    pinMode(VALVE_BODY_PIN, OUTPUT);
    pinMode(MIXER_PIN, OUTPUT);
    pinMode(BUZZER_PIN, OUTPUT);

    // Безопасное начальное состояние
    heaterPin1.write(false);
    heaterPin2.write(false);
    contactor.write(false);
    valveWater.write(false);
    valveHead.write(false);
    valveBody.write(false);
    mixer.write(false);
    buzzer.write(false);

    emergencyState = false;
    safeShutdownActive = false;
    Serial.print("[OutputManager] Initialized. Body Valve Type: "); 
    Serial.println(valveBody.isNC ? "NC" : "NO");
}



// ==================== УПРАВЛЕНИЕ АВАРИЕЙ ====================

void OutputManager::resetEmergency() {
    emergencyState = false;
    buzzer.write(false);
    Serial.println("[OutputManager] Emergency state RESET");
    logger.log("[OutputManager] Emergency state RESET");
}

// ==================== ОСНОВНЫЕ КОМАНДЫ ====================

void OutputManager::setHeater(bool enable, bool fullPower) {
    if (emergencyState && enable) return; // Блокировка включения при аварии

    if (enable) {
        // Включаем Контактор
        contactor.write(true);
        
        if (fullPower) {
            heaterPin1.write(true);  // Разгон
            heaterPin2.write(true);  // Рабочий
        } else {
            heaterPin1.write(true); // Рабочий вкл
            heaterPin2.write(false);  // Разгон выкл
        }
    } else {
        // Выключаем все
        contactor.write(false);
        heaterPin1.write(false);
        heaterPin2.write(false);
    }
}

void OutputManager::setHeaterOff() {
    heaterPin2.write(false);  // разгонный
    heaterPin1.write(false);  // рабочий
    contactor.write(false);   // обесточиваем РМ
    logger.log("[OutputMgr] Heater OFF (correct sequence)");
}

void OutputManager::setHeaterPowerOff() {
    heaterPin2.write(false);  // разгонный
    heaterPin1.write(false);  // рабочий (контактор остаётся)
    logger.log("[OutputMgr] Heater power pins OFF (contactor kept)");
}

void OutputManager::openWaterValve() {
    if (emergencyState) return;
    valveWater.write(true);
}

void OutputManager::closeWaterValve() {
    valveWater.write(false);
}

void OutputManager::openHeadValve() {
    if (emergencyState) return;
    if (headValveCycling) stopValveCycling();
    valveHead.write(true);
}

void OutputManager::closeHeadValve() {
    valveHead.write(false);
}

void OutputManager::openBodyValve() {
    if (emergencyState) return;
    if (bodyValveCycling) stopValveCycling();
    valveBody.write(true);
}

void OutputManager::closeBodyValve() {
    valveBody.write(false);
}
void OutputManager::powerOffBodyValve() {
   // Принудительно снимаем питание с реле (LOW)
    digitalWrite(valveBody.pin, LOW);
    Serial.println("[OutputManager] Body Valve Safe Close");
    logger.log("[OutputManager] Body Valve Safe Close");
}
void OutputManager::startMixer() {
    if (emergencyState) return;
    if (mixerCycling) mixerCycling = false;
    mixer.write(true);
}

void OutputManager::stopMixer() {
    mixer.write(false);
    mixerCycling = false;
}

void OutputManager::setMixer(bool enable) {
    if (enable) startMixer(); else stopMixer();
}

// ==================== ЗВУКОВЫЕ СИГНАЛЫ ====================

void OutputManager::beep(int count, int durationMs) {
    for (int i = 0; i < count; i++) {
        buzzer.write(true);
        delay(durationMs);
        buzzer.write(false);
        if (i < count - 1) delay(durationMs);
    }
}

void OutputManager::alarmBeep(AlarmType alarmType) {
    currentAlarm = alarmType;
    bsm.active = false;
    buzzer.write(false);
    if (alarmType == AlarmType::ALARM_TSA)          startBeepPattern(3, 200, 200, 7600);
else if (alarmType == AlarmType::ATTENTION_BOX) startBeepPattern(2, 200, 200, 28600);
}

void OutputManager::stopAlarm() {
    currentAlarm = AlarmType::NONE;
    bsm.active = false;
    buzzer.write(false);
}

// ==================== СПЕЦИАЛЬНЫЕ РЕЖИМЫ ====================

void OutputManager::startDistillationMode() {
    closeHeadValve();
    closeBodyValve();
    setHeater(false);
}

void OutputManager::pauseDistillationMode() {
    closeBodyValve();
    setHeater(false);
}

void OutputManager::startRectificationMode() {
    closeHeadValve();
    closeBodyValve();
    setHeater(false);
}

void OutputManager::startHeadValveCycling(int openMs, int closeMs) {
    if (emergencyState) return;

    // Если цикл уже запущен с такими же параметрами - выходим
    if (headValveCycling && headValveOpenMs == openMs && headValveCloseMs == closeMs) {
        return;
    }
    // =======================

    Serial.print("[OutputMgr] Start Head Cycling: Open="); Serial.print(openMs);
    Serial.print(" Close="); Serial.println(closeMs);
    // === НОВОЕ: ЗАПИСЬ В ЛОГ ===
    logger.log("[OutputMgr] Head Cycling: Open=" + String(openMs) + "ms, Close=" + String(closeMs) + "ms");
    // ===========================
    
    headValveCycling = true;
    headValveOpenMs = openMs;
    headValveCloseMs = closeMs;
    headValveCycleStart = millis();
    valveHead.write(true);
    
}

void OutputManager::startBodyValveCycling(int openMs, int closeMs) {
    if (emergencyState) return;

    // ЗАЩИТА: Если цикл уже запущен с такими же параметрами - выходим, чтобы не сбрасывать таймер
    if (bodyValveCycling && bodyValveOpenMs == openMs && bodyValveCloseMs == closeMs) {
        return;
    }

    Serial.print("[OutputMgr] Start Body Cycling: Open="); Serial.print(openMs);
    Serial.print(" Close="); Serial.println(closeMs);
    // === НОВОЕ: ЗАПИСЬ В ЛОГ ===
    logger.log("[OutputMgr] Body Cycling: Open=" + String(openMs) + "ms, Close=" + String(closeMs) + "ms");
    // ===========================
    bodyValveCycling = true;
    bodyValveOpenMs = openMs;
    bodyValveCloseMs = closeMs;
    bodyValveCycleStart = millis();
    valveBody.write(true); // Сразу открываем
}
// === НОВЫЕ ФУНКЦИИ ОСТАНОВКИ (Вставить перед stopValveCycling) ===

void OutputManager::stopHeadValveTest() {
    if (headValveCycling) {
        headValveCycling = false;
        closeHeadValve(); // Головы всегда НЗ
        Serial.println("[OutputMgr] Head Test Stopped");
        logger.log("[OutputMgr] Head Stopped");
    }
}

void OutputManager::stopBodyValveTest() {
    if (bodyValveCycling) {
        bodyValveCycling = false;
        closeBodyValve(); // Умное закрытие (учитывает НЗ/НО)
        Serial.println("[OutputMgr] Body Test Stopped");
        logger.log("[OutputMgr] Body Stopped");
    }
}

// === ОБНОВЛЕННАЯ ГЛАВНАЯ ФУНКЦИЯ ===
void OutputManager::stopValveCycling() {
    stopHeadValveTest();
    stopBodyValveTest();
}

void OutputManager::startMixerCycling(int onTimeSec, int offTimeSec) {
    if (emergencyState) return;
    mixerCycling = true;
    mixerOnTimeSec = onTimeSec;
    mixerOffTimeSec = offTimeSec;
    mixerCycleStart = millis();
    mixer.write(true);
}

// ==================== АВАРИЙНЫЕ КОМАНДЫ ====================

void OutputManager::emergencyStop() {
    emergencyState = true;
    heaterPin1.write(false);
    heaterPin2.write(false);
    contactor.write(false);
    valveWater.write(false);
    valveHead.write(false);
    valveBody.write(false);
    mixer.write(false);
    stopValveCycling();
    mixerCycling = false;
    buzzer.write(true);
    Serial.println("[OutputManager] EMERGENCY STOP");
    logger.log("[OutputMgr] EMERGENCY STOP");
}

void OutputManager::safeShutdown() {
    if (emergencyState) return;
    safeShutdownActive = true;
    safeShutdownStart = millis();
    setHeaterOff();
    closeHeadValve();
    closeBodyValve();
    stopValveCycling();
    stopMixer();
    openWaterValve();
    Serial.println("[OutputManager] Safe shutdown started");
    logger.log("[OutputMgr] Safe shutdown started");
}

// ==================== СОСТОЯНИЕ ====================

bool OutputManager::isHeaterOn() const { return heaterPin1.read() || heaterPin2.read(); }
bool OutputManager::isWaterValveOpen() const { return valveWater.read(); }
bool OutputManager::isHeadValveOpen() const { return valveHead.read(); }
bool OutputManager::isBodyValveOpen() const { return valveBody.read(); }
bool OutputManager::isMixerOn() const { return mixer.read(); }
bool OutputManager::isEmergency() const { return emergencyState; }

// ==================== ОБНОВЛЕНИЕ ====================

void OutputManager::update() {
    unsigned long currentTime = millis();
    if (safeShutdownActive) {
        if (currentTime - safeShutdownStart >= SAFE_SHUTDOWN_DURATION) {
            safeShutdownActive = false;
            closeWaterValve();
            Serial.println("[OutputManager] Safe shutdown complete");
            logger.log("[OutputMgr] Safe shutdown complete");
        }
    }
    updateCycling();
    updateAlarm();
}

void OutputManager::updateCycling() {
    unsigned long currentTime = millis();
    if (headValveCycling) {
        unsigned long d = headValveOpenMs + headValveCloseMs;
        if (d > 0) { unsigned long p = (currentTime - headValveCycleStart) % d; valveHead.write(p < headValveOpenMs); }
    }
    if (bodyValveCycling) {
        unsigned long d = bodyValveOpenMs + bodyValveCloseMs;
        if (d > 0) { unsigned long p = (currentTime - bodyValveCycleStart) % d; valveBody.write(p < bodyValveOpenMs); }
    }
    if (mixerCycling) {
        unsigned long d = (mixerOnTimeSec + mixerOffTimeSec) * 1000;
        if (d > 0) { unsigned long p = (currentTime - mixerCycleStart) % d; mixer.write(p < (unsigned long)mixerOnTimeSec * 1000); }
    }
}

void OutputManager::startBeepPattern(int count, int onTime, int offTime, int repeatDelay) {
    bsm.active      = true;
    bsm.count       = count;
    bsm.totalCount  = count;
    bsm.onTime      = onTime;
    bsm.offTime     = offTime;
    bsm.repeatDelay = repeatDelay;
    bsm.phase       = 0;
    bsm.stepStart   = millis();
    buzzer.write(true);
    bsm.buzzerOn    = true;
}

void OutputManager::updateAlarm() {
    if (bsm.active) {
        unsigned long now = millis();
        unsigned long elapsed = now - bsm.stepStart;

        switch (bsm.phase) {
            case 0: // ON
                if (elapsed >= (unsigned long)bsm.onTime) {
                    buzzer.write(false);
                    bsm.count--;
                    bsm.phase = (bsm.count > 0) ? 1 : 2;
                    bsm.stepStart = now;
                }
                break;

            case 1: // пауза между бипами
                if (elapsed >= (unsigned long)bsm.offTime) {
                    buzzer.write(true);
                    bsm.phase = 0;
                    bsm.stepStart = now;
                }
                break;

            case 2: // пауза перед повтором (27 сек)
                if (elapsed >= (unsigned long)bsm.repeatDelay) {
                    if (currentAlarm != AlarmType::NONE) {
                        bsm.count     = bsm.totalCount;
                        bsm.phase     = 0;
                        bsm.stepStart = now;
                        buzzer.write(true);
                    } else {
                        bsm.active = false;
                    }
                }
                break;
        }
    }

    if (!bsm.active && currentAlarm != AlarmType::NONE) {
        if (currentAlarm == AlarmType::ALARM_TSA)          startBeepPattern(3, 200, 200, 7600);
else if (currentAlarm == AlarmType::ATTENTION_BOX) startBeepPattern(2, 200, 200, 28600);
    }
}
void OutputManager::setBodyValveType(bool isNC) {
    valveBody.isNC = isNC;
    Serial.print("[OutputManager] Body Valve Type changed to: "); 
    Serial.println(valveBody.isNC ? "NC" : "NO");
}