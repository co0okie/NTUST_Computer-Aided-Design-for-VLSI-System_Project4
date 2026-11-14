#include "resource_order.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <numeric>
#include <random>
#include <vector>

namespace {
using Id = std::int32_t;

// Each resource lane is a linked list. Its consecutive arcs augment the original DAG.
// There are at most limit[k] lists of kind k, so every acyclic orientation's earliest
// schedule satisfies the resource constraint without a separate capacity approximation.
struct Lanes {
    std::vector<Id> prev, next, lane;
};

Lanes lanes_from_steps(OpGraph const &g, std::vector<Id> const &step,
                       std::mt19937_64 *rng = nullptr) {
    Lanes s;
    s.prev.assign(g.n, -1);
    s.next.assign(g.n, -1);
    s.lane.resize(g.n);
    std::array<std::vector<Id>, OP_KINDS> tail;
    std::array<Id, OP_KINDS> base{};
    Id total = 0;
    for (int k = 0; k < OP_KINDS; ++k) {
        base[k] = total;
        auto const count = std::min(g.limit[k], g.count[k]);
        tail[k].assign(count, -1);
        total += count;
    }
    std::vector<Id> order(g.n);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](Id a, Id b) {
        return step[a] != step[b] ? step[a] < step[b] : a < b;
    });
    std::array<Id, OP_KINDS> used{};
    Id last_step = -1;
    std::array<std::vector<Id>, OP_KINDS> permutation;
    for (int k = 0; k < OP_KINDS; ++k) {
        permutation[k].resize(tail[k].size());
        std::iota(permutation[k].begin(), permutation[k].end(), 0);
    }
    for (auto const u : order) {
        if (step[u] != last_step) {
            used.fill(0);
            last_step = step[u];
            if (rng)
                for (auto &p : permutation) std::shuffle(p.begin(), p.end(), *rng);
        }
        auto const k = g.kind[u];
        auto const l = permutation[k][used[k]++];
        s.lane[u] = base[k] + l;
        s.prev[u] = tail[k][l];
        if (s.prev[u] >= 0) s.next[s.prev[u]] = u;
        tail[k][l] = u;
    }
    return s;
}

// A relocation is u immediately before/after v, possibly on another identical lane.
struct Move { Id u = -1, v = -1; bool before = false; };
struct Undo {
    std::array<Id, 6> node{}, prev{}, next{}, lane{};
    int count = 0;
};

Undo apply(Lanes &s, Move const &m) {
    Undo undo;
    auto const save = [&](Id u) {
        if (u < 0) return;
        for (int i = 0; i < undo.count; ++i) if (undo.node[i] == u) return;
        auto const i = undo.count++;
        undo.node[i] = u;
        undo.prev[i] = s.prev[u];
        undo.next[i] = s.next[u];
        undo.lane[i] = s.lane[u];
    };
    save(m.u); save(s.prev[m.u]); save(s.next[m.u]);
    save(m.v); save(m.before ? s.prev[m.v] : s.next[m.v]);
    auto const p = s.prev[m.u], q = s.next[m.u];
    if (p >= 0) s.next[p] = q;
    if (q >= 0) s.prev[q] = p;
    auto const a = m.before ? s.prev[m.v] : m.v;
    auto const b = m.before ? m.v : s.next[m.v];
    s.prev[m.u] = a;
    s.next[m.u] = b;
    s.lane[m.u] = s.lane[m.v];
    if (a >= 0) s.next[a] = m.u;
    if (b >= 0) s.prev[b] = m.u;
    return undo;
}

void restore(Lanes &s, Undo const &undo) {
    for (int i = 0; i < undo.count; ++i) {
        auto const u = undo.node[i];
        s.prev[u] = undo.prev[i]; s.next[u] = undo.next[i]; s.lane[u] = undo.lane[i];
    }
}

struct Evaluation {
    std::vector<Id> step, tail, degree, topo;
    Id latency = 0;
    std::int64_t pressure = 0;
};

// O(n+m) time and O(n) scratch. Parallel copies of a data edge and a machine edge
// intentionally count twice in indegree and are also decremented twice.
bool evaluate(OpGraph const &g, Lanes const &s, Evaluation &e, Id cutoff) {
    e.step.assign(g.n, 1);
    e.degree.resize(g.n);
    e.topo.clear();
    for (Id i = 0; i < g.n; ++i) {
        e.degree[i] = static_cast<Id>(g.pred.row(i).size()) + (s.prev[i] >= 0);
        if (e.degree[i] == 0) e.topo.push_back(i);
    }
    e.latency = 0;
    for (std::size_t pos = 0; pos < e.topo.size(); ++pos) {
        auto const u = e.topo[pos];
        auto const t = e.step[u];
        e.latency = std::max(e.latency, t);
        if (e.latency > cutoff) return false;
        auto const visit = [&](Id v) {
            e.step[v] = std::max(e.step[v], t + 1);
            if (--e.degree[v] == 0) e.topo.push_back(v);
        };
        for (auto const v : g.succ.row(u)) visit(v);
        if (s.next[u] >= 0) visit(s.next[u]);
    }
    if (static_cast<Id>(e.topo.size()) != g.n) return false;
    e.tail.assign(g.n, 1);
    for (auto it = e.topo.rbegin(); it != e.topo.rend(); ++it) {
        auto const u = *it;
        for (auto const v : g.succ.row(u)) e.tail[u] = std::max(e.tail[u], e.tail[v] + 1);
        if (s.next[u] >= 0) e.tail[u] = std::max(e.tail[u], e.tail[s.next[u]] + 1);
    }
    // Smooth critical-path pressure distinguishes equal-makespan orientations.
    // Fewer almost-critical operations leaves more room for subsequent moves.
    e.pressure = 0;
    for (Id u = 0; u < g.n; ++u) {
        auto const slack = e.latency + 1 - e.step[u] - e.tail[u];
        e.pressure += 4096 / (1 + slack);
    }
    return true;
}

