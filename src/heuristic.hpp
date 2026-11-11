#pragma once

#include <cstdint>
#include <vector>

#include "clock.hpp"
#include "opgraph.hpp"

// List scheduling: at each step, each kind runs its ready ops with the largest keys.
// Returns the step (>= 1) of every op.
std::vector<std::int32_t> list_schedule(OpGraph const &g, std::vector<std::int64_t> const &key);

struct HeuristicResult {
    std::vector<std::int32_t> step; // per op index
    std::int32_t latency = 0;
    std::int32_t lower_bound = 0;
    std::vector<std::int32_t> head, tail; // resource-aware bounds, reused by the ILP
    std::int32_t first_latency = 0;       // latency of the first (plain tail-priority) pass
    std::int32_t passes = 0;
};

HeuristicResult heuristic_schedule(OpGraph const &g, Clock const &clock, double time_budget);
