/* avims_freertos.c - AVIMS on FreeRTOS (POSIX simulator port).
 *
 * Maps the models to tasks (see docs/mapping.md):
 *   VehicleTask[i]  <- Vehicle state machine (05-state-vehicle) / Lab-TC Vehicle template
 *   SchedulerTask   <- Coordination Server (06-state-server) / Lab-TC PrimaryServer
 *   BackupTask      <- BackupServer template (FR-08, NFR-03)
 *
 * IPC:
 *   xReqQueue    : vehicles -> scheduler (crossing requests, FIFS is enforced in core)
 *   xExitQueue   : vehicles -> scheduler (exit notifications, frees a channel)
 *   xSyncQueue   : scheduler -> backup   (state sync before any grant)
 *   xAckQueue    : backup    -> scheduler (sync acknowledgement)
 *   task notify  : scheduler -> vehicle   (grant permission)
 *
 * M/M/1 vs M/M/2 selected by AVIMS_CHANNELS below.
 */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "avims_core.h"
#include <stdio.h>

#ifndef AVIMS_CHANNELS
#define AVIMS_CHANNELS      2      /* 1 = M/M/1, 2 = M/M/2 */
#endif
#define AVIMS_NUM_CARS      4
#define DECISION_LIMIT_MS   50
#define CROSS_TIME_MS       40

typedef enum { MSG_REQUEST, MSG_EXIT } msg_kind_t;
typedef struct { msg_kind_t kind; int id; int movement; } avims_msg_t;

static QueueHandle_t xReqQueue, xExitQueue, xSyncQueue, xAckQueue;
static TaskHandle_t  xVehicleHandle[AVIMS_NUM_CARS];

/* fixed movement per car: NS(1), SS(7) compatible ; ES(4), WS(10) compatible.
 * NS conflicts ES -> nice M/M/2 pairing, matching the UPPAAL move_of[]. */
static const int car_movement[AVIMS_NUM_CARS] = { 1, 7, 4, 10 };

/* ---- Backup coordination server (fault-tolerant replica) ------------------ */
static void BackupTask(void *arg)
{
    (void)arg;
    int token;
    for (;;) {
        if (xQueueReceive(xSyncQueue, &token, portMAX_DELAY) == pdTRUE) {
            /* store replicated state, then acknowledge (NFR-03) */
            xQueueSend(xAckQueue, &token, portMAX_DELAY);
        }
    }
}

/* primary must sync to backup before activating any permission (FR-08) */
static void sync_to_backup(int round)
{
    int ack;
    xQueueSend(xSyncQueue, &round, portMAX_DELAY);
    xQueueReceive(xAckQueue, &ack, portMAX_DELAY);
}

/* ---- Primary coordination server ------------------------------------------ */
static void SchedulerTask(void *arg)
{
    (void)arg;
    avims_scheduler_t sched;
    avims_init(&sched, AVIMS_CHANNELS);
    int round = 0;

    avims_msg_t msg;
    for (;;) {
        /* Serve exits first (frees channels), then requests. Block until any event. */
        if (xQueueReceive(xExitQueue, &msg, 0) == pdTRUE) {
            avims_release(&sched, msg.id);
        } else if (xQueueReceive(xReqQueue, &msg, pdMS_TO_TICKS(5)) == pdTRUE) {
            avims_enqueue(&sched, msg.id, msg.movement);
        } else {
            continue;
        }

        /* Decision phase (bounded by DECISION_LIMIT_MS - trivially met in sim). */
        TickType_t t0 = xTaskGetTickCount();
        int granted[AVIMS_MAX_CHANNELS], n = 0;
        avims_decide(&sched, granted, &n);
        TickType_t dt = (xTaskGetTickCount() - t0) * portTICK_PERIOD_MS;
        configASSERT(dt <= DECISION_LIMIT_MS);

        if (n > 0) {
            sync_to_backup(++round);              /* FR-08 before any grant */
            for (int i = 0; i < n; i++) {
                int id = granted[i];
                printf("[sched] round %d GRANT car %d (%s)  occ=%d qlen=%d\n",
                       round, id, avims_movement_name(car_movement[id]),
                       sched.occ, avims_queue_len(&sched));
                xTaskNotifyGive(xVehicleHandle[id]);   /* grant permission (FR-06) */
            }
            configASSERT(!avims_committed_conflict(&sched));  /* safety (NFR-02) */
        }
    }
}

/* ---- Vehicle -------------------------------------------------------------- */
static void VehicleTask(void *arg)
{
    int id = (int)(long)arg;
    int mv = car_movement[id];
    avims_msg_t req = { MSG_REQUEST, id, mv };
    avims_msg_t ext = { MSG_EXIT,    id, mv };

    for (;;) {
        /* submit crossing request (FR-03) */
        xQueueSend(xReqQueue, &req, portMAX_DELAY);
        /* wait for grant permission (C-02: no entry without permission) */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        printf("        car %d ENTER (%s)\n", id, avims_movement_name(mv));
        vTaskDelay(pdMS_TO_TICKS(CROSS_TIME_MS));       /* crossing */
        printf("        car %d EXIT  (%s)\n", id, avims_movement_name(mv));
        xQueueSend(xExitQueue, &ext, portMAX_DELAY);    /* FR-07 */
        vTaskDelay(pdMS_TO_TICKS(30 + 17 * id));        /* re-approach after a while */
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered so logs survive SIGKILL */
    printf("AVIMS FreeRTOS simulation  (M/M/%d, %d cars)\n",
           AVIMS_CHANNELS, AVIMS_NUM_CARS);

    xReqQueue  = xQueueCreate(AVIMS_QCAP, sizeof(avims_msg_t));
    xExitQueue = xQueueCreate(AVIMS_QCAP, sizeof(avims_msg_t));
    xSyncQueue = xQueueCreate(4, sizeof(int));
    xAckQueue  = xQueueCreate(4, sizeof(int));

    xTaskCreate(SchedulerTask, "sched", configMINIMAL_STACK_SIZE * 4, NULL, 3, NULL);
    xTaskCreate(BackupTask,    "backup", configMINIMAL_STACK_SIZE * 2, NULL, 2, NULL);
    for (long i = 0; i < AVIMS_NUM_CARS; i++)
        xTaskCreate(VehicleTask, "car", configMINIMAL_STACK_SIZE * 2,
                    (void *)i, 1, &xVehicleHandle[i]);

    vTaskStartScheduler();
    for (;;) {}   /* not reached */
    return 0;
}

/* ---- FreeRTOS hooks (required by the POSIX port config) ------------------- */
void vApplicationMallocFailedHook(void) { printf("malloc failed\n"); configASSERT(0); }
void vApplicationStackOverflowHook(TaskHandle_t t, char *n) { (void)t; printf("stack overflow %s\n", n); configASSERT(0); }
void vApplicationIdleHook(void) {}
void vApplicationTickHook(void) {}
void vApplicationGetTimerTaskMemory(StaticTask_t **b, StackType_t **s, uint32_t *n){(void)b;(void)s;(void)n;}
void vApplicationDaemonTaskStartupHook(void) {}
