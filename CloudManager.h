#ifndef CLOUD_MANAGER_H
#define CLOUD_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

// ============================================================================
// CloudManager — облачное подключение с FreeRTOS задачей на core 0
// ============================================================================
// update() НЕ делает HTTP-запросов. Только:
//   1. Копирует телеметрию в общий буфер через mutex (~2 мкс)
//   2. Читает очереди команд/настроек и вызывает callbacks
//
// Все HTTP-запросы выполняются в отдельной задаче на core 0 (там WiFi стек).
// При падении сервера — блокируется ТОЛЬКО cloud задача, основной loop
// работает без малейших задержек. Экспоненциальный backoff при ошибках.
// ============================================================================

class CloudManager {
private:
    String serverUrl;
    String apiKey;
    unsigned long telemetryIntervalMs = 2000;       // Отправка телеметрии каждые 2 сек
    unsigned long commandCheckIntervalMs = 2000;     // Проверка команд каждые 2 сек
    unsigned long settingsCheckIntervalMs = 60000;   // Проверка настроек каждые 60 сек
    volatile unsigned long settingsLastUpdate = 0;  // Timestamp последнего обновления (volatile: запись из cloud задачи)

    // === ЭКСПОНЕНЦИАЛЬНЫЙ BACKOFF при ошибках (только в cloud задаче) ===
    int errorCount = 0;
    static const int MAX_BACKOFF_MULTIPLIER = 5;     // Макс. множитель: 2^5 = 32

    // === ПЕРЕИСПОЛЬЗУЕМЫЙ HTTPS клиент (только из cloud задачи, без mutex) ===
    WiFiClientSecure _client;
    bool _clientInitialized = false;

    // === FREERTOS: задача, мьютекс, очереди ===
    static const int QUEUE_DATA_SIZE = 1024;         // Макс. размер элемента очереди (байт)
    struct QueueItem {
        char data[QUEUE_DATA_SIZE];
    };

    TaskHandle_t _taskHandle = nullptr;              // Handle cloud задачи
    SemaphoreHandle_t _telemetryMutex = nullptr;     // Мьютекс для _telemetryBuffer
    String _telemetryBuffer;                         // Общий буфер телеметрии (main → cloud)
    QueueHandle_t _commandQueue = nullptr;           // Команды: cloud → main (5 элементов)
    QueueHandle_t _settingsQueue = nullptr;          // Настройки: cloud → main (2 элемента)
    volatile bool _running = false;                  // Флаг работы cloud задачи

    // === Callbacks (вызываются ТОЛЬКО из основного loop через очереди) ===
    typedef void (*CommandCallback)(const String& command, const String& params);
    CommandCallback onCommand = nullptr;

    typedef void (*SettingsCallback)(const String& settingsJson);
    SettingsCallback onSettings = nullptr;

    // ----------------------------------------------------------------
    // Инициализация HTTPS клиента (только из cloud задачи)
    // ----------------------------------------------------------------
    void ensureClient() {
        if (!_clientInitialized) {
            _client.setInsecure();
            _clientInitialized = true;
        }
    }

    // ----------------------------------------------------------------
    // Расчёт текущего интервала с backoff
    // ----------------------------------------------------------------
    unsigned long getBackoffInterval(unsigned long baseInterval) {
        int multiplier = 1;
        for (int i = 0; i < errorCount && i < MAX_BACKOFF_MULTIPLIER; i++) {
            multiplier *= 2;
        }
        return baseInterval * multiplier;
    }

    // =================================================================
    // HTTP методы — вызываются ТОЛЬКО из cloud задачи (core 0)
    // =================================================================

    bool sendTelemetry(const String& jsonData) {
        if (!isConfigured()) return false;

        ensureClient();
        HTTPClient http;

        String url = serverUrl + "?telemetry=1";
        http.begin(_client, url);
        http.setTimeout(2000);
        http.addHeader("Content-Type", "application/json");
        String authHeader = "Bearer " + apiKey;
        http.addHeader("Authorization", authHeader.c_str());

        int httpCode = http.POST(jsonData);

        if (httpCode == 200) {
            http.getString();  // drain response
            http.end();
            return true;
        } else {
            Serial.printf("[Cloud] Telemetry failed: %d\n", httpCode);
            http.end();
            return false;
        }
    }

