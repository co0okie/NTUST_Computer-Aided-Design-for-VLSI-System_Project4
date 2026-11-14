#pragma once

#include <cstdint>
#include "clock.hpp"
#include "opgraph.hpp"
#include "seed_heuristic.hpp"

struct OrderSearchStats {
    std::uint64_t attempts = 0, feasible = 0, accepted = 0;
    std::int32_t initial_latency = 0, final_latency = 0;
};

// Improve a valid incumbent by changing orders on identical resource lanes. Every accepted
// orientation is checked for cycles; the returned incumbent never has greater latency.
OrderSearchStats improve_resource_order(OpGraph const &g, HeuristicResult &best,
                                         Clock const &clock, double deadline_elapsed);
