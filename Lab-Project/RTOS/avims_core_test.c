/* avims_core_test.c - native (no-RTOS) test of the AVIMS core.
 * Mirrors the UPPAAL safety properties: no two conflicting vehicles committed,
 * occupancy <= channels, FIFS head served first, and M/M/2 pairs compatible.
 * Build:  make test   (or: gcc avims_core.c avims_core_test.c -o coretest) */
#include "avims_core.h"
#include <stdio.h>
#include <stdlib.h>

static int failures = 0;
#define CHECK(cond, msg) do { if(!(cond)){ printf("  FAIL: %s\n", msg); failures++; } } while(0)

/* movement ids (approach*3 + intent; N,E,S,W x R,S,L) */
enum { NR=0,NS=1,NL=2, ER=3,ES=4,EL=5, SR=6,SS=7,SL=8, WR=9,WS=10,WL=11 };

static void scenario_mm2(void)
{
    printf("Scenario M/M/2: NS, SS (compatible) + ES (conflicts NS)\n");
    avims_scheduler_t s; avims_init(&s, 2);
    avims_enqueue(&s, 100, NS);
    avims_enqueue(&s, 101, SS);
    avims_enqueue(&s, 102, ES);
    int g[AVIMS_MAX_CHANNELS], n;
    avims_decide(&s, g, &n);
    printf("  granted %d: ", n);
    for (int i=0;i<n;i++) printf("%d ", g[i]);
    printf("| queue left=%d occ=%d\n", avims_queue_len(&s), s.occ);
    CHECK(n == 2, "M/M/2 should grant two compatible vehicles");
    CHECK(g[0] == 100, "FIFS: head (NS,id100) granted first");
    CHECK(g[1] == 101, "second grant is SS,id101 (compatible with NS)");
    CHECK(avims_queue_len(&s) == 1, "ES stays queued (conflicts NS)");
    CHECK(!avims_committed_conflict(&s), "committed pair must be conflict-free");
    /* NS & SS exit, ES now gets served */
    avims_release(&s, 100);
    avims_release(&s, 101);
    avims_decide(&s, g, &n);
    CHECK(n == 1 && g[0] == 102, "ES served after the conflicting pair leaves");
}

static void scenario_mm1(void)
{
    printf("Scenario M/M/1: NS, SS -> only one crosses at a time\n");
    avims_scheduler_t s; avims_init(&s, 1);
    avims_enqueue(&s, 200, NS);
    avims_enqueue(&s, 201, SS);
    int g[AVIMS_MAX_CHANNELS], n;
    avims_decide(&s, g, &n);
    CHECK(n == 1 && g[0] == 200, "M/M/1 grants exactly the FIFS head");
    CHECK(s.occ == 1, "occupancy is 1 under M/M/1");
    avims_release(&s, 200);
    avims_decide(&s, g, &n);
    CHECK(n == 1 && g[0] == 201, "SS served next, FIFS order");
}

static void stress(int channels, int rounds)
{
    printf("Stress: channels=%d rounds=%d\n", channels, rounds);
    avims_scheduler_t s; avims_init(&s, channels);
    int next_id = 0;
    unsigned seed = 12345u;
    for (int r = 0; r < rounds; r++) {
        /* random arrivals */
        int arrivals = (seed = seed*1103515245u+12345u) % 4;
        for (int a = 0; a < arrivals; a++) {
            int mv = (seed = seed*1103515245u+12345u) % AVIMS_NUM_MOVEMENTS;
            avims_enqueue(&s, next_id++, mv);
        }
        int g[AVIMS_MAX_CHANNELS], n;
        avims_decide(&s, g, &n);
        /* invariants that the UPPAAL model proves */
        CHECK(s.occ <= s.channels, "occupancy never exceeds channels");
        CHECK(!avims_committed_conflict(&s), "no conflicting movements committed");
        /* random departures */
        for (int c = 0; c < channels; c++) {
            if (s.committed_id[c] >= 0 && ((seed = seed*1103515245u+12345u) & 1))
                avims_release(&s, s.committed_id[c]);
        }
    }
    printf("  served=%lu maxQ=%d\n", s.total_served, s.max_queue_len);
}

int main(void)
{
    printf("== AVIMS core tests ==\n");
    scenario_mm2();
    scenario_mm1();
    stress(1, 20000);
    stress(2, 20000);
    if (failures == 0) printf("\nALL CORE TESTS PASSED\n");
    else               printf("\n%d CHECK(S) FAILED\n", failures);
    return failures ? 1 : 0;
}