    bool checkCommands() {
        if (!isConfigured()) return false;

        ensureClient();
        HTTPClient http;

        String url = serverUrl + "?commands=1";
        http.begin(_client, url);
        http.setTimeout(2000);
        String authHeader = "Bearer " + apiKey;
        http.addHeader("Authorization", authHeader.c_str());

        int httpCode = http.GET();

        if (httpCode == 200) {
            String response = http.getString();
            http.end();

            // Если есть команды — парсим и отправляем в очередь
            if (response.indexOf("\"commands\"") != -1) {
                parseCommands(response);
            }
            return true;
        } else {
            if (httpCode == -1) {
                Serial.println("[Cloud] Command check failed: Timeout/Connection error");
            } else {
                Serial.printf("[Cloud] Command check failed: %d\n", httpCode);
            }
            http.end();
        }
        return false;
    }

    bool checkSettings() {
        if (!isConfigured() || onSettings == nullptr) return true;  // не ошибка — фича не используется

        ensureClient();
        HTTPClient http;

        String url = serverUrl + "?settings=1";
        http.begin(_client, url);
        http.setTimeout(2000);
        String authHeader = "Bearer " + apiKey;
        http.addHeader("Authorization", authHeader.c_str());

        int httpCode = http.GET();

        if (httpCode == 200) {
            String response = http.getString();
            http.end();

            if (response.indexOf("\"settings\"") != -1) {
                // Извлекаем timestamp настроек
                int tsPos = response.indexOf("\"settings_last_update\":");
                unsigned long cloudTs = 0;
                if (tsPos != -1) {
                    int start = tsPos + 22;
                    int end = response.indexOf(",", start);
                    if (end == -1) end = response.indexOf("}", start);
                    String tsStr = response.substring(start, end);
                    cloudTs = tsStr.toInt();
                }

                // Применяем только если настройки новее
                if (cloudTs > settingsLastUpdate) {
                    settingsLastUpdate = cloudTs;
                    Serial.printf("[Cloud] Settings updated from cloud (ts: %lu)\n", cloudTs);

                    // Извлекаем JSON объекта настроек
                    int settingsStart = response.indexOf("{\"settings\":");
                    if (settingsStart != -1) {
                        int jsonStart = response.indexOf("{", settingsStart);
                        int depth = 0;
                        int jsonEnd = jsonStart;
                        for (int i = jsonStart; i < response.length(); i++) {
                            if (response.charAt(i) == '{') depth++;
                            else if (response.charAt(i) == '}') {
                                depth--;
                                if (depth == 0) {
                                    jsonEnd = i + 1;
                                    break;
                                }
                            }
                        }
                        String settingsJson = response.substring(jsonStart, jsonEnd);

                        // Отправляем в очередь вместо прямого вызова callback
                        // Callback будет вызван из update() в основном loop
                        if (_settingsQueue && settingsJson.length() > 0) {
                            if (settingsJson.length() < QUEUE_DATA_SIZE) {
                                QueueItem item;
                                memset(item.data, 0, QUEUE_DATA_SIZE);
                                strncpy(item.data, settingsJson.c_str(), QUEUE_DATA_SIZE - 1);
                                if (xQueueSend(_settingsQueue, &item, 0) != pdTRUE) {
                                    Serial.println("[Cloud] WARNING: settings queue full!");
                                }
                            } else {
                                Serial.printf("[Cloud] WARNING: settings JSON too large (%d bytes)\n",
                                              settingsJson.length());
                            }
                        }
                    }
                }
            }
            return true;
        } else {
            Serial.printf("[Cloud] Settings check failed: %d\n", httpCode);
            http.end();
        }
        return false;
    }

    // ----------------------------------------------------------------
    // Парсинг команд — результат в очередь (cloud → main)
    // ----------------------------------------------------------------
    void parseCommands(const String& response) {
        int cmdsStart = response.indexOf("\"commands\"");
        if (cmdsStart == -1) return;

        int arrStart = response.indexOf("[", cmdsStart);
        if (arrStart == -1) return;

        // Находим закрывающую ] по глубине скобок
        int depth = 1;
        int pos = arrStart + 1;
        while (pos < response.length() && depth > 0) {
            char c = response.charAt(pos);
            if (c == '[' || c == '{') depth++;
            else if (c == ']' || c == '}') depth--;
            pos++;
        }
        int arrEnd = pos - 1;

        String commands = response.substring(arrStart + 1, arrEnd);

        // Извлекаем каждую команду и отправляем в очередь
        int cmdPos = 0;
        while (cmdPos < commands.length()) {
            int cmdStart = commands.indexOf("{\"command\":", cmdPos);
            if (cmdStart == -1) break;

            int nameStart = commands.indexOf("\"", cmdStart + 11) + 1;
            int nameEnd = commands.indexOf("\"", nameStart);
            if (nameEnd == -1 || nameEnd <= nameStart) break;

            String cmdName = commands.substring(nameStart, nameEnd);

            // В очередь вместо прямого вызова callback
            if (_commandQueue && cmdName.length() < QUEUE_DATA_SIZE) {
                QueueItem item;
                memset(item.data, 0, QUEUE_DATA_SIZE);
                strncpy(item.data, cmdName.c_str(), QUEUE_DATA_SIZE - 1);
                if (xQueueSend(_commandQueue, &item, 0) != pdTRUE) {
                    Serial.println("[Cloud] WARNING: command queue full!");
                }
                Serial.printf("[Cloud] Command queued: %s\n", cmdName.c_str());
            }

            cmdPos = nameEnd + 1;
        }
    }

