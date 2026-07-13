// =====================================================================
//  traffic_hls_ga.cpp
//  Smart Traffic Signal Controller - HLS-style Genetic Algorithm scheduler
//
//  Companion implementation for the seminar paper
//    "High-Level Synthesis with Genetic Algorithm Scheduling.
//   
//
//  The program performs SCHEDULING + SIMULATION + METRIC REPORTING only.
//  There is no RTL / VHDL / FPGA generation: the output is an optimized
//  task schedule and its traffic metrics, exactly as in Sections IV-VII.
//
//  Build:  g++ -std=c++17 -O2 -o traffic_hls_ga traffic_hls_ga.cpp
//  Run:    ./traffic_hls_ga
// =====================================================================

#include <algorithm>
#include <array>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------------
//  Task types = computation-resource classes of one lane's pipeline
//  (Section II-B / VI: Capture -> Count -> Density -> Decision -> Update)
// ---------------------------------------------------------------------
enum TaskType { CAPTURE = 0, COUNT, DENSITY, DECISION, UPDATE, N_TYPES };

// ---------------------------------------------------------------------
//  Section V-B: Core data structures
// ---------------------------------------------------------------------
struct TrafficTask {
    std::string name;       // human-readable label, e.g. "LC_Decision"
    int         lane;       // owning lane (0..3 = A..D)
    int         duration;   // control steps
    int         type;       // computation-resource class (TaskType)
};

struct Schedule {
    std::vector<int> order;          // gene i = start step of task i
    int    latency       = 0;
    int    waitingTime   = 0;
    int    signalChanges = 0;
    int    violations    = 0;
    double fitness       = 0.0;
};

struct TaskGraph {
    std::vector<TrafficTask>        task;          // N tasks
    std::vector<std::vector<int>>   succ;          // adjacency (dependencies)
    std::vector<int>                asap, alap;     // mobility windows
    std::array<int, N_TYPES>        units{};        // resource count per type
    int                             Tmax = 0;       // latency budget
    std::vector<int>                decisionTask;   // index of DECISION task per lane
    std::vector<int>                queue;          // vehicles per lane
    int                             threshold = 0;  // density threshold
    int                             normal    = 0;  // nominal green time (s)
};

// ---------------------------------------------------------------------
//  Behavioral specification (Section VI-A)
//  Density-adaptive green time: extend a congested lane, else nominal.
// ---------------------------------------------------------------------
int greenTime(int vehicles, int threshold, int normal) {
    if (vehicles > threshold)   // congested lane
        return normal + 10;     // extend green
    else
        return normal;          // nominal green
}

// ---------------------------------------------------------------------
//  Build the four-lane task graph: 5 chained tasks per lane.
// ---------------------------------------------------------------------
TaskGraph buildGraph(const std::vector<int>& queue, int threshold, int normal) {
    TaskGraph g;
    const char* tn[N_TYPES] = {"Capture", "Count", "Density", "Decision", "Update"};
    int nLanes = static_cast<int>(queue.size());

    g.queue     = queue;
    g.threshold = threshold;
    g.normal    = normal;
    g.decisionTask.assign(nLanes, -1);

    for (int l = 0; l < nLanes; ++l) {
        int prev = -1;
        for (int t = 0; t < N_TYPES; ++t) {
            int id = static_cast<int>(g.task.size());
            std::string nm = std::string("L") + char('A' + l) + "_" + tn[t];
            g.task.push_back({nm, l, 1, t});      // every task lasts 1 control step
            g.succ.push_back({});
            if (prev >= 0) g.succ[prev].push_back(id);   // chain dependency
            if (t == DECISION) g.decisionTask[l] = id;
            prev = id;
        }
    }
    // Only the signal-DECISION stage is the contended resource (two
    // decision units shared by all lanes); the lightweight capture /
    // count / density / update stages are not a bottleneck. This is the
    // realistic case and gives the scheduler a clear choice: which lanes
    // get to decide first.
    for (int t = 0; t < N_TYPES; ++t) g.units[t] = nLanes;   // unconstrained
    g.units[DECISION] = 2;                                    // bottleneck
    return g;
}

