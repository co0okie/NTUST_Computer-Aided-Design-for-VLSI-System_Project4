// Standalone white-box regression: c++ -std=c++17 -O1 -Isrc
// tests/resource_order.cpp src/bounds.cpp src/priority_cem.cpp -o <test-binary>
// Including the implementation deliberately exposes its private move/cycle helpers.
#include "../src/resource_order.cpp"
#include <cassert>
#include <climits>
#include <iostream>

namespace {
OpGraph graph(Id n, std::vector<std::pair<Id, Id>> const &edges,
              std::array<Id, OP_KINDS> limits) {
    OpGraph g;
    g.n = n;
    g.limit = limits;
    g.kind.assign(n, 0);
    g.count[0] = n;
    g.pred.offset.assign(n + 1, 0);
    g.succ.offset.assign(n + 1, 0);
    for (auto const &[u, v] : edges) {
        ++g.succ.offset[u + 1];
        ++g.pred.offset[v + 1];
    }
    for (Id i = 0; i < n; ++i) {
        g.succ.offset[i + 1] += g.succ.offset[i];
        g.pred.offset[i + 1] += g.pred.offset[i];
    }
    g.succ.item.resize(edges.size());
    g.pred.item.resize(edges.size());
    auto so = g.succ.offset, po = g.pred.offset;
    for (auto const &[u, v] : edges) {
        g.succ.item[so[u]++] = v;
        g.pred.item[po[v]++] = u;
    }
    return g;
}

void check_lanes(OpGraph const &g, Lanes const &s) {
    std::vector<bool> seen(g.n, false);
    std::array<int, OP_KINDS> heads{};
    for (Id u = 0; u < g.n; ++u) {
        if (s.prev[u] >= 0) {
            assert(s.next[s.prev[u]] == u);
            assert(s.lane[s.prev[u]] == s.lane[u]);
            assert(g.kind[s.prev[u]] == g.kind[u]);
        } else {
            ++heads[g.kind[u]];
            for (auto v = u; v >= 0; v = s.next[v]) {
                assert(!seen[v]);
                seen[v] = true;
            }
        }
    }
    for (auto visited : seen) assert(visited);
    for (int k = 0; k < OP_KINDS; ++k) assert(heads[k] <= g.limit[k]);
}

void check_steps(OpGraph const &g, Evaluation const &e) {
    std::vector<std::array<Id, OP_KINDS>> usage(e.latency + 1);
    Id latency = 0;
    for (Id u = 0; u < g.n; ++u) {
        assert(e.step[u] >= 1 && e.step[u] <= e.latency);
        latency = std::max(latency, e.step[u]);
        assert(++usage[e.step[u]][g.kind[u]] <= g.limit[g.kind[u]]);
        for (auto const v : g.succ.row(u)) assert(e.step[u] < e.step[v]);
    }
    assert(latency == e.latency);
}
} // namespace