    // =================================================================
    // Cloud задача — FreeRTOS, core 0, приоритет 0 (низкий)
    // =================================================================
    // Все HTTP-запросы, backoff, _client — только здесь.
    // Основной loop (core 1) НИКОГДА не вызывает HTTP.
    // =================================================================

    void _cloudTaskLoop() {
        unsigned long lastTelemetryMs = millis();
        unsigned long lastCommandCheckMs = millis();
        unsigned long lastSettingsCheckMs = millis();

        Serial.println("[Cloud] Task started on core " + String(xPortGetCoreID()));

        // === Отладочный мониторинг стека (каждые 60 сек) ===
        unsigned long lastStackCheck = 0;

        while (_running) {
            unsigned long now = millis();

            // === Мониторинг свободного стека (каждые 60 сек) ===
            // Если стек близок к исчерпанию — логируем предупреждение.
            // При Kernel Panic это поможет определить причину.
            if (now - lastStackCheck > 60000) {
                lastStackCheck = now;
                unsigned long freeStack = uxTaskGetStackHighWaterMark(nullptr);
                Serial.printf("[Cloud] Stack free: %u bytes\n", freeStack);
                if (freeStack < 2048) {
                    Serial.printf("[Cloud] WARNING: Stack low! Only %u bytes free!\n", freeStack);
                }
            }

            // Проверяем какие запросы пора сделать
            bool doTelemetry = (now - lastTelemetryMs >= getBackoffInterval(telemetryIntervalMs));
            bool doCommands = (now - lastCommandCheckMs >= getBackoffInterval(commandCheckIntervalMs));
            bool doSettings = (now - lastSettingsCheckMs >= getBackoffInterval(settingsCheckIntervalMs));

            if (doTelemetry || doCommands || doSettings) {
                // Читаем телеметрию из общего буфера (через mutex)
                String telemetry;
                if (xSemaphoreTake(_telemetryMutex, pdMS_TO_TICKS(100))) {
                    telemetry = _telemetryBuffer;
                    xSemaphoreGive(_telemetryMutex);
                }

                // Делаем только ОДИН запрос за цикл
                bool success = false;
                unsigned long baseInterval = telemetryIntervalMs;

                if (doTelemetry) {
                    success = sendTelemetry(telemetry);
                    lastTelemetryMs = millis();
                    baseInterval = telemetryIntervalMs;
                } else if (doCommands) {
                    success = checkCommands();
                    lastCommandCheckMs = millis();
                    baseInterval = commandCheckIntervalMs;
                } else if (doSettings) {
                    success = checkSettings();
                    lastSettingsCheckMs = millis();
                    baseInterval = settingsCheckIntervalMs;
                }

                if (success) {
                    if (errorCount > 0) {
                        Serial.println("[Cloud] Connection restored");
                    }
                    errorCount = 0;
                } else {
                    errorCount++;
                    Serial.printf("[Cloud] Backoff: errorCount=%d, next interval=%lu ms\n",
                                  errorCount, getBackoffInterval(baseInterval));
                }

                // Закрываем мёртвое TCP при ошибках
                if (errorCount > 0) {
                    _client.stop();
                }
            } else {
                // Нечего делать — спим до ближайшего запроса
                // (vTaskDelay НЕ блокирует другие задачи!)
                unsigned long nt = getBackoffInterval(telemetryIntervalMs) - (millis() - lastTelemetryMs);
                unsigned long nc = getBackoffInterval(commandCheckIntervalMs) - (millis() - lastCommandCheckMs);
                unsigned long ns = getBackoffInterval(settingsCheckIntervalMs) - (millis() - lastSettingsCheckMs);
                unsigned long sleepMs = nt;
                if (nc < sleepMs) sleepMs = nc;
                if (ns < sleepMs) sleepMs = ns;
                // Минимум 10мс, максимум 1 сек (чтобы быстро реагировать при смене errorCount)
                if (sleepMs < 10) sleepMs = 10;
                if (sleepMs > 1000) sleepMs = 1000;
                vTaskDelay(pdMS_TO_TICKS(sleepMs));
            }
        }

        Serial.println("[Cloud] Task stopped");
        _taskHandle = nullptr;
        vTaskDelete(nullptr);
    }

