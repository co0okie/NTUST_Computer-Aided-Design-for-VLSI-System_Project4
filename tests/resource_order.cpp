// Standalone white-box regression: c++ -std=c++17 -O1 -Isrc
// tests/resource_order.cpp src/bounds.cpp -o <test-binary>
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
