#ifndef SDLOGGER_H
#define SDLOGGER_H

#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <time.h> 

// === ИЗМЕНЕНО: Увеличен лимит с 50 КБ до 200 КБ ===
// 200 КБ хватит на 2-3 часа работы с детальным логом
const unsigned long MAX_LOG_SIZE = 204800; 
// ==================================================

class SDLogger {
public:
    void init() {
        // Ничего не делаем. SD должна быть инициализирована раньше (в AppNetwork).
    }

    void log(const String &message) {
        // === АВТООПРЕДЕЛЕНИЕ SD ===
        if (!sdAvailable) {
            if (SD.cardSize() > 0) sdAvailable = true;
        }
        
        if (!sdAvailable) return;

        // === ОПТИМИЗАЦИЯ: Проверка размера ===
        // Проверяем, превысил ли *отслеживаемый* размер лимит.
        // Это быстрее, чем открывать файл и вызывать .size() каждый раз.
        if (currentFileSize > MAX_LOG_SIZE) {
            rotateLogs();
            currentFileSize = 0; // Сбрасываем счетчик после ротации
        }
        // =====================================

        // Формируем строку
        String timeStr = getTimeStr();
        String logLine = "[" + timeStr + "] " + message;

        // Пишем на SD карту
        File file = SD.open("/system.log", FILE_APPEND);
        if (file) {
            file.println(logLine);
            // Обновляем счетчик размера (длина строки + 2 байта на \r\n)
            currentFileSize += logLine.length() + 2; 
            file.close();
        }

        // Выводим в Serial
        Serial.println(logLine);
    }

    void log(const String &label, int value) { log(label + String(value)); }
    void log(const String &label, float value) { log(label + String(value, 2)); }

        String readLastLog() {
        if (!sdAvailable) return "SD Error";
        File file = SD.open("/system.log");
        if (!file) return "Log file not found";
        
        String content = "";
        
        // === ИЗМЕНЕНО: Буфер увеличен с 4096 до 16384 байт (16 КБ) ===
        // ESP32-S3 имеет достаточно памяти для этого.
        const int bufferSize = 16384; 
        
        if (file.size() > bufferSize) {
            file.seek(file.size() - bufferSize);
        }
        
        // Резервируем память для строки, чтобы не было фрагментации кучи
        content.reserve(bufferSize); 
        
        while (file.available()) {
            content += (char)file.read();
        }
        file.close();
        return content;
    }

    bool isSdAvailable() { return sdAvailable; }

private:
    bool sdAvailable = false;
    // === НОВАЯ ПЕРЕМЕННАЯ ===
    unsigned long currentFileSize = 0; 
    // ========================

    String getTimeStr() {
        time_t now = time(nullptr);
        if (now > 1609459200) {
            struct tm* timeinfo = localtime(&now);
            char buffer[20];
            strftime(buffer, 20, "%d.%m.%Y %H:%M:%S", timeinfo);
            return String(buffer);
        } else {
            return String(millis() / 1000) + "s";
        }
    }

    void rotateLogs() {
        Serial.println("[Logger] Rotating logs...");
        // Удаляем самый старый
        if (SD.exists("/system5.log")) SD.remove("/system5.log");
        if (SD.exists("/system4.log")) SD.rename("/system4.log", "/system5.log");
        if (SD.exists("/system3.log")) SD.rename("/system3.log", "/system4.log");
        if (SD.exists("/system2.log")) SD.rename("/system2.log", "/system3.log");
        if (SD.exists("/system1.log")) SD.rename("/system1.log", "/system2.log");
        
        // Переименовываем текущий лог
        SD.rename("/system.log", "/system1.log");
        
        // sdAvailable остается true, счетчик currentFileSize сбрасывается в log()
    }
};

extern SDLogger logger;

#endif