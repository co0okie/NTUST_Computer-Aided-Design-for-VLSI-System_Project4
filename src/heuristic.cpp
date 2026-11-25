#include "heuristic.hpp"
#include "seed_heuristic.hpp"
#include "resource_order.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>

#include <algorithm>
#include <queue>
#include <utility>

namespace {

// Optional research sidecar; no file is opened unless explicitly requested. Each
// headerless TSV row contains model, capacities, seed/final latency, counters,
// enabled flag and skip/search reason. Logging failure cannot discard a schedule.
void log_order_search(Netlist const &n, Resources const &r, OrderSearchStats const &stats,
                      bool enabled, char const *reason) {
    auto const *path = std::getenv("MLRCS_ORDER_LOG");
    if (!path || !*path) return;
    std::ofstream out(path, std::ios::app);
    out << n.model << '\t' << r.limit[0] << '\t' << r.limit[1] << '\t' << r.limit[2]
        << '\t' << stats.initial_latency << '\t' << stats.final_latency
        << '\t' << stats.attempts << '\t' << stats.feasible << '\t' << stats.accepted
        << '\t' << (enabled ? 1 : 0) << '\t' << reason << '\n';
}

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

Result<Schedule> fast_schedule(Netlist const &n, Resources const &r) {
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

Result<Schedule> list_schedule(Netlist const &n, Resources const &r, Clock const &clock,
                               ScheduleProgress const &progress) {
    TRY(auto baseline, fast_schedule(n, r));
    auto const *toggle = std::getenv("MLRCS_ORDER_SEARCH");
    bool const enabled = !toggle || std::string(toggle) != "0";
    if (n.num_ops() == 0 || clock.remaining() <= 0.25) {
        OrderSearchStats stats;
        stats.initial_latency = stats.final_latency = baseline.latency;
        log_order_search(n, r, stats, enabled, n.num_ops() == 0 ? "empty" : "budget");
        return baseline;
    }
    auto const graph = make_op_graph(n, r);
    auto const reserve = std::min(5.0, clock.budget * 0.2);
    auto const seed_deadline = std::min(clock.elapsed() + 5.0, clock.budget - reserve);
    auto seed = heuristic_schedule(graph, clock, seed_deadline);
    if (baseline.latency < seed.latency) {
        seed.latency = baseline.latency;
        for (std::int32_t i = 0; i < graph.n; ++i) seed.step[i] = baseline.step[graph.node[i]];
    }
    auto const publish = [&]() -> Result<Unit> {
        if (!progress) return Unit{};
        auto const candidate = to_schedule(n, graph, seed.step);
        TRY_VOID(validate(n, r, candidate));
        return progress(candidate);
    };
    TRY_VOID(publish());
    auto const restart_deadline = std::min(10.0, clock.budget - reserve);
    improve_schedule(graph, seed, clock, restart_deadline);
    TRY_VOID(publish());
    OrderSearchStats stats;
    stats.initial_latency = stats.final_latency = seed.latency;
    char const *reason = "disabled";
    if (enabled) {
        auto const deadline = std::min(clock.elapsed() + 8.0, clock.budget - reserve);
        reason = seed.latency <= seed.lower_bound ? "bound" :
                 clock.elapsed() >= deadline ? "budget" : "search";
        stats = improve_resource_order(graph, seed, clock, deadline);
        if (std::getenv("MLRCS_ORDER_DIAGNOSTICS"))
            std::cerr << "order-search initial=" << stats.initial_latency
                      << " final=" << stats.final_latency << " attempts=" << stats.attempts
                      << " feasible=" << stats.feasible << " accepted=" << stats.accepted << '\n';
    }
    TRY_VOID(publish());
    auto result = to_schedule(n, graph, seed.step);
    TRY_VOID(validate(n, r, result));
    log_order_search(n, r, stats, enabled, reason);
    return result;
}

Result<Schedule> list_schedule(Netlist const &n, Resources const &r) {
    Clock clock;
    return list_schedule(n, r, clock);
}
