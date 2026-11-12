#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "clock.hpp"
#include "heuristic.hpp"
#include "opgraph.hpp"
#include "result.hpp"

struct IlpOptions {
    std::string gurobi_cl;   // from find_gurobi_cl()
    std::string keep_dir;    // keep the LP/solution/log files here; empty: temporary, deleted
    int threads = 0;         // Gurobi Threads parameter; 0 = Gurobi's default
    bool use_ends_bound = false; // experimental prefix/suffix relaxation when the model is too large
    std::int64_t max_vars = 400'000; // do not build a model larger than this
};

// How the reported schedule was obtained
enum class ExactStatus {
    Optimal,    // proven optimal by the ILP (found or matched a bound)
    Bound,      // the heuristic already meets the combinatorial lower bound; no ILP needed
    TimeLimit,  // ran out of time; best schedule found so far
    TooLarge,   // model above max_vars; best schedule found so far
};

char const *exact_status_name(ExactStatus s);

struct ExactResult {
    std::vector<std::int32_t> step; // per op index
    std::int32_t latency = 0;
    ExactStatus status = ExactStatus::Optimal;
    std::int32_t lower_bound = 0;
    std::int64_t num_vars = 0; // variables of the last ILP built
    std::int32_t solves = 0;   // Gurobi runs
    std::string note;          // solver trouble that was survived, for --stats
};

// Starts from the heuristic schedule (latency U) and asks Gurobi whether a schedule with horizon
// U - 1 exists, repeating until the answer is no (U is optimal) or U meets the lower bound.
// Never fails: on any solver trouble the best schedule found so far is returned.
ExactResult exact_schedule(OpGraph const &g, HeuristicResult const &heuristic,
                           IlpOptions const &options, Clock const &clock);
