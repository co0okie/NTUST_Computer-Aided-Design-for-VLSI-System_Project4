#pragma once

#include <cstdint>
#include <vector>

#include "opgraph.hpp"

// Resource-aware tails. tail[i] is a lower bound on the number of steps that must follow op i:
// every feasible schedule has latency >= step[i] + tail[i].
//
// For op i, let D be its descendants. For each kind k and each threshold q, the N descendants of
// kind k with tail >= q all run after i, at most limit[k] per step, and q more steps follow the
// last of them, so tail[i] >= ceil(N / limit[k]) + q. (With N = 1 this is the usual height.)
//
// Needs the transitive closure (n^2 / 8 bytes); above `max_closure_bytes` it falls back to
// plain heights.
std::vector<std::int32_t> resource_tails(OpGraph const &g,
                                         std::int64_t max_closure_bytes = 600'000'000);

// Heads from the mirror problem: head[i] is the earliest possible step of op i (>= 1).
std::vector<std::int32_t> resource_heads(OpGraph const &g,
                                         std::int64_t max_closure_bytes = 600'000'000);

// Lower bound on the latency: for each kind on its own, the optimal schedule of its ops with
// release steps `head` and delivery times `tail` on limit[k] machines, ignoring precedence
// (largest tail first is optimal for unit jobs).
std::int32_t jackson_bound(OpGraph const &g, std::vector<std::int32_t> const &head,
                           std::vector<std::int32_t> const &tail);
