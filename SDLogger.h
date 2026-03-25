#ifndef SDLOGGER_H
#define SDLOGGER_H

#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <time.h> 
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// === ИЗМЕНЕНО: Уменьшен лимит до 100 КБ ===
// 100 КБ хватит на ~1 час детального лога
const unsigned long MAX_LOG_SIZE = 102400; 
// ==================================================

// === ГЛОБАЛЬНЫЙ МЬЮТЕКС ДЛЯ SD КАРТЫ ===
// Объявлен в .ino файле, создаётся в setup()
// Защищает SPI шину от одновременного доступа с разных ядер ESP32
extern SemaphoreHandle_t sdMutex;
// =======================================

// === КЛАСС ДЛЯ АВТОМАТИЧЕСКОГО ЗАХВАТА МЬЮТЕКСА ===
// При создании - захватывает мьютекс
// При уничтожении (выход из области видимости) - освобождает
class SDScopeLock {
public:
    SDScopeLock() {
        if (sdMutex) {
            // Конечный таймаут 5 секунд вместо бесконечного ожидания
            xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000));
        }
    }
    ~SDScopeLock() {
        if (sdMutex) {
            xSemaphoreGive(sdMutex);
        }
    }
    // Запрещаем копирование
    SDScopeLock(const SDScopeLock&) = delete;
    SDScopeLock& operator=(const SDScopeLock&) = delete;
};
// ==================================================

class SDLogger {
public:
    void init() {
        // Ничего не делаем. SD должна быть инициализирована раньше (в AppNetwork).
    }

    void log(const String &message) {
        // === БЕЗОПАСНАЯ ПРОВЕРКА SD ===
        // Если SD ещё не проверяли - проверяем один раз
        if (!sdChecked) {
            // Первичная проверка - без мьютекса, т.к. вызывается только из Core 1
            sdAvailable = (SD.cardSize() > 0);
            sdChecked = true;
        }
        
        if (!sdAvailable) {
            // SD недоступна - выводим только в Serial
            Serial.println("[NO SD] " + message);
            return;
        }

        // === ОПТИМИЗАЦИЯ: Проверка размера ===
        // Проверяем, превысил ли *отслеживаемый* размер лимит.
        // Это быстрее, чем открывать файл и вызывать .size() каждый раз.
        if (currentFileSize > MAX_LOG_SIZE) {
            rotateLogs();
            currentFileSize = 0; // Сбрасываем счетчик после ротации
        }
        // =====================================

        // Формируем строку (это быстро, мьютекс не нужен)
        String timeStr = getTimeStr();
        String logLine = "[" + timeStr + "] " + message;

        // === КРИТИЧЕСКАЯ СЕКЦИЯ: работа с SD ===
        {
            SDScopeLock lock;  // Захват мьютекса (автоматически)
            
            // Пишем на SD карту с retry
            const int maxRetries = 3;
            File file;
            for (int retry = 0; retry < maxRetries; retry++) {
                file = SD.open("/system.log", FILE_APPEND);
                if (file) break;
                delay(10);  // Пауза между попытками
            }
            
            if (file) {
                file.println(logLine);
                // Обновляем счетчик размера (длина строки + 2 байта на \r\n)
                currentFileSize += logLine.length() + 2; 
                file.close();
            }
            // При выходе из блока lock уничтожается -> мьютекс освобождается
        }
        // ======================================

        // Выводим в Serial (не требует мьютекса)
        Serial.println(logLine);
    }

    void log(const String &label, int value) { log(label + String(value)); }
    void log(const String &label, float value) { log(label + String(value, 2)); }

    String readLastLog() {
        if (!sdAvailable) return "SD Error";
        
        // === КРИТИЧЕСКАЯ СЕКЦИЯ: чтение с SD ===
        SDScopeLock lock;  // Захват мьютекса
        
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
        // При выходе lock уничтожается -> мьютекс освобождается
        
        return content;
    }
    
    // Читает только НОВЫЕ записи с момента последнего вызова (для облака)
    String readNewLog(unsigned long &lastSize) {
        if (!sdAvailable) return "";
        
        SDScopeLock lock;
        
        File file = SD.open("/system.log");
        if (!file) return "";
        
        unsigned long fileSize = file.size();
        
        // Если файл уменьшился (ротация) - сбрасываем
        if (fileSize < lastSize) {
            lastSize = 0;
        }
        
        // Нет новых данных
        if (fileSize <= lastSize) {
            file.close();
            return "";
        }
        
        // Читаем только новую часть
        file.seek(lastSize);
        
        const int maxNewBytes = 2048;  // Макс 2KB за раз
        String newContent = "";
        newContent.reserve(maxNewBytes);
        
        int bytesRead = 0;
        while (file.available() && bytesRead < maxNewBytes) {
            newContent += (char)file.read();
            bytesRead++;
        }
        
        file.close();
        
        // Обновляем позицию
        lastSize = file.tell();
        
        return newContent;
    }
    
    bool isSdAvailable() { return sdAvailable; }

private:
    bool sdAvailable = false;
    bool sdChecked = false;  // Флаг: уже проверяли SD
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
        // === КРИТИЧЕСКАЯ СЕКЦИЯ: ротация логов ===
        // Вызывается из log(), который уже держит мьютекс
        // Поэтому здесь мьютекс НЕ захватываем повторно!
        
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
