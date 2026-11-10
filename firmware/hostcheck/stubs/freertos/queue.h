#pragma once
#include <cstring>
#include <deque>
#include <string>
#include "FreeRTOS.h"
struct QueueStub { size_t item, cap; std::deque<std::string> q; };
typedef QueueStub* QueueHandle_t;
inline QueueHandle_t xQueueCreate(size_t n, size_t item) { return new QueueStub{item, n, {}}; }
inline int xQueueSend(QueueHandle_t h, const void* p, uint32_t) { if (h->q.size() >= h->cap) return 0; h->q.emplace_back((const char*)p, h->item); return 1; }
inline int xQueueReceive(QueueHandle_t h, void* p, uint32_t) { if (h->q.empty()) return 0; memcpy(p, h->q.front().data(), h->item); h->q.pop_front(); return 1; }
