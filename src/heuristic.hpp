#pragma once

#include <cstdint>
#include <vector>

#include "bounds.hpp"
#include "clock.hpp"
#include "opgraph.hpp"

// List scheduling: at each step, each kind runs its ready ops with the largest keys.
// Returns the step (>= 1) of every op.
std::vector<std::int32_t> list_schedule(OpGraph const &g, std::vector<std::int64_t> const &key);

// List scheduling that tries not to starve the machines of the kinds in `target_kinds` (bit
// mask): each step it looks at the unscheduled target ops with the fewest unscheduled ancestors
// (at most `horizon`) and runs their ready ancestors first; free units are then filled by key.
struct UnlockOptions {
    unsigned target_kinds = 0;
    unsigned driven_kinds = 7; // kinds whose units may be claimed by a target's ancestors
    std::int32_t horizon = 64;
    std::int32_t max_targets = 16; // targets examined per step
};
std::vector<std::int32_t> unlock_schedule(OpGraph const &g, Closure const &closure,
                                          std::vector<std::int64_t> const &key,
                                          UnlockOptions const &options);

struct HeuristicResult {
    std::vector<std::int32_t> step; // per op index
    std::int32_t latency = 0;
    std::int32_t lower_bound = 0;
    std::vector<std::int32_t> head, tail; // resource-aware bounds, reused by the ILP
    std::int32_t first_latency = 0;       // latency of the first (plain tail-priority) pass
    std::int32_t passes = 0;
};

HeuristicResult heuristic_schedule(OpGraph const &g, Clock const &clock, double deadline_elapsed);

// Randomized multi-start improvement of `r` (seeded): perturbed priorities, list/unlock
// scheduling forward or on the mirror graph, then a few forward-backward rounds. Runs while
// work < work_budget (work units, see improve_schedule), clock.elapsed() < deadline_elapsed and
// r.latency > r.lower_bound; only ever lowers r.latency.
void restart_schedule(OpGraph const &g, HeuristicResult &r, std::int64_t work_budget,
                      std::int64_t &work, std::uint64_t seed, Clock const &clock,
                      double deadline_elapsed);

// Depth-first search for a schedule with latency <= target (see heuristic.cpp). `infeasible`:
// the search finished without one, which proves target infeasible.
struct DfsResult {
    std::vector<std::int32_t> step; // empty: none found
    bool infeasible = false;
    std::int64_t nodes = 0;
};
DfsResult target_dfs(OpGraph const &g, std::vector<std::int32_t> const &tail,
                     std::vector<std::int32_t> const &incumbent, std::int32_t target,
                     std::int64_t node_limit);

// Iterated local search (seeded): perturb the priorities of the current schedule, two
// forward-backward rounds, move to the result if its latency is not worse. Same stopping rules
// as restart_schedule.
void local_search(OpGraph const &g, HeuristicResult &r, std::int64_t work_budget,
                  std::int64_t &work, std::uint64_t seed, Clock const &clock,
                  double deadline_elapsed);

// Deterministic improvement of heuristic_schedule's result within `work_budget` work units
// (about 50 ns each on the development machine): target_dfs, restart_schedule, local_search.
// The result depends only on the input and the budget unless the wall-clock guard
// `deadline_elapsed` fires first (a machine several times slower).
void improve_schedule(OpGraph const &g, HeuristicResult &r, std::int64_t work_budget,
                      Clock const &clock, double deadline_elapsed, std::int64_t &work);

// List and unlock scheduling with the priority `key` (larger first), on g itself or, when
// `on_mirror`, on its mirror graph (key indexed by mirror op), then forward-backward rounds from
// the best result. Only ever lowers r.latency; stops at r.lower_bound or the deadline.
void guided_schedule(OpGraph const &g, HeuristicResult &r, std::vector<std::int64_t> const &key,
                     bool on_mirror, Clock const &clock, double deadline_elapsed);

