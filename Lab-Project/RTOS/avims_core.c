/* avims_core.c - portable AVIMS scheduling core. See avims_core.h. */
#include "avims_core.h"

void avims_init(avims_scheduler_t *s, int channels)
{
    int i;
    if (channels < 1) channels = 1;
    if (channels > AVIMS_MAX_CHANNELS) channels = AVIMS_MAX_CHANNELS;
    s->channels = channels;
    s->head = s->tail = s->count = 0;
    s->occ = 0;
    s->total_served = 0;
    s->total_wait_ticks = 0;
    s->max_queue_len = 0;
    for (i = 0; i < AVIMS_MAX_CHANNELS; i++) {
        s->committed_id[i] = -1;
        s->committed_move[i] = -1;
    }
}

int avims_enqueue(avims_scheduler_t *s, int id, int movement)
{
    if (s->count >= AVIMS_QCAP) return -1;
    s->q[s->tail].id = id;
    s->q[s->tail].movement = movement;
    s->tail = (s->tail + 1) % AVIMS_QCAP;
    s->count++;
    if (s->count > s->max_queue_len) s->max_queue_len = s->count;
    return 0;
}

/* is `movement` compatible with every currently-committed movement?
 * Two cars with the SAME movement share one lane, so they must cross one after another
 * (not simultaneously) even though a movement doesn't "conflict" with itself in the matrix. */
static int compatible_committed(const avims_scheduler_t *s, int movement)
{
    int i;
    for (i = 0; i < s->channels; i++) {
        if (s->committed_id[i] >= 0 &&
            (movement == s->committed_move[i] ||
             avims_conflict_pair(movement, s->committed_move[i])))
            return 0;
    }
    return 1;
}

/* remove queue element at logical position `pos` (0 = head), preserving FIFS order */
static void queue_remove_at(avims_scheduler_t *s, int pos)
{
    int i, idx;
    for (i = pos; i < s->count - 1; i++) {
        idx = (s->head + i) % AVIMS_QCAP;
        int nxt = (s->head + i + 1) % AVIMS_QCAP;
        s->q[idx] = s->q[nxt];
    }
    s->tail = (s->tail - 1 + AVIMS_QCAP) % AVIMS_QCAP;
    s->count--;
}

static int commit_slot(avims_scheduler_t *s)
{
    int i;
    for (i = 0; i < s->channels; i++)
        if (s->committed_id[i] < 0) return i;
    return -1;
}

int avims_decide(avims_scheduler_t *s, int granted[], int *n)
{
    int g = 0;
    /* Fill free channels. First channel always takes the FIFS head (position 0);
     * remaining channels take the earliest queued vehicle compatible with the
     * already-committed ones -> FIFS-fair, conflict-free, up to `channels`. */
    while (s->occ < s->channels && s->count > 0) {
        int pos = -1, i;
        if (s->occ == 0) {
            /* strict head */
            if (compatible_committed(s, s->q[s->head].movement))
                pos = 0;
            else
                break; /* head blocked by nothing yet impossible; safety */
        } else {
            for (i = 0; i < s->count; i++) {
                int idx = (s->head + i) % AVIMS_QCAP;
                if (compatible_committed(s, s->q[idx].movement)) { pos = i; break; }
            }
        }
        if (pos < 0) break;

        int idx = (s->head + pos) % AVIMS_QCAP;
        int slot = commit_slot(s);
        s->committed_id[slot]   = s->q[idx].id;
        s->committed_move[slot] = s->q[idx].movement;
        granted[g++] = s->q[idx].id;
        s->occ++;
        s->total_served++;
        queue_remove_at(s, pos);
    }
    *n = g;
    return g;
}

void avims_release(avims_scheduler_t *s, int id)
{
    int i;
    for (i = 0; i < s->channels; i++) {
        if (s->committed_id[i] == id) {
            s->committed_id[i] = -1;
            s->committed_move[i] = -1;
            if (s->occ > 0) s->occ--;
            return;
        }
    }
}

int avims_queue_len(const avims_scheduler_t *s) { return s->count; }

int avims_committed_conflict(const avims_scheduler_t *s)
{
    int i, j;
    for (i = 0; i < s->channels; i++)
        for (j = i + 1; j < s->channels; j++)
            if (s->committed_id[i] >= 0 && s->committed_id[j] >= 0 &&
                avims_conflict_pair(s->committed_move[i], s->committed_move[j]))
                return 1;
    return 0;
}

const char *avims_movement_name(int movement)
{
    if (movement < 0 || movement >= AVIMS_NUM_MOVEMENTS) return "??";
    return avims_move_name[movement];
}
