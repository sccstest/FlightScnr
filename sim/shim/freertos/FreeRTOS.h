#pragma once
// FreeRTOS on desktop threads (sim only). 1 tick = 1 ms.
#include <stddef.h>
#include <stdint.h>

typedef int32_t BaseType_t;
typedef uint32_t UBaseType_t;
typedef uint32_t TickType_t;
typedef uint8_t StackType_t;
typedef void (*TaskFunction_t)(void*);

#define pdTRUE ((BaseType_t)1)
#define pdFALSE ((BaseType_t)0)
#define pdPASS pdTRUE
#define pdFAIL pdFALSE
#define portMAX_DELAY ((TickType_t)0xffffffffUL)
#define configTICK_RATE_HZ 1000
#define portTICK_PERIOD_MS 1
#define portTICK_RATE_MS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define pdTICKS_TO_MS(t) ((uint32_t)(t))
#define tskNO_AFFINITY 0x7FFFFFFF
#define tskIDLE_PRIORITY 0
#define configMAX_PRIORITIES 25

typedef struct {
  int unused;
} portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}

#ifdef __cplusplus
extern "C" {
#endif
void sim_critical_enter(void);
void sim_critical_exit(void);
#ifdef __cplusplus
}
#endif

#define portENTER_CRITICAL(mux) sim_critical_enter()
#define portEXIT_CRITICAL(mux) sim_critical_exit()
#define portENTER_CRITICAL_ISR(mux) sim_critical_enter()
#define portEXIT_CRITICAL_ISR(mux) sim_critical_exit()
#define taskENTER_CRITICAL(mux) sim_critical_enter()
#define taskEXIT_CRITICAL(mux) sim_critical_exit()
#define portYIELD_FROM_ISR(...) ((void)0)
#define portYIELD() ((void)0)
