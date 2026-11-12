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

HeuristicResult heuristic_schedule(OpGraph const &g, Clock const &clock, double time_budget);

// Randomized multi-start improvement of `r` (fixed seed): perturbed priorities, list/unlock
// scheduling forward or on the mirror graph, then a few forward-backward rounds. Runs while
// clock.elapsed() < deadline_elapsed and r.latency > r.lower_bound; only ever lowers r.latency.
void improve_schedule(OpGraph const &g, HeuristicResult &r, Clock const &clock,
                      double deadline_elapsed);
