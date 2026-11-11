#include "heuristic.hpp"

#include <algorithm>
#include <queue>
#include <utility>

namespace {

// Height of each node: 1 + max height over fanouts (an operation with no fanout has height 1).
std::vector<std::int32_t> heights(Netlist const &n) {
    std::vector<std::int32_t> h(n.size(), 0);
    for (auto it = n.topo.rbegin(); it != n.topo.rend(); ++it) {
        h[*it] = 1;
        for (auto const j : n.fanout.row(*it)) h[*it] = std::max(h[*it], h[j] + 1);
    }
    return h;
}

} // namespace

Result<Schedule> list_schedule(Netlist const &n, Resources const &r) {
    TRY_VOID(check_resources(n, r));
    auto const height = heights(n);

    // Max-heap on (height, -id): larger height first, then smaller id
    using Entry = std::pair<std::int32_t, NodeId>;
    auto const lower = [](Entry const &a, Entry const &b) {
        return a.first != b.first ? a.first < b.first : a.second > b.second;
    };
    using Heap = std::priority_queue<Entry, std::vector<Entry>, decltype(lower)>;
    std::array<Heap, OP_KINDS> ready = {Heap(lower), Heap(lower), Heap(lower)};

    // Unscheduled operation fanins of each node; PIs are available from the start
    std::vector<std::int32_t> waiting(n.size(), 0);
    std::vector<NodeId> next; // ops that become ready at the next step
    for (auto const i : n.topo) {
        for (auto const j : n.fanin.row(i))
            if (n.is_op(j)) ++waiting[i];
        if (waiting[i] == 0) next.push_back(i);
    }

    Schedule s;
    s.step.assign(n.size(), 0);
    NodeId remaining = n.num_ops();
    std::vector<NodeId> done;
    for (std::int32_t t = 1; remaining > 0; ++t) {
        for (auto const i : next) ready[op_index(n.kind[i])].push({height[i], i});
        next.clear();
        done.clear();
        for (int k = 0; k < OP_KINDS; ++k) {
            for (std::int32_t used = 0; used < r.limit[k] && !ready[k].empty(); ++used) {
                auto const i = ready[k].top().second;
                ready[k].pop();
                s.step[i] = t;
                done.push_back(i);
            }
        }
        for (auto const i : done)
            for (auto const j : n.fanout.row(i))
                if (--waiting[j] == 0) next.push_back(j);
        remaining -= static_cast<NodeId>(done.size());
        s.latency = t;
    }
    return s;
}