// ---------------------------------------------------------------------
//  ASAP / ALAP mobility windows (Section II-C).
// ---------------------------------------------------------------------
void computeMobility(TaskGraph& g, int Tmax) {
    int N = static_cast<int>(g.task.size());
    g.Tmax = Tmax;
    g.asap.assign(N, 0);
    g.alap.assign(N, 0);

    std::vector<std::vector<int>> pred(N);
    for (int i = 0; i < N; ++i)
        for (int j : g.succ[i]) pred[j].push_back(i);

    // ASAP: earliest start respecting predecessors (DAG relaxation).
    for (bool changed = true; changed; ) {
        changed = false;
        for (int i = 0; i < N; ++i)
            for (int p : pred[i]) {
                int v = g.asap[p] + g.task[p].duration;
                if (v > g.asap[i]) { g.asap[i] = v; changed = true; }
            }
    }
    // ALAP: latest start that still meets the latency budget Tmax.
    for (int i = 0; i < N; ++i) g.alap[i] = Tmax - g.task[i].duration;
    for (bool changed = true; changed; ) {
        changed = false;
        for (int i = N - 1; i >= 0; --i)
            for (int s : g.succ[i]) {
                int v = g.alap[s] - g.task[i].duration;
                if (v < g.alap[i]) { g.alap[i] = v; changed = true; }
            }
    }
}

// ---------------------------------------------------------------------
//  Objective helpers (Section III-B / VII).
// ---------------------------------------------------------------------
// Accumulated waiting: each lane's queue weighted by how late its
// signal decision is taken -> rewards serving congested lanes first.
int waitingTime(const Schedule& c, const TaskGraph& g) {
    int w = 0;
    for (int l = 0; l < static_cast<int>(g.decisionTask.size()); ++l)
        w += g.queue[l] * c.order[g.decisionTask[l]];
    return w;
}

// Signal stability: number of green<->nominal transitions along the
// service order of the lanes -> rewards a steady, non-chattering plan.
int signalChanges(const Schedule& c, const TaskGraph& g) {
    std::vector<std::pair<int,int>> svc;          // (start cycle, lane)
    for (int l = 0; l < static_cast<int>(g.decisionTask.size()); ++l)
        svc.push_back({c.order[g.decisionTask[l]], l});
    std::sort(svc.begin(), svc.end());
    int changes = 0, prevExt = -1;
    for (auto& [cyc, l] : svc) {
        (void)cyc;
        int ext = (g.queue[l] > g.threshold) ? 1 : 0;
        if (prevExt != -1 && ext != prevExt) ++changes;
        prevExt = ext;
    }
    return changes;
}

// ---------------------------------------------------------------------
//  Section V-C: Fitness engine. Decodes a schedule into latency,
//  waiting time, signal changes and a constraint-violation count,
//  then maps cost -> fitness via Eq.(7).
// ---------------------------------------------------------------------
double evaluate(Schedule& c, const TaskGraph& g,
                double wL, double wW, double wS, double lam) {
    int N = static_cast<int>(g.task.size());
    int T = 0, viol = 0;
    std::map<std::pair<int,int>,int> active;      // (cycle,type) -> count

    for (int i = 0; i < N; ++i) {
        int s = c.order[i], d = g.task[i].duration;
        T = std::max(T, s + d);
        for (int t = s; t < s + d; ++t)
            active[{t, g.task[i].type}]++;          // resource usage
        for (int j : g.succ[i])                     // precedence (Eq.4)
            if (c.order[j] < s + d) ++viol;         // ... broken
    }
    // resource over-subscription beyond the available units of each type
    for (auto& kv : active) {
        int type = kv.first.second;
        if (kv.second > g.units[type]) viol += kv.second - g.units[type];
    }

    int wait = waitingTime(c, g);
    int chg  = signalChanges(c, g);
    c.latency = T; c.waitingTime = wait; c.signalChanges = chg; c.violations = viol;

    double cost = wL * T + wW * wait + wS * chg;    // Eq.(6): 0.5 / 0.3 / 0.2
    c.fitness = 1.0 / (1.0 + cost + lam * viol);    // Eq.(7)
    return c.fitness;
}

