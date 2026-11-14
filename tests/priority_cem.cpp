// c++ -std=c++17 -O1 -Isrc tests/priority_cem.cpp -o <binary>
#include "../src/priority_cem.cpp"
#include <cassert>
#include <climits>
#include <iostream>

namespace {
OpGraph graph(Id n, std::vector<std::pair<Id, Id>> const &edges) {
    OpGraph g;
    g.n = n;
    g.kind.assign(n, 0);
    g.count[0] = n;
    g.limit = {1, 1, 1};
    g.pred.offset.assign(n + 1, 0); g.succ.offset.assign(n + 1, 0);
    for (auto const &[u, v] : edges) { ++g.succ.offset[u + 1]; ++g.pred.offset[v + 1]; }
    for (Id u = 0; u < n; ++u) { g.succ.offset[u + 1] += g.succ.offset[u]; g.pred.offset[u + 1] += g.pred.offset[u]; }
    g.succ.item.resize(edges.size()); g.pred.item.resize(edges.size());
    auto so = g.succ.offset, po = g.pred.offset;
    for (auto const &[u, v] : edges) { g.succ.item[so[u]++] = v; g.pred.item[po[v]++] = u; }
    return g;
}
void legal(OpGraph const &g, SerialCandidate const &c) {
    std::vector<std::array<Id, 3>> usage(c.latency + 1);
    Id latency = 0;
    for (Id u = 0; u < g.n; ++u) {
        assert(c.step[u] >= 1 && c.step[u] <= c.latency);
        assert(++usage[c.step[u]][g.kind[u]] <= g.limit[g.kind[u]]);
        latency = std::max(latency, c.step[u]);
        for (auto const v : g.succ.row(u)) assert(c.step[u] < c.step[v]);
    }
    assert(latency == c.latency);
    // For unit jobs the serial decoder cannot leave avoidable active idle.
    for (Id t = 1; t <= latency; ++t) for (Id u = 0; u < g.n; ++u) {
        if (c.step[u] <= t || usage[t][g.kind[u]] == g.limit[g.kind[u]]) continue;
        bool ready = true;
        for (auto const p : g.pred.row(u)) if (c.step[p] >= t) ready = false;
        assert(!ready);
    }
}
}
int main() {
    Clock clock; std::uint64_t work = 0;
    SerialCandidate candidate, repacked;
    auto g = graph(3, {{0, 2}});
    g.kind[0] = 1; g.count = {2, 1, 0};
    assert(serial_decode(g, {0.0, 0.9, 0.1}, 3, clock, 30, candidate, work));
    assert(candidate.rank[2] == 1 && candidate.step[2] == 2);
    assert(candidate.rank[1] == 2 && candidate.step[1] == 1); // fill reserved-future hole
    legal(g, candidate);
    // A deliberately idling seed is legal; earliest insertion repairs it.
    g = graph(2, {});
    assert(serial_decode(g, step_priorities({2, 3}), 3, clock, 30, candidate, work));
    assert(candidate.step == std::vector<Id>({1, 2})); legal(g, candidate);
    assert(!serial_decode(g, {0, 1}, 1, clock, 30, candidate, work));
    assert(!serial_decode(g, {0, 1}, 2, clock, -1, candidate, work));
    g.limit = {INT_MAX, INT_MAX, INT_MAX};
    assert(serial_decode(g, {0, 1}, 2, clock, 30, candidate, work));
    assert(candidate.latency == 1); legal(g, candidate);
    std::mt19937_64 rng(17531);
    for (int test = 0; test < 120; ++test) {
        auto const n = 3 + static_cast<Id>(rng() % 40);
        std::vector<std::pair<Id, Id>> edges;
        for (Id u = 0; u < n; ++u) for (Id v = u + 1; v < n; ++v)
            if (rng() % 7 == 0) edges.push_back({u, v});
        g = graph(n, edges); g.count = {0, 0, 0};
        for (Id u = 0; u < n; ++u) { g.kind[u] = rng() % 3; ++g.count[g.kind[u]]; }
        for (int k = 0; k < 3; ++k) g.limit[k] = 1 + rng() % 3;
        std::vector<double> key(n);
        for (auto &value : key) value = static_cast<double>(rng() % 10000) / 10000;
        assert(serial_decode(g, key, n, clock, 30, candidate, work)); legal(g, candidate);
        assert(serial_decode(g, step_priorities(candidate.step), n, clock, 30, repacked, work));
        legal(g, repacked);
        assert(repacked.latency <= candidate.latency);
        for (Id u = 0; u < n; ++u) assert(repacked.step[u] <= candidate.step[u]);
    }
    g = graph(12, {}); g.limit[0] = 2;
    HeuristicResult result;
    result.step = {1, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 8};
    result.latency = 8; result.lower_bound = 6;
    auto const improved = improve_priority_cem(g, result, clock, 30);
    assert(result.latency == 6 && improved.accepted == 1);
    result.lower_bound = 0;
    auto copy = result;
    auto const a = improve_priority_cem(g, result, clock, 30);
    auto const b = improve_priority_cem(g, copy, clock, 30);
    assert(result.step == copy.step && a.attempts == b.attempts && a.attempts == 2049);
    g.n = 5001;
    assert(improve_priority_cem(g, result, clock, 30).attempts == 0);
    std::cout << "priority-cem OK random=120 seed-repack future-hole idle-repair deterministic guard\n";
}
