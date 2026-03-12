#ifndef PROCESS_CONFIGS_H
#define PROCESS_CONFIGS_H

// Конфигурация процесса DIST
struct DistConfig {
    float bakStopTemp = 98.0;
    float midtermTemp = 92.0;
    bool useValve = true;
    bool fullPower = false;
    bool mixerEnabled = false;
    int mixerOnTime = 60;
    int mixerOffTime = 180;
};

// Конфигурация процесса RECT
struct RectConfig {
    int nasebTime = 30;
    bool calibration = true;
    bool useHeadValve = true;
    bool bodyValveNC = true;
    bool headsTypeKSS = false;
    int cycleLim = 3;
    float histeresis = 0.5;
    float delta = 0.2;
    float correlation = 1.5;
    
    int headOpenMs = 1000;
    int headCloseMs = 5000;
    int bodyOpenMs = 2000;
    int bodyCloseMs = 10000;
    
    float valveHeadCapacity = 0.0;
    float valveBodyCapacity = 0.0;
    float valve0BodyCapacity = 0.0;
};

#endif