// ---------------------------------------------------------------------
//  Section V-D: Genetic operators.
// ---------------------------------------------------------------------
void crossover(const Schedule& p1, const Schedule& p2,
               Schedule& c1, Schedule& c2) {
    int N = static_cast<int>(p1.order.size());
    int k = 1 + rand() % (N - 1);                   // single cut point
    for (int i = 0; i < N; ++i) {
        c1.order[i] = (i < k) ? p1.order[i] : p2.order[i];
        c2.order[i] = (i < k) ? p2.order[i] : p1.order[i];
    }
}

void mutate(Schedule& c, const TaskGraph& g, double rate) {
    for (int i = 0; i < static_cast<int>(c.order.size()); ++i)
        if ((double)rand() / RAND_MAX < rate) {     // re-draw inside mobility
            int lo = g.asap[i], hi = g.alap[i];
            c.order[i] = lo + (hi > lo ? rand() % (hi - lo + 1) : 0);
        }
}

// Roulette-wheel selection: probability proportional to fitness.
int roulette(const std::vector<Schedule>& pop) {
    double sum = 0.0;
    for (const auto& s : pop) sum += s.fitness;
    double r = (double)rand() / RAND_MAX * sum, acc = 0.0;
    for (int i = 0; i < static_cast<int>(pop.size()); ++i) {
        acc += pop[i].fitness;
        if (acc >= r) return i;
    }
    return static_cast<int>(pop.size()) - 1;
}

// ---------------------------------------------------------------------
//  Valid baseline: a simple FIFO list scheduler. Tasks are placed in the
//  given priority order at the earliest cycle that respects precedence
//  and the per-type unit count, so the result never violates a
//  constraint (Section VII baseline).
// ---------------------------------------------------------------------
Schedule listSchedule(const TaskGraph& g, const std::vector<int>& priority,
                      double wL, double wW, double wS, double lam) {
    int N = static_cast<int>(g.task.size());
    Schedule s;
    s.order.assign(N, -1);
    std::map<std::pair<int,int>,int> used;        // (cycle,type) -> count

    for (int idx : priority) {
        int type = g.task[idx].type;
        int est  = 0;                              // earliest start from preds
        for (int i = 0; i < N; ++i)
            for (int j : g.succ[i])
                if (j == idx && s.order[i] >= 0)
                    est = std::max(est, s.order[i] + g.task[i].duration);
        int c = est;                               // first free resource slot
        while (used[{c, type}] >= g.units[type]) ++c;
        s.order[idx] = c;
        used[{c, type}]++;
    }
    evaluate(s, g, wL, wW, wS, lam);
    return s;
}

// ---------------------------------------------------------------------
//  Section IV-B: Genetic scheduler main loop (Algorithm 1).
// ---------------------------------------------------------------------
Schedule runGA(const TaskGraph& g, int P, int G, double mutRate,
               double wL, double wW, double wS, double lam) {
    int N = static_cast<int>(g.task.size());

    auto makeRandom = [&](Schedule& c) {
        c.order.resize(N);
        for (int i = 0; i < N; ++i) {
            int lo = g.asap[i], hi = g.alap[i];
            c.order[i] = lo + (hi > lo ? rand() % (hi - lo + 1) : 0);
        }
    };

    std::vector<Schedule> pop(P);
    pop[0].order = g.asap;          // seed with the two known valid schedules
    pop[1].order = g.alap;
    for (int i = 2; i < P; ++i) makeRandom(pop[i]);
    for (auto& c : pop) evaluate(c, g, wL, wW, wS, lam);

    Schedule best = *std::max_element(
        pop.begin(), pop.end(),
        [](const Schedule& a, const Schedule& b) { return a.fitness < b.fitness; });

    for (int gen = 0; gen < G; ++gen) {
        std::sort(pop.begin(), pop.end(),
            [](const Schedule& a, const Schedule& b) { return a.fitness > b.fitness; });

        std::vector<Schedule> next;
        int elite = std::max(1, P / 10);            // elitism: keep top 10%
        for (int i = 0; i < elite; ++i) next.push_back(pop[i]);

        while (static_cast<int>(next.size()) < P) {
            const Schedule& p1 = pop[roulette(pop)];
            const Schedule& p2 = pop[roulette(pop)];
            Schedule c1, c2;
            c1.order.resize(N); c2.order.resize(N);
            crossover(p1, p2, c1, c2);
            mutate(c1, g, mutRate);
            mutate(c2, g, mutRate);
            evaluate(c1, g, wL, wW, wS, lam);
            evaluate(c2, g, wL, wW, wS, lam);
            next.push_back(c1);
            if (static_cast<int>(next.size()) < P) next.push_back(c2);
        }
        pop.swap(next);
        for (const auto& c : pop)
            if (c.fitness > best.fitness) best = c;
    }
    return best;
}

