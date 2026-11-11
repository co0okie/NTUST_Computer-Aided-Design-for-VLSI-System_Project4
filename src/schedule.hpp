#pragma once

#include <array>
#include <cstdint>
#include <ostream>
#include <vector>

#include "netlist.hpp"
#include "result.hpp"

// Resource constraint: number of units of each operation kind (AND, OR, NOT) per step.
struct Resources {
    std::array<std::int32_t, OP_KINDS> limit{};
};

// A schedule assigns every operation a step 1..latency; primary inputs are available at step 0.
// Every operation takes exactly one step, so an operation can start one step after its last
// fanin.
struct Schedule {
    std::vector<std::int32_t> step; // per node; 0 for primary inputs
    std::int32_t latency = 0;       // max step over all operations
};

// Checks precedence, resource limits and latency. Every schedule must pass this before printing.
Result<Unit> validate(Netlist const &n, Resources const &r, Schedule const &s);

// Rejects constraints that make the problem infeasible (a used kind with zero units).
Result<Unit> check_resources(Netlist const &n, Resources const &r);

// Prints the schedule in the assignment's format:
//   <title>
//   1: {AND ops} {OR ops} {NOT ops}
//   ...
//   LATENCY: <latency>
//   END
void print_schedule(std::ostream &out, Netlist const &n, Schedule const &s, char const *title);

// --- Unconstrained bounds -------------------------------------------------------------------

// Earliest step of each node with unlimited resources (PIs 0, ops >= 1).
std::vector<std::int32_t> asap(Netlist const &n);

// Latest step of each node such that every operation still finishes by `latency`
// (unlimited resources). Nodes with no path to a sink get `latency`.
std::vector<std::int32_t> alap(Netlist const &n, std::int32_t latency);

// Lower bound from critical path, resource counts, and per-kind unit-job
// feasibility in precedence/resource-work windows (precedence relaxed).
// O(m + n log n) EDF plus an optional memory/work-capped bitset closure pass.
std::int32_t lower_bound(Netlist const &n, Resources const &r);