    // Статическая точка входа для xTaskCreatePinnedToCore
    static void _cloudTaskEntry(void* param) {
        static_cast<CloudManager*>(param)->_cloudTaskLoop();
    }

public:
    CloudManager() : serverUrl(""), apiKey("") {}

    ~CloudManager() {
        stop();
    }

    // ----------------------------------------------------------------
    // Инициализация — вызывается из setup()
    // Создаёт мьютекс и очереди, cloud задача стартует автоматически
    // при первом вызове update()
    // ----------------------------------------------------------------
    void begin(const String& url, const String& key) {
        serverUrl = url;
        apiKey = key;

        // Создаём примитивы FreeRTOS (одноразово)
        if (!_telemetryMutex) {
            _telemetryMutex = xSemaphoreCreateMutex();
        }
        if (!_commandQueue) {
            _commandQueue = xQueueCreate(5, sizeof(QueueItem));    // До 5 команд в очереди
        }
        if (!_settingsQueue) {
            _settingsQueue = xQueueCreate(2, sizeof(QueueItem));  // До 2 пакетов настроек
        }

        Serial.println("[Cloud] Initialized: " + serverUrl);
    }

    // Запуск cloud задачи (core 0, приоритет 0, стек 16KB)
    void startTask() {
        if (_taskHandle != nullptr) return;  // Уже запущена

        _running = true;
        xTaskCreatePinnedToCore(
            _cloudTaskEntry,   // Функция задачи
            "cloud_task",      // Имя (для отладки)
            16384,             // Стек (16KB — SSL/TLS требует 6-10KB, JSON + буферы ещё ~4KB)
            this,              // Параметр: указатель на CloudManager
            0,                 // Приоритет (низкий, ниже WiFi задач)
            &_taskHandle,      // Handle задачи
            0                  // Core 0 (там WiFi/TCP стек)
        );
    }

    // Остановка cloud задачи
    void stop() {
        _running = false;
        // НЕ вызываем xSemaphoreGive — мьютекс может принадлежать cloud задаче.
        // Cloud задача увидит _running==false и выйдет сама, освободив мьютекс.
    }

    void onCommandReceived(CommandCallback callback) {
        onCommand = callback;
    }

    void onSettingsReceived(SettingsCallback callback) {
        onSettings = callback;
    }

    void setTelemetryInterval(unsigned long ms) {
        telemetryIntervalMs = ms;
    }

    void setCommandCheckInterval(unsigned long ms) {
        commandCheckIntervalMs = ms;
    }

    bool isConfigured() {
        return serverUrl.length() > 0 && apiKey.length() > 0;
    }

    void setSettingsLastUpdate(unsigned long ts) {
        settingsLastUpdate = ts;
    }

    // =================================================================
    // update() — вызывается из основного loop (core 1)
    // =================================================================
    // НЕ делает HTTP-запросов! Занимает микросекунды:
    //   1. Копирует телеметрию в буфер через mutex (~2 мкс)
    //   2. Читает очередь команд — вызывает onCommand callback
    //   3. Читает очередь настроек — вызывает onSettings callback
    //
    // Callback-и вызываются из контекста основного loop — безопасно.
    // =================================================================
    void update(const String& telemetryJson) {
        if (!isConfigured() || !_telemetryMutex) return;

        // Автозапуск cloud задачи при первом вызове
        if (_taskHandle == nullptr && !_running) {
            startTask();
        }

        // 1. Копируем телеметрию в общий буфер (быстро, через mutex)
        if (xSemaphoreTake(_telemetryMutex, pdMS_TO_TICKS(10))) {
            _telemetryBuffer = telemetryJson;
            xSemaphoreGive(_telemetryMutex);
        }

        // 2. Проверяем очередь команд (неблокирующе, 0 мкс если пусто)
        if (_commandQueue && onCommand) {
            QueueItem item;
            while (xQueueReceive(_commandQueue, &item, 0) == pdTRUE) {
                onCommand(String(item.data), "{}");
            }
        }

        // 3. Проверяем очередь настроек (неблокирующе, 0 мкс если пусто)
        if (_settingsQueue && onSettings) {
            QueueItem item;
            while (xQueueReceive(_settingsQueue, &item, 0) == pdTRUE) {
                onSettings(String(item.data));
            }
        }
    }

    // Статус подключения
    bool isConnected() {
        return WiFi.status() == WL_CONNECTED && isConfigured();
    }
};

#endif
