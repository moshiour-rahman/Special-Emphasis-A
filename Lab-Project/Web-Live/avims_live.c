/* avims_live.c - INTERACTIVE AVIMS on real FreeRTOS (POSIX port).
 *
 * Reads commands on STDIN (one per line) and streams JSON events on STDOUT:
 *   commands: ADD <mv> | CH <1|2> | CROSS <ms> | PAUSE | RESUME | RESET
 *   events  : hello, request, grant, enter, exit, state, paused, reset
 *
 * A small Python bridge (server.py) relays stdin/stdout to/from the browser, so what you see
 * is produced by REAL FreeRTOS tasks, queues and semaphores.
 *
 * Design (queue is decoupled from the workers, so you can add cars at any time):
 *   SchedulerTask  - the ONLY owner of the core scheduler. Assigns car ids, enqueues
 *                    requests, runs avims_decide() (bounded 50 ms), syncs the backup, and
 *                    hands each granted car to a crossing worker. Skips granting while paused.
 *   CrossWorker[K] - a tiny fixed pool (K=4). Since at most `channels` (<=2) cars cross at a
 *                    time, this never stalls. Each worker times the crossing (pausable) and
 *                    emits enter/exit, then tells the scheduler to release the channel.
 *   BackupTask     - fault-tolerant replica (sync handshake).
 *   CommandTask    - non-blocking stdin reader -> pushes commands onto one queue.
 */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#include "avims_core.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#define NCROSS            4        /* crossing workers (>= max channels) */
#define MAXID             8192
#define DECISION_LIMIT_MS 50
#define STEP_MS           40       /* crossing time granularity (for pause) */

enum { CMD_ADD = 1, CMD_CH, CMD_CROSS, CMD_RESET, CMD_PAUSE, CMD_RESUME };
typedef struct { int type; int val; } cmd_t;

typedef struct { int id; int mv; int dur; } cross_t;

static QueueHandle_t xCmdQueue;     /* CommandTask -> SchedulerTask                */
static QueueHandle_t xCrossQueue;   /* SchedulerTask -> a CrossWorker (granted car) */
static QueueHandle_t xExitQueue;    /* CrossWorker -> SchedulerTask (release id)     */
static QueueHandle_t xSyncQueue, xAckQueue;
static SemaphoreHandle_t xEmit;

static int   mvOf[MAXID];
static volatile int g_channels = 2;
static volatile int g_crossMs  = 2200;
static volatile int g_paused   = 0;

static void emit(const char *fmt, ...)
{
    va_list ap;
    xSemaphoreTake(xEmit, portMAX_DELAY);
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    putchar('\n'); fflush(stdout);
    xSemaphoreGive(xEmit);
}

/* ---- backup coordination server ------------------------------------------ */
static void BackupTask(void *arg)
{
    (void)arg; int tok;
    for (;;)
        if (xQueueReceive(xSyncQueue, &tok, portMAX_DELAY) == pdTRUE)
            xQueueSend(xAckQueue, &tok, portMAX_DELAY);
}
static void sync_to_backup(int round)
{
    int ack;
    xQueueSend(xSyncQueue, &round, portMAX_DELAY);
    xQueueReceive(xAckQueue, &ack, portMAX_DELAY);
}

/* ---- crossing worker pool ------------------------------------------------- */
static void CrossWorker(void *arg)
{
    (void)arg;
    cross_t c;
    for (;;) {
        xQueueReceive(xCrossQueue, &c, portMAX_DELAY);
        emit("{\"ev\":\"enter\",\"id\":%d,\"mv\":%d,\"dur\":%d}", c.id, c.mv, c.dur);
        int remaining = c.dur;
        while (remaining > 0) {
            vTaskDelay(pdMS_TO_TICKS(STEP_MS));
            if (!g_paused) remaining -= STEP_MS;     /* time only advances when running */
        }
        emit("{\"ev\":\"exit\",\"id\":%d}", c.id);
        int id = c.id;
        xQueueSend(xExitQueue, &id, portMAX_DELAY);  /* free the channel */
    }
}

