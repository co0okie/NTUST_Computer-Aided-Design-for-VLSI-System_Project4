#pragma once

#include <cstdint>
#include <string>

#include "clock.hpp"
#include "netlist.hpp"
#include "result.hpp"
#include "schedule.hpp"

struct IlpOptions {
    std::string gurobi_cl;   // from find_gurobi_cl()
    std::string keep_dir;    // keep the LP/solution/log files here; empty: temporary, deleted
    int threads = 0;         // Gurobi Threads parameter; 0 = Gurobi's default
    std::int64_t max_vars = 3'000'000; // skip the ILP if the model would be larger
};

// How the reported schedule was obtained
enum class ExactStatus {
    Optimal,    // Gurobi proved optimality
    Bound,      // the heuristic already meets a lower bound; no ILP needed
    TimeLimit,  // Gurobi ran out of time; best schedule found so far
    TooLarge,   // model above max_vars; heuristic schedule returned unchanged
};

char const *exact_status_name(ExactStatus s);

struct ExactResult {
    Schedule schedule;
    ExactStatus status = ExactStatus::Optimal;
    std::int32_t lower_bound = 0;
    std::int64_t num_vars = 0; // binary variables in the ILP (0 if not built)
};

// Baseline time-indexed ILP. The heuristic schedule supplies the latency upper bound (which
// limits every operation's [ASAP, ALAP] window) and a MIP start.
Result<ExactResult> exact_schedule(Netlist const &n, Resources const &r, Schedule const &heuristic,
                                   std::int32_t known_bound, IlpOptions const &options, Clock const &clock);
