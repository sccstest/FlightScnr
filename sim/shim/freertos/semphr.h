#pragma once
#include "freertos/FreeRTOS.h"

struct SimSemaphore;
typedef SimSemaphore* SemaphoreHandle_t;
typedef SimSemaphore* QueueHandle_t;

#ifdef __cplusplus
extern "C" {
#endif
SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
SemaphoreHandle_t xSemaphoreCreateCounting(UBaseType_t max_count, UBaseType_t initial);
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem);
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t sem, TickType_t ticks);
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t sem);
UBaseType_t uxSemaphoreGetCount(SemaphoreHandle_t sem);
void vSemaphoreDelete(SemaphoreHandle_t sem);
#ifdef __cplusplus
}
#endif

#define xSemaphoreGiveFromISR(sem, woken) xSemaphoreGive(sem)
#define xSemaphoreTakeFromISR(sem, woken) xSemaphoreTake(sem, 0)
