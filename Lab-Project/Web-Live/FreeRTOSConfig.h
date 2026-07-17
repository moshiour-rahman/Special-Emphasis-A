/* FreeRTOSConfig.h - POSIX simulator config for the interactive AVIMS engine.
 * Larger heap than the batch demo because we run a pool of worker tasks. */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#define configUSE_PREEMPTION            1
#define configUSE_IDLE_HOOK             1
#define configUSE_TICK_HOOK             1
#define configTICK_RATE_HZ              1000
#define configMINIMAL_STACK_SIZE        ((unsigned short)PTHREAD_STACK_MIN)
#define configTOTAL_HEAP_SIZE           ((size_t)(16 * 1024 * 1024))
#define configMAX_TASK_NAME_LEN         16
#define configUSE_TRACE_FACILITY        0
#define configUSE_16_BIT_TICKS          0
#define configIDLE_SHOULD_YIELD         1
#define configUSE_MUTEXES               1
#define configUSE_RECURSIVE_MUTEXES     1
#define configUSE_COUNTING_SEMAPHORES   1
#define configQUEUE_REGISTRY_SIZE       8
#define configUSE_TASK_NOTIFICATIONS    1
#define configMAX_PRIORITIES            7
#define configSUPPORT_STATIC_ALLOCATION 0
#define configSUPPORT_DYNAMIC_ALLOCATION 1

#define configUSE_TIMERS                1
#define configTIMER_TASK_PRIORITY       (configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH        10
#define configTIMER_TASK_STACK_DEPTH    (configMINIMAL_STACK_SIZE * 2)

#define configCHECK_FOR_STACK_OVERFLOW  0
#define configUSE_MALLOC_FAILED_HOOK    1
#define configUSE_DAEMON_TASK_STARTUP_HOOK 0

#define configUSE_CO_ROUTINES           0
#define configMAX_CO_ROUTINE_PRIORITIES 1

#define INCLUDE_vTaskPrioritySet        1
#define INCLUDE_uxTaskPriorityGet       1
#define INCLUDE_vTaskDelete             1
#define INCLUDE_vTaskCleanUpResources   1
#define INCLUDE_vTaskSuspend            1
#define INCLUDE_vTaskDelayUntil         1
#define INCLUDE_vTaskDelay              1
#define INCLUDE_xTaskGetSchedulerState  1
#define INCLUDE_xTaskGetCurrentTaskHandle 1
#define INCLUDE_xTaskGetIdleTaskHandle  1
#define INCLUDE_eTaskGetState           1

#define configASSERT(x) if((x)==0){ printf("ASSERT %s:%d\n", __FILE__, __LINE__); for(;;); }

#include <stdio.h>
#endif /* FREERTOS_CONFIG_H */
