#pragma once
#include "freertos/FreeRTOS.h"

struct SimTask;
typedef SimTask* TaskHandle_t;

#ifdef __cplusplus
extern "C" {
#endif
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char* name, uint32_t stack_depth,
                                   void* param, UBaseType_t priority, TaskHandle_t* out,
                                   BaseType_t core);
BaseType_t xTaskCreate(TaskFunction_t fn, const char* name, uint32_t stack_depth, void* param,
                       UBaseType_t priority, TaskHandle_t* out);
/** vTaskDelete(NULL) ends the calling task (never returns). Deleting another task is ignored. */
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
TickType_t xTaskGetTickCount(void);
const char* pcTaskGetName(TaskHandle_t task);
BaseType_t xPortGetCoreID(void);
#ifdef __cplusplus
}
#endif
