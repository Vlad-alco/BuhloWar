#ifndef NOTIFY_CATEGORY_H
#define NOTIFY_CATEGORY_H

// === КАТЕГОРИИ УВЕДОМЛЕНИЙ ===
// Вынесено в отдельный файл для избежания циклических зависимостей
enum class NotifyCategory {
    ALARM,          // Тревоги (TSA, BOX, VREAC, EMERGENCY) - ВСЕГДА отправляются
    PROCESS_BASIC,  // Процесс базовый (START, FINISH, CANCEL) - ВСЕГДА отправляются
    ATTENTION,      // Требует внимания оператора - ВСЕГДА отправляются
    SYSTEM,         // Система (WiFi, восстановление) - настраиваемый
    DISTILLATION,   // Дистилляция (RAZGON, WAITING, OTBOR, BAKSTOP) - настраиваемый
    RECTIFICATION,  // Ректификация (GOLOVY, TELO, NASEBYA) - настраиваемый
    SENSORS         // Датчики (BME280 проблемы) - настраиваемый
};

#endif // NOTIFY_CATEGORY_H