bool useful(Lanes const &s, Move const &m) {
    return m.u != m.v && (m.before ? s.next[m.u] != m.v : s.prev[m.u] != m.v);
}
} // namespace

OrderSearchStats improve_resource_order(OpGraph const &g, HeuristicResult &best,
                                         Clock const &clock, double deadline_elapsed) {
    OrderSearchStats stats;
    stats.initial_latency = stats.final_latency = best.latency;
    if (g.n < 2 || best.latency <= best.lower_bound || clock.elapsed() >= deadline_elapsed)
        return stats;
    std::mt19937_64 rng(0xd1b54a32d192ed03ULL);
    auto state = lanes_from_steps(g, best.step);
    Evaluation current, scratch;
    if (!evaluate(g, state, current, best.latency)) return stats;
    if (current.latency < best.latency) {
        best.latency = current.latency;
        best.step = current.step;
    }
    std::array<std::vector<Id>, OP_KINDS> by_kind;
    for (Id u = 0; u < g.n; ++u) by_kind[g.kind[u]].push_back(u);
    std::vector<Id> critical;
    std::vector<int> tabu(g.n, 0);
    std::uint64_t work = 0;
    // Bounds worst-case exploration independently of processor speed. Each evaluation
    // scans at most n+m edges; total working storage remains linear.
    constexpr std::uint64_t work_limit = 350'000'000;
    auto const cost = static_cast<std::uint64_t>(g.n) + g.succ.item.size();
    int stale = 0, iteration = 0;
    while (best.latency > best.lower_bound && clock.elapsed() < deadline_elapsed && work < work_limit) {
        ++iteration;
        critical.clear();
        for (Id u = 0; u < g.n; ++u)
            if (current.step[u] + current.tail[u] >= current.latency)
                critical.push_back(u); // slack 0 or 1
        std::shuffle(critical.begin(), critical.end(), rng);
        Move selected;
        Id selected_latency = std::numeric_limits<Id>::max();
        std::int64_t selected_pressure = std::numeric_limits<std::int64_t>::max();
        unsigned ties = 0;
        auto const consider = [&](Move m) {
            if (!useful(state, m) || clock.elapsed() >= deadline_elapsed || work >= work_limit)
                return;
            auto const undo = apply(state, m);
            ++stats.attempts;
            work += cost;
            auto const feasible = evaluate(g, state, scratch, best.latency + 1);
            restore(state, undo);
            if (!feasible) return;
            ++stats.feasible;
            // Moving a recently changed node is forbidden unless it improves the best.
            if (tabu[m.u] > iteration && scratch.latency >= best.latency) return;
            if (scratch.latency < selected_latency ||
                (scratch.latency == selected_latency && scratch.pressure < selected_pressure)) {
                selected = m;
                selected_latency = scratch.latency;
                selected_pressure = scratch.pressure;
                ties = 1;
            } else if (scratch.latency == selected_latency && scratch.pressure == selected_pressure &&
                       rng() % ++ties == 0) {
                selected = m;
            }
        };
        // Critical machine arcs supply the adjacent-swap neighborhood.
        auto const samples = std::min<std::size_t>(critical.size(), 32);
        for (std::size_t i = 0; i < samples; ++i) {
            auto const u = critical[i];
            if (state.next[u] >= 0) consider({u, state.next[u], false});
            if (state.prev[u] >= 0) consider({u, state.prev[u], true});
            // Cross-lane and longer relocations are essential with parallel units.
            // Probe nearby completion times, while retaining occasional distant moves.
            auto const &group = by_kind[g.kind[u]];
            for (int trial = 0; trial < 2; ++trial) {
                auto v = group[rng() % group.size()];
                for (int j = 0; j < 6 && std::abs(current.step[v] - current.step[u]) > 4; ++j)
                    v = group[rng() % group.size()];
                consider({u, v, (rng() & 1U) != 0});
            }
        }
        if (selected.u < 0 || stale >= 80) {
            state = lanes_from_steps(g, best.step, &rng);
            evaluate(g, state, current, best.latency);
            std::fill(tabu.begin(), tabu.end(), 0);
            stale = 0;
            continue;
        }
        apply(state, selected);
        // Re-evaluate the selected edit only once; candidate evaluations reuse scratch.
        if (!evaluate(g, state, current, best.latency + 1)) break;
        ++stats.accepted;
        tabu[selected.u] = iteration + 5 + static_cast<int>(rng() % 7);
        if (current.latency < best.latency) {
            best.latency = current.latency;
            best.step = current.step;
            stale = 0;
        } else {
            ++stale;
        }
    }
    stats.final_latency = best.latency;
    return stats;
}
