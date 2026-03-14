#ifndef PROCESS_ENGINE_H
#define PROCESS_ENGINE_H

#include "SensorAdapter.h"
#include "OutputManager.h"
#include "ProcessCommon.h"
#include "preferences.h"
#include "ValveCalMenu.h"
#include "SetPwAsMenu.h"
#include "NotifyCategory.h"  // Вынесено для избежания циклических зависимостей

// Предварительное объявление
class AppNetwork;

enum class TestType { NONE, HEAD, BODY };

struct TestStatus {
    bool active = false;
    TestType type = TestType::NONE;
    unsigned long startTime = 0;
    int durationSec = 0;
    // Снимок параметров на момент старта теста (для расчёта после завершения)
    int openSec = 0;    // headOpenMs или bodyOpenMs
    int closeSec = 0;   // headCloseMs или bodyCloseMs
    bool awaitingInput = false;  // тест завершён, ждём мл от оператора
};
class ProcessEngine {
public:
    // ИЗМЕНЕНО: Добавлен lcd и appNetwork
    void begin(LiquidCrystal_I2C* lcd, SensorAdapter* sensors, OutputManager* outputs, ConfigManager* cfgMgr);
    void setAppNetwork(AppNetwork* net) { appNetwork = net; }
    
    // === МЕТОД УВЕДОМЛЕНИЙ ===
    void sendNotification(NotifyCategory category, const String& message);
    void update(); 
    EngineResponse handleCommand(UiCommand command);
    const SystemStatus& getStatus() const;
    const SensorData& getSensorData() { return sensorAdapter->getData(); } // Геттер датчиков
    bool startProcess(ProcessType type);
    bool stopCurrentProcess();
    void emergencyStop();
    
    bool isProcessRunning() const;
    ProcessType getActiveProcessType() const;
    String getProcessName() const;
    String getStageName() const;
    // Методы для получения статуса теста
        // === НОВЫЕ МЕТОДЫ ДЛЯ РАЗДЕЛЬНЫХ ТЕСТОВ ===
    bool isHeadTestActive() const { return headTestStatus.active; }
    int getHeadTestRemaining() const { 
        if (!headTestStatus.active) return 0;
        unsigned long elapsed = (millis() - headTestStatus.startTime) / 1000;
        return (elapsed < headTestStatus.durationSec) ? (headTestStatus.durationSec - elapsed) : 0;
    }
    int getHeadTestTotal() const { return headTestStatus.durationSec; }

    bool isBodyTestActive() const { return bodyTestStatus.active; }
    int getBodyTestRemaining() const { 
        if (!bodyTestStatus.active) return 0;
        unsigned long elapsed = (millis() - bodyTestStatus.startTime) / 1000;
        return (elapsed < bodyTestStatus.durationSec) ? (bodyTestStatus.durationSec - elapsed) : 0;
    }
    int getBodyTestTotal() const { return bodyTestStatus.durationSec; }
    // Геттеры снимка параметров теста
int getHeadTestOpenSec()    const { return headTestStatus.openSec; }
int getHeadTestCloseSec()   const { return headTestStatus.closeSec; }
int getHeadTestDuration()   const { return headTestStatus.durationSec; }
int getBodyTestOpenSec()    const { return bodyTestStatus.openSec; }
int getBodyTestCloseSec()   const { return bodyTestStatus.closeSec; }
int getBodyTestDuration()   const { return bodyTestStatus.durationSec; }

void clearHeadTestAwait()   { headTestStatus.awaitingInput = false; }
void clearBodyTestAwait()   { bodyTestStatus.awaitingInput = false; }
    // Публичные методы для кнопок (для menu_rect.h)
    void handleUiUp();
    void handleUiDown();
    void handleUiSet();
    void handleUiBack();
    void updateNetworkStatus(char networkSymbol);  // 'W' / 'A' / 'X'
    // Геттеры состояния выходов (для JSON веб-интерфейса)
    bool isHeaterOn()       const { return outputManager ? outputManager->isHeaterOn()      : false; }
    bool isMixerOn()        const { return outputManager ? outputManager->isMixerOn()        : false; }
    bool isWaterValveOpen() const { return outputManager ? outputManager->isWaterValveOpen() : false; }
    // Геттеры референтных значений TELO
    float getRtsarM()  const { return rtsarM; }
    float getAdPressM() const { return adPressM; }
private:
    SensorAdapter* sensorAdapter;
    OutputManager* outputManager;
    ConfigManager* configManager;
    ValveCalMenu* valveCalMenu;
    SetPwAsMenu* setPwAsMenu;
    AppNetwork* appNetwork = nullptr;  // Ссылка на сеть для уведомлений
    TestStatus headTestStatus;
    TestStatus bodyTestStatus;
    DistConfig distConfig;
    RectConfig rectConfig;
    
