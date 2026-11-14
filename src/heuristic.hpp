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

// Planned start ("ramp") for the bottleneck kind b. At the start the units of the other kinds
// decide how soon b has work: a b op can run only after all its non-b ancestors. The plan is a
// sequence of bundles, each the missing non-b ancestors of one b op; a bundle is chosen when a
// unit of another kind would otherwise run an op outside the plan, by the ratio (b ops it
// completes the non-b ancestors of) / (unit-steps it costs), randomly perturbed.
//
// RampData: the non-b ancestor sets of the b ops, grouped (identical sets share a group), for
// the ops that can matter during the first `steps` steps. Empty (no groups) without a closure.
struct RampData {
    int bottleneck = 0;
    std::int32_t steps = 0;
    std::vector<std::int32_t> group_count;  // b ops per group
    Csr group_items;                        // group -> its ops (non-b), sorted
    Csr containing;                         // non-b op -> groups containing it
};
// `ancestors`: the closure of the mirror graph (the descendants of mirror op n-1-i are the
// ancestors of op i).
RampData make_ramp_data(OpGraph const &g, Closure const &ancestors, int bottleneck,
                        std::int32_t steps, std::int32_t horizon);

struct RampOptions {
    std::int32_t horizon = 32;    // largest bundle considered (ops)
    std::int32_t candidates = 64; // bundles considered per choice (the smallest)
    double noise = 1;             // ratios are multiplied by 1 + noise * U[0, 1)
    std::uint64_t seed = 1;
};
// List scheduling with priority `key` (larger first), except that during the first data.steps
// steps the planned non-b ops run first (earlier bundles first). `work` grows by the elementary
// operations of the bundle choices.
std::vector<std::int32_t> ramp_schedule(OpGraph const &g, RampData const &data,
                                        std::vector<std::int64_t> const &key,
                                        RampOptions const &options, std::int64_t &work);

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

// Search for a schedule with latency <= target (see heuristic.cpp): depth-first, step by step,
// with time-window propagation; restarted with perturbed branching orders. `infeasible`: the
// first (unperturbed) search finished without a solution, which proves target infeasible.
struct DfsOptions {
    std::int64_t cost_limit = 0;      // elementary operations in all
    double first_share = 0.3;         // of cost_limit, for the first search
    std::int64_t restart_nodes = 1000; // search nodes per later restart
    std::uint64_t seed = 1;
};
struct DfsResult {
    std::vector<std::int32_t> step; // empty: none found
    bool infeasible = false;
    std::int64_t cost = 0;
    std::int64_t restarts = 0;
};
// Failed search states, shared by the target_dfs calls on one graph: after steps 1..t are
// decided, what remains depends only on t and the set S of decided ops (all finished by t + 1),
// so a subtree searched completely without a solution proves (t', S) hopeless for every
// t' >= t. S is keyed by two 64-bit Zobrist hashes (a false match has probability ~2^-128 per
// lookup). Entries are valid for one target only; target_dfs clears them when it changes.
struct DfsMemo {
    struct Entry {
        std::uint64_t a = 0, b = 0;
        std::int32_t t = 0; // 0: empty slot
    };
    std::int32_t target = 0;
    std::vector<std::uint64_t> zob_a, zob_b;
    std::vector<Entry> table; // open addressing, size a power of two (empty: no memo)
    std::size_t count = 0;
};
// A table of at least `states` slots (power of two, at most 2^21)
DfsMemo make_dfs_memo(std::int64_t states);

DfsResult target_dfs(OpGraph const &g, std::vector<std::int32_t> const &tail,
                     std::vector<std::int32_t> const &incumbent, std::int32_t target,
                     DfsOptions const &options, DfsMemo &memo);

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

