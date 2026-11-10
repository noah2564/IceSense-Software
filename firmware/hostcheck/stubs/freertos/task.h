#pragma once
#include "FreeRTOS.h"
inline TickType_t xTaskGetTickCount() { return 0; }
inline void vTaskDelayUntil(TickType_t*, TickType_t) {}
inline int xTaskCreatePinnedToCore(void (*)(void*), const char*, int, void*, int, void*, int) { return 1; }
