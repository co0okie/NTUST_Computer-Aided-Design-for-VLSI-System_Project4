#pragma once

#include <cstdint>
#include <vector>

#include "opgraph.hpp"

// Transitive closure: bit j of row i is set iff op j is a descendant of op i. Empty (words == 0)
// when it would need more than `max_bytes`.
struct Closure {
    std::int64_t words = 0; // 64-bit words per row
    std::vector<std::uint64_t> bits;

    bool empty() const { return words == 0; }
    std::uint64_t const *row(std::int32_t i) const { return bits.data() + i * words; }
};

Closure descendants(OpGraph const &g, std::int64_t max_bytes = 400'000'000);

// Resource-aware tails. tail[i] is a lower bound on the number of steps that must follow op i:
// every feasible schedule has latency >= step[i] + tail[i].
//
// For op i, let D be its descendants. For each kind k and each threshold q, the N descendants of
// kind k with tail >= q all run after i, at most limit[k] per step, and q more steps follow the
// last of them, so tail[i] >= ceil(N / limit[k]) + q. (With N = 1 this is the usual height.)
//
// With an empty closure it falls back to plain heights.
std::vector<std::int32_t> resource_tails(OpGraph const &g, Closure const &closure);

// Idle slots of kind b's units that every schedule has in its first `steps` steps.
//
// Let S be the set of b ops finished by step t. Every non-b ancestor of S runs by step t - 1, so
// for each kind k != b the set U of non-b ancestors of S has at most limit[k] * (t - 1) ops of
// kind k; conversely S can only contain b ops whose non-b ancestors are all in U. Hence
// |S| <= M(t) = max over such U (any union of the b ops' non-b ancestor sets within the
// budgets) of the number of b ops whose non-b ancestors lie in U, found by exhaustive search
// over the unions (iterative deepening on t; stops when `max_nodes` would be exceeded). |S| is
// also at most the number the units can run given the release steps `head`. The b units then
// idle at least limit[b] * t - min(those) slots by step t; the result is the best t.
struct StartIdle {
    std::int64_t slots = 0;
    std::int32_t steps = 0;
};
StartIdle start_idle_bound(OpGraph const &g, Closure const &ancestors, std::vector<std::int32_t> const &head,
                           int b, std::int32_t max_steps, std::int64_t max_nodes);

// Lower bound on the latency: for each kind on its own, the optimal schedule of its ops with
// release steps `head` and delivery times `tail` on limit[k] machines, ignoring precedence
// (largest tail first is optimal for unit jobs).
std::int32_t jackson_bound(OpGraph const &g, std::vector<std::int32_t> const &head,
                           std::vector<std::int32_t> const &tail);