/* ---- primary coordination server ----------------------------------------- */
static void SchedulerTask(void *arg)
{
    (void)arg;
    avims_scheduler_t sched;
    avims_init(&sched, g_channels);
    int round = 0, tick = 0, nextId = 0;

    for (;;) {
        cmd_t cm;
        while (xQueueReceive(xCmdQueue, &cm, 0) == pdTRUE) {
            switch (cm.type) {
            case CMD_ADD:
                if (avims_enqueue(&sched, nextId, cm.val) == 0) {
                    mvOf[nextId % MAXID] = cm.val;
                    emit("{\"ev\":\"request\",\"id\":%d,\"mv\":%d}", nextId, cm.val);
                    nextId++;
                }
                break;
            case CMD_CH:    sched.channels = (cm.val == 1) ? 1 : 2; g_channels = sched.channels; break;
            case CMD_CROSS: g_crossMs = cm.val; break;
            case CMD_PAUSE: g_paused = 1; emit("{\"ev\":\"paused\",\"paused\":1}"); break;
            case CMD_RESUME:g_paused = 0; emit("{\"ev\":\"paused\",\"paused\":0}"); break;
            case CMD_RESET: avims_init(&sched, g_channels); emit("{\"ev\":\"reset\"}"); break;
            }
        }

        int id;
        while (xQueueReceive(xExitQueue, &id, 0) == pdTRUE)
            avims_release(&sched, id);               /* a car finished crossing */

        if (!g_paused) {
            TickType_t t0 = xTaskGetTickCount();
            int granted[AVIMS_MAX_CHANNELS], n = 0;
            avims_decide(&sched, granted, &n);
            TickType_t dt = (xTaskGetTickCount() - t0) * portTICK_PERIOD_MS;
            configASSERT(dt <= DECISION_LIMIT_MS);

            if (n > 0) {
                sync_to_backup(++round);
                for (int i = 0; i < n; i++) {
                    emit("{\"ev\":\"grant\",\"id\":%d,\"occ\":%d,\"qlen\":%d}",
                         granted[i], sched.occ, avims_queue_len(&sched));
                    cross_t c = { granted[i], mvOf[granted[i] % MAXID], g_crossMs };
                    xQueueSend(xCrossQueue, &c, portMAX_DELAY);
                }
                configASSERT(!avims_committed_conflict(&sched));
            }
        }

        if (++tick % 12 == 0)
            emit("{\"ev\":\"state\",\"qlen\":%d,\"occ\":%d,\"ch\":%d,\"served\":%lu,\"paused\":%d}",
                 avims_queue_len(&sched), sched.occ, sched.channels, sched.total_served, g_paused);

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ---- non-blocking stdin command reader ------------------------------------ */
static void handle_line(char *line)
{
    char *tok = strtok(line, " \t\r\n");
    if (!tok) return;
    if (strcmp(tok, "ADD") == 0) {
        char *a = strtok(NULL, " \t\r\n");
        if (a) { int mv = atoi(a); if (mv >= 0 && mv < AVIMS_NUM_MOVEMENTS) {
                     cmd_t c = { CMD_ADD, mv }; xQueueSend(xCmdQueue, &c, 0); } }
    } else if (strcmp(tok, "CH") == 0) {
        char *a = strtok(NULL, " \t\r\n");
        if (a) { cmd_t c = { CMD_CH, atoi(a) }; xQueueSend(xCmdQueue, &c, 0); }
    } else if (strcmp(tok, "CROSS") == 0) {
        char *a = strtok(NULL, " \t\r\n");
        if (a) { cmd_t c = { CMD_CROSS, atoi(a) }; xQueueSend(xCmdQueue, &c, 0); }
    } else if (strcmp(tok, "PAUSE") == 0) {
        cmd_t c = { CMD_PAUSE, 0 }; xQueueSend(xCmdQueue, &c, 0);
    } else if (strcmp(tok, "RESUME") == 0) {
        cmd_t c = { CMD_RESUME, 0 }; xQueueSend(xCmdQueue, &c, 0);
    } else if (strcmp(tok, "RESET") == 0) {
        cmd_t c = { CMD_RESET, 0 }; xQueueSend(xCmdQueue, &c, 0);
    }
}
static void CommandTask(void *arg)
{
    (void)arg;
    fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL, 0) | O_NONBLOCK);
    static char buf[128]; int len = 0;
    for (;;) {
        char ch; int r = read(STDIN_FILENO, &ch, 1);
        if (r == 1) {
            if (ch == '\n') { buf[len] = 0; handle_line(buf); len = 0; }
            else if (len < (int)sizeof(buf) - 1) buf[len++] = ch;
        } else {
            vTaskDelay(pdMS_TO_TICKS(8));
        }
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    xEmit       = xSemaphoreCreateMutex();
    xCmdQueue   = xQueueCreate(64, sizeof(cmd_t));
    xCrossQueue = xQueueCreate(16, sizeof(cross_t));
    xExitQueue  = xQueueCreate(16, sizeof(int));
    xSyncQueue  = xQueueCreate(4, sizeof(int));
    xAckQueue   = xQueueCreate(4, sizeof(int));

    emit("{\"ev\":\"hello\",\"channels\":%d,\"workers\":%d,\"cap\":%d}",
         g_channels, NCROSS, AVIMS_QCAP);

    xTaskCreate(SchedulerTask, "sched",  configMINIMAL_STACK_SIZE * 4, NULL, 3, NULL);
    xTaskCreate(BackupTask,    "backup", configMINIMAL_STACK_SIZE * 2, NULL, 2, NULL);
    xTaskCreate(CommandTask,   "cmd",    configMINIMAL_STACK_SIZE * 2, NULL, 2, NULL);
    for (long i = 0; i < NCROSS; i++)
        xTaskCreate(CrossWorker, "cross", configMINIMAL_STACK_SIZE * 2, (void *)i, 1, NULL);

    vTaskStartScheduler();
    for (;;) {}
    return 0;
}

/* ---- FreeRTOS hooks ------------------------------------------------------- */
void vApplicationMallocFailedHook(void) { fprintf(stderr, "malloc failed\n"); configASSERT(0); }
void vApplicationStackOverflowHook(TaskHandle_t t, char *n) { (void)t; fprintf(stderr, "stack overflow %s\n", n); configASSERT(0); }
void vApplicationIdleHook(void) {}
void vApplicationTickHook(void) {}
void vApplicationGetTimerTaskMemory(StaticTask_t **b, StackType_t **s, uint32_t *n){(void)b;(void)s;(void)n;}
void vApplicationDaemonTaskStartupHook(void) {}
