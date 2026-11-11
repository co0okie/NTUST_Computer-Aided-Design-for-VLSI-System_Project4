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

// Lower bound on the latency: for each kind on its own, the optimal schedule of its ops with
// release steps `head` and delivery times `tail` on limit[k] machines, ignoring precedence
// (largest tail first is optimal for unit jobs).
std::int32_t jackson_bound(OpGraph const &g, std::vector<std::int32_t> const &head,
                           std::vector<std::int32_t> const &tail);