int main() {
    // Reversing a machine arc that duplicates a data arc creates a cycle.
    auto g = graph(2, {{0, 1}}, {1, 0, 0});
    auto s = lanes_from_steps(g, {1, 2});
    Evaluation e;
    assert(evaluate(g, s, e, 2));
    assert(e.latency == 2);
    auto undo = apply(s, {0, 1, false});
    assert(!evaluate(g, s, e, 2));
    restore(s, undo);
    assert(evaluate(g, s, e, 2));
    assert(!evaluate(g, s, e, 1));

    std::mt19937_64 rng(12345);
    std::uint64_t moves = 0, cyclic = 0;
    for (int sample = 0; sample < 120; ++sample) {
        Id const n = 3 + static_cast<Id>(rng() % 13);
        std::vector<std::pair<Id, Id>> edges;
        for (Id u = 0; u < n; ++u)
            for (Id v = u + 1; v < n; ++v)
                if (rng() % 7 == 0) edges.push_back({u, v});
        g = graph(n, edges, {1 + static_cast<Id>(rng() % 4), 3, 2});
        g.count.fill(0);
        for (auto &kind : g.kind) {
            kind = static_cast<std::uint8_t>(rng() % OP_KINDS);
            ++g.count[kind];
        }
        std::vector<Id> seed(n);
        std::iota(seed.begin(), seed.end(), 1);
        s = lanes_from_steps(g, seed, &rng);
        for (Id u = 0; u < n; ++u) {
            for (Id v = 0; v < n; ++v) {
                if (g.kind[u] != g.kind[v]) continue;
                for (bool const before : {false, true}) {
                    Move const move{u, v, before};
                    if (!useful(s, move)) continue;
                    auto const original = s;
                    auto const saved = apply(s, move);
                    check_lanes(g, s);
                    if (evaluate(g, s, e, n)) check_steps(g, e);
                    else ++cyclic;
                    restore(s, saved);
                    assert(s.prev == original.prev && s.next == original.next && s.lane == original.lane);
                    ++moves;
                }
            }
        }
        // Partial machine relaxation: data edges always remain. Every insertion
        // replaces a->b by a->u->b, preserving its transitive implication.
        auto relaxed = s;
        std::vector<Id> removed;
        for (Id u = 0; u < n; ++u) if (rng() % 2 == 0) {
            detach(relaxed, u);
            removed.push_back(u);
        }
        Evaluation lower;
        assert(evaluate(g, relaxed, lower, n));
        auto previous = lower.latency;
        for (auto const u : removed) {
            // Test every same-kind reinsertion, including empty-lane heads.
            auto const k = g.kind[u];
            Id base = 0;
            for (int j = 0; j < k; ++j) base += std::min(g.limit[j], g.count[j]);
            for (Id anchor = -1; anchor < n; ++anchor) {
                if (anchor >= 0 && (relaxed.lane[anchor] < 0 || g.kind[anchor] != k)) continue;
                auto candidate = relaxed;
                insert_after(candidate, u, anchor, anchor < 0 ? base : relaxed.lane[anchor]);
                Evaluation added;
                if (evaluate(g, candidate, added, n)) assert(added.latency >= previous);
            }
            // Reconstruct the original lane order, skipping still-removed nodes.
            auto p = s.prev[u];
            while (p >= 0 && relaxed.lane[p] < 0) p = s.prev[p];
            insert_after(relaxed, u, p, s.lane[u]);
            assert(evaluate(g, relaxed, lower, n));
            assert(lower.latency >= previous);
            previous = lower.latency;
        }
        assert(relaxed.prev == s.prev && relaxed.next == s.next && relaxed.lane == s.lane);
        check_steps(g, lower);
        HeuristicResult best;
        best.step = seed;
        best.latency = n;
        best.lower_bound = 1;
        Clock clock;
        improve_resource_order(g, best, clock, 0.002);
        assert(best.latency <= n);
        e.step = best.step;
        e.latency = best.latency;
        check_steps(g, e);
        auto const unchanged = best.step;
        auto const stats = improve_resource_order(g, best, clock, -1.0);
        assert(stats.attempts == 0 && best.step == unchanged);
    }
    // Known-solvable destroy/rebuild: eight independent operations initially
    // serialized on one of two lanes. The exact optimum is ceil(8/2)=4.
    g = graph(8, {}, {2, 0, 0});
    HeuristicResult repair;
    repair.step = {1, 2, 3, 4, 5, 6, 7, 8};
    repair.latency = 8;
    repair.lower_bound = 4;
    Clock repair_clock;
    setenv("MLRCS_ORDER_METHOD", "beam", 1);
    auto const repair_stats = improve_resource_order(g, repair, repair_clock, 1.0);
    unsetenv("MLRCS_ORDER_METHOD");
    assert(repair.latency == 4 && repair_stats.accepted > 0);
    e.step = repair.step;
    e.latency = repair.latency;
    check_steps(g, e);
    // Annealing acceptance really admits positive energy and rejects some too.
    assert(metropolis_accept(-1.0, 0.4, 0.99));
    assert(metropolis_accept(0.0, 0.4, 0.99));
    assert(metropolis_accept(1.0, 0.4, 0.05));
    assert(!metropolis_accept(1.0, 0.4, 0.10));
    g = graph(12, {}, {2, 0, 0});
    repair.step = {1, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 8};
    repair.latency = 8;
    repair.lower_bound = 6;
    Clock anneal_clock;
    setenv("MLRCS_ORDER_METHOD", "anneal", 1);
    auto const anneal_stats = improve_resource_order(g, repair, anneal_clock, 0.5);
    unsetenv("MLRCS_ORDER_METHOD");
    assert(repair.latency == 6 && anneal_stats.accepted > 0);
    e.step = repair.step;
    e.latency = repair.latency;
    check_steps(g, e);
    // Oversized resource capacities must allocate by operation count, not capacity.
    g = graph(3, {}, {INT_MAX, INT_MAX, INT_MAX});
    s = lanes_from_steps(g, {1, 1, 1});
    assert(evaluate(g, s, e, 1));
    check_steps(g, e);
    // Closure preprocessing must decline excessive work even if its memory fits.
    g = graph(15000, {}, {1, 1, 1});
    assert(descendants(g).empty());
    std::cout << "resource-order OK moves=" << moves << " cyclic=" << cyclic << '\n';
}