    SystemStatus currentStatus;
    ProcessType activeProcess = PROCESS_NONE;
    
    bool processRunning = false;
    bool processPaused = false;
    bool emergencyState = false;
    bool dialogPending = false;
    unsigned long lastShporaAdjustTime = 0; // Время последней корректировки Шпоры
    unsigned long processStartTime = 0;
    unsigned long stageStartTime = 0;
    
    enum class Stage {
        IDLE,
        WATER_TEST,
        RAZGON,
        WAITING,
        OTBOR,
        REPLACEMENT,
        BAKSTOP,
        NASEBYA,
        VALVE_CAL,
        SET_PW_AS,
        GOLOVY,
        GOLOVY_OK, // Новый этап
        TELO,
        FINISHING_WORK
    };

    // Подэтапы GOLOVY
    enum class GolovyStage {
        IDLE,
        ST_MAIN,        // Стандартный метод (10%)
        KSS_SPIT,       // КСС этап 1: Плевок (2%)
        KSS_STANDARD,   // КСС этап 2: Стандарт (3%)
        KSS_AKATELO     // КСС этап 3: Добор (15%)
    };
    
    GolovyStage currentGolovyStage = GolovyStage::IDLE;
    
            // Переменные для GOLOVY
    unsigned long golovyTargetTime = 0; // Время цели для этапов GOLOVY (сек)
    
    // Переменные для TELO
    float rtsarM = 0.0f;          // Запомненная температура TSAR (Reference Temp)
    float adPressM = 0.0f;         // Запомненное давление (гПа)
    
    // Переменные для метода Шпора
    float bodyOpenCor = 0.0f;      // Скорректированное время открытия клапана тела (мс)
    float speedShpora = 0.0f;      // Текущая скорость отбора для Шпоры (л/ч)
    
    // Вспомогательные переменные расчета
    float koff = 0.0f;
    float speedGolovy = 0.0f; // Скорость голов (мл/ч)
    float speedTelo = 0.0f;   // Скорость тела (мл/ч)
    
    // Накопленный объём голов и таймер расчёта
    float headsVolDone = 0.0f;
    unsigned long lastVolCalcTime = 0;
    
    Stage currentStage = Stage::IDLE;
    Stage previousStage = Stage::IDLE;
    
    int counter = 0;
    float initialTsarTemp = 0.0f;
    bool razgonMixerStarted = false;
    bool midtermHandled = false;
    
    void checkSafety();
    // Проверка готовности датчиков перед запуском процесса.
    // Возвращает true если все 4 датчика откалиброваны и отвечают.
    // При false заполняет errorMsg именами проблемных датчиков (для LCD и лога).
    bool checkSensorsReady(String& errorMsg);
    unsigned long alarmStartTime = 0;
    bool alarmTSA_Active = false;
    bool alarmBOX_Active = false;
    
    void printStartupInfo();
    
    void handleIdleState();
    void handleWaterTest();
    void handleRazgon();
    void handleWaiting();
    
    void handleDistOtbor();
    void handleDistReplacement();
    void handleDistBakstop();
    
    void handleNasebya();
    void handleRectProcess(); 
    void handleGolovyOk();
    void handleFinishingWork();
    
    void updateDisplayData();
    void syncConfig();
    void changeStage(Stage newStage);
    String formatTimeMMSS(unsigned long seconds) const;

    void handleGolovy();
    void startStandardGolovy(SystemConfig& cfg);
    void startKssSpit(SystemConfig& cfg);
    void startKssStandard(SystemConfig& cfg);
    void startKssAkaTelo(SystemConfig& cfg);
    void finishGolovyStage();
    void handleTelo();
    void finishTelo(SystemConfig& cfg);
    const char* getStageName(Stage stage);
    void updateRectWebInfo();
};

#endif