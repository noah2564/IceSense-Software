#pragma once
#include "FreeRTOS.h"
typedef int* SemaphoreHandle_t;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { static int x; return &x; }
inline int xSemaphoreTake(SemaphoreHandle_t, uint32_t) { return 1; }
inline int xSemaphoreGive(SemaphoreHandle_t) { return 1; }
