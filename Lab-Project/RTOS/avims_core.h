/* avims_core.h - portable AVIMS scheduling core (no RTOS dependency).
 *
 * This is the "core logic" the brief asks for, kept free of FreeRTOS so it can be
 * unit-tested natively AND driven by the freeRTOS tasks in avims_freertos.c.
 *
 * It implements exactly the grant rule proven in the UPPAAL model (model/Lab-TC.xml):
 *   a waiting vehicle is granted iff a channel is free (occ < channels) and its
 *   movement does not conflict with any already-committed movement.
 * On top of that it enforces First-In-First-Served ordering with a real queue,
 * which the timed-automata model could not encode with a bounded state space.
 *
 * M/M/1 vs M/M/2 is chosen by `channels` (1 or 2).
 */
#ifndef AVIMS_CORE_H
#define AVIMS_CORE_H

#include "conflict_table.h"   /* AVIMS_NUM_MOVEMENTS, avims_conflict_pair(), names */

#define AVIMS_MAX_CHANNELS 2
#define AVIMS_QCAP         256

typedef struct { int id; int movement; } avims_vehicle_t;

typedef struct {
    int channels;                              /* 1 = M/M/1, 2 = M/M/2 */

    avims_vehicle_t q[AVIMS_QCAP];             /* FIFS ring buffer */
    int head, tail, count;

    int committed_id[AVIMS_MAX_CHANNELS];      /* granted, not yet exited */
    int committed_move[AVIMS_MAX_CHANNELS];
    int occ;                                   /* number committed this round */

    /* metrics for the congestion comparison (RQ2) */
    unsigned long total_served;
    unsigned long total_wait_ticks;            /* summed queue residence */
    int max_queue_len;
} avims_scheduler_t;

void avims_init(avims_scheduler_t *s, int channels);

/* Submit a crossing request (FIFS). Returns 0 on success, -1 if the queue is full. */
int  avims_enqueue(avims_scheduler_t *s, int id, int movement);

/* Grant as many waiting vehicles as free channels allow, honouring FIFS and the
 * conflict rule. Newly granted vehicle ids are written to granted[], count to *n.
 * Returns the number granted. */
int  avims_decide(avims_scheduler_t *s, int granted[], int *n);

/* A granted vehicle has finished crossing and left the box: frees its channel. */
void avims_release(avims_scheduler_t *s, int id);

/* Helpers used by tasks / tests. */
int  avims_queue_len(const avims_scheduler_t *s);
int  avims_committed_conflict(const avims_scheduler_t *s); /* 1 if any pair conflicts (must stay 0) */
const char *avims_movement_name(int movement);

#endif /* AVIMS_CORE_H */