// ---------------------------------------------------------------------
//  Pretty-printing.
// ---------------------------------------------------------------------
void printSchedule(const Schedule& s, const TaskGraph& g) {
    std::cout << "  Cycle | Active tasks (lane_stage)\n";
    std::cout << "  ------+--------------------------------------\n";
    for (int cyc = 0; cyc < s.latency; ++cyc) {
        std::cout << "    " << std::setw(2) << cyc << "  | ";
        bool any = false;
        for (int i = 0; i < static_cast<int>(g.task.size()); ++i)
            if (s.order[i] == cyc) {
                std::cout << g.task[i].name << "  ";
                any = true;
            }
        if (!any) std::cout << "(idle)";
        std::cout << "\n";
    }
}

void printMetrics(const std::string& label, const Schedule& s) {
    std::cout << std::left << std::setw(16) << label
              << " latency=" << std::setw(2) << s.latency
              << "  waiting=" << std::setw(4) << s.waitingTime
              << "  signalChanges=" << s.signalChanges
              << "  violations=" << s.violations << "\n";
}

// ---------------------------------------------------------------------
int main() {
    std::srand(42);                 // fixed seed -> reproducible run
                                    // (use std::srand(std::time(nullptr)) for variety)

    // ---- Inputs (Section VI-A) -------------------------------------
    std::vector<int> queue = {20, 12, 30, 5};   // Lanes A, B, C, D
    int threshold = 15, normal = 20;

    std::cout << "=== Behavioral specification (density-adaptive green) ===\n";
    for (int l = 0; l < static_cast<int>(queue.size()); ++l)
        std::cout << "  Lane " << char('A' + l)
                  << ": " << std::setw(2) << queue[l] << " vehicles -> green = "
                  << greenTime(queue[l], threshold, normal) << " s\n";

    // ---- Build task graph + mobility -------------------------------
    TaskGraph g = buildGraph(queue, threshold, normal);
    computeMobility(g, /*Tmax=*/8);

    // ---- GA weights (Eq. 6) and parameters -------------------------
    const double wL = 0.5, wW = 0.3, wS = 0.2;   // latency / waiting / signal
    const double lambda = 1000.0;                // hard constraint penalty
    const int    P = 200, Gmax = 400;
    const double mutRate = 0.20;

    // ---- Valid baseline: FIFO list scheduler (lanes in order A..D) --
    std::vector<int> fifoPriority;               // stage-major within each lane
    for (int l = 0; l < static_cast<int>(queue.size()); ++l)
        for (int t = 0; t < N_TYPES; ++t)
            fifoPriority.push_back(l * N_TYPES + t);
    Schedule fifo = listSchedule(g, fifoPriority, wL, wW, wS, lambda);

    // ---- Run the genetic scheduler ---------------------------------
    Schedule best = runGA(g, P, Gmax, mutRate, wL, wW, wS, lambda);

    std::cout << "\n=== Best GA schedule (Tmax=" << g.Tmax << ") ===\n";
    printSchedule(best, g);

    std::cout << "\n=== Comparison (valid schedules; lower is better) ===\n";
    printMetrics("FIFO baseline", fifo);
    printMetrics("GA scheduler", best);

    double impr = fifo.waitingTime ? 100.0 * (fifo.waitingTime - best.waitingTime)
                                            / fifo.waitingTime
                                   : 0.0;
    std::cout << "\nWaiting-time reduction vs. FIFO baseline: "
              << std::fixed << std::setprecision(1) << impr << " %\n";

    return 0;
}
