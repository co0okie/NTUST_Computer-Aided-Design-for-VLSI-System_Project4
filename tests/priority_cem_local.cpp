// c++ -std=c++17 -O1 -Isrc tests/priority_cem_local.cpp -o <binary>
#include "../src/priority_cem.cpp"
#include <cassert>
#include <iostream>
int main() {
    auto const original = noise_parameters(false), local = noise_parameters(true);
    assert(original.initial == .25 && original.minimum == .10 && original.seed == .4);
    assert(local.initial == .025 && local.minimum == .01 && local.seed == .04);
    OpGraph g;
    g.n = 12; g.limit = {2, 1, 1}; g.count = {12, 0, 0};
    g.kind.assign(12, 0); g.pred.offset.assign(13, 0); g.succ.offset.assign(13, 0);
    HeuristicResult a;
    a.step = {1, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 8}; a.latency = 8;
    auto b = a;
    Clock clock;
    auto const x = improve_priority_cem(g, a, clock, 10, true);
    auto const y = improve_priority_cem(g, b, clock, 10, true);
    assert(a.latency == 6 && a.step == b.step && x.attempts == 2049 && y.attempts == 2049);
    assert(x.accepted == 1 && y.accepted == 1);
    std::cout << "priority-cem-local OK parameters original-preserved local-toy deterministic-work\n";
}
