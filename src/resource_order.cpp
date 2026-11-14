#include "resource_order.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <string>
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

OrderSearchStats improve_resource_order_single(OpGraph const &g, HeuristicResult &best,
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

namespace {
// Removing an operation only relaxes machine precedence. Its data edges remain.
// Bridging its neighbors preserves their relative order in every reconstruction.
void detach(Lanes &s, Id u) {
    auto const p = s.prev[u], q = s.next[u];
    if (p >= 0) s.next[p] = q;
    if (q >= 0) s.prev[q] = p;
    s.prev[u] = s.next[u] = s.lane[u] = -1;
}

void insert_after(Lanes &s, Id u, Id anchor, Id lane) {
    Id q = -1;
    if (anchor >= 0) q = s.next[anchor];
    else {
        for (Id v = 0; v < static_cast<Id>(s.lane.size()); ++v)
            if (s.lane[v] == lane && s.prev[v] < 0) { q = v; break; }
    }
    s.lane[u] = lane;
    s.prev[u] = anchor;
    s.next[u] = q;
    if (anchor >= 0) s.next[anchor] = u;
    if (q >= 0) s.prev[q] = u;
}

// Pick one machine-edge endpoint on a randomly traced longest path. If that
// path contains data edges only, no machine reordering can shorten the schedule.
Id critical_endpoint(OpGraph const &g, Lanes const &s, Evaluation const &e,
                     std::mt19937_64 &rng) {
    std::vector<Id> sinks;
    for (Id u = 0; u < g.n; ++u) if (e.step[u] == e.latency) sinks.push_back(u);
    if (sinks.empty()) return -1;
    auto u = sinks[rng() % sinks.size()];
    std::vector<Id> endpoints, pred;
    while (e.step[u] > 1) {
        pred.clear();
        for (auto const v : g.pred.row(u))
            if (e.step[v] + 1 == e.step[u]) pred.push_back(v);
        auto const p = s.prev[u];
        if (p >= 0 && e.step[p] + 1 == e.step[u]) pred.push_back(p);
        if (pred.empty()) break;
        auto const v = pred[rng() % pred.size()];
        if (v == p) {
            endpoints.push_back(u);
            endpoints.push_back(v);
        }
        u = v;
    }
    return endpoints.empty() ? -1 : endpoints[rng() % endpoints.size()];
}

struct BeamState {
    Lanes lanes;
    Id latency = 0;
    std::int64_t pressure = 0;
};

bool beam_less(BeamState const &a, BeamState const &b) {
    return a.latency != b.latency ? a.latency < b.latency : a.pressure < b.pressure;
}
} // namespace

namespace {
bool metropolis_accept(double delta, double temperature, double uniform) {
    return delta <= 0 || uniform < std::exp(-delta / temperature);
}

OrderSearchStats improve_resource_order_anneal(OpGraph const &g, HeuristicResult &best,
                                               Clock const &clock, double deadline_elapsed) {
    OrderSearchStats stats;
    stats.initial_latency = stats.final_latency = best.latency;
    if (g.n < 2 || best.latency <= best.lower_bound || clock.elapsed() >= deadline_elapsed)
        return stats;
    std::mt19937_64 rng(0x8cb92baa3f3d8dd7ULL);
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
    auto const rebuild_critical = [&]() {
        critical.clear();
        for (Id u = 0; u < g.n; ++u)
            if (current.step[u] + current.tail[u] >= current.latency) critical.push_back(u);
    };
    rebuild_critical();
    // Match the existing single-order candidate work budget. Eight reheating
    // epochs avoid dependence on the speed of the machine within the wall cap.
    constexpr std::uint64_t work_limit = 350'000'000;
    constexpr std::uint64_t epoch_work = work_limit / 8;
    auto const cost = static_cast<std::uint64_t>(g.n) + g.succ.item.size();
    std::uint64_t work = 0, next_restart = epoch_work;
    std::uint64_t uphill = 0, plateau = 0, downhill = 0, restarts = 0;
    auto const energy = [&](Evaluation const &e) {
        // The pressure term spans at most 0.2 cycles. A one-cycle rise always
        // has positive energy, while equal-latency states can freely diffuse.
        return static_cast<double>(e.latency) +
               0.2 * static_cast<double>(e.pressure) / (4096.0 * g.n);
    };
    while (best.latency > best.lower_bound && work < work_limit &&
           clock.elapsed() < deadline_elapsed) {
        if (work >= next_restart) {
            state = lanes_from_steps(g, best.step, &rng);
            if (!evaluate(g, state, current, best.latency)) break;
            rebuild_critical();
            ++restarts;
            next_restart += epoch_work;
        }
        auto u = critical.empty() || rng() % 4 == 0 ?
                 static_cast<Id>(rng() % static_cast<std::uint64_t>(g.n)) :
                 critical[rng() % critical.size()];
        Move move;
        if (rng() % 2 == 0) {
            bool const forward = (rng() & 1U) != 0;
            auto const v = forward ? state.next[u] : state.prev[u];
            if (v < 0) { work += cost; continue; }
            move = {u, v, !forward};
        } else {
            auto const &group = by_kind[g.kind[u]];
            auto v = group[rng() % group.size()];
            for (int trial = 0; trial < 6 && std::abs(current.step[v] - current.step[u]) > 4; ++trial)
                v = group[rng() % group.size()];
            move = {u, v, (rng() & 1U) != 0};
        }
        work += cost;
        if (!useful(state, move)) continue;
        ++stats.attempts;
        auto const undo = apply(state, move);
        if (!evaluate(g, state, scratch, best.latency + 1)) {
            restore(state, undo);
            continue;
        }
        ++stats.feasible;
        auto const fraction = static_cast<double>(work % epoch_work) / epoch_work;
        // Geometric cooling 0.4 -> 0.02 cycles per epoch: a +1-cycle
        // move initially has about exp(-1/0.4)=8.2% acceptance probability.
        auto const temperature = 0.4 * std::pow(0.02 / 0.4, fraction);
        auto const uniform = static_cast<double>(rng() >> 11) * 0x1.0p-53;
        if (!metropolis_accept(energy(scratch) - energy(current), temperature, uniform)) {
            restore(state, undo);
            continue;
        }
        if (scratch.latency > current.latency) ++uphill;
        else if (scratch.latency < current.latency) ++downhill;
        else ++plateau;
        std::swap(current, scratch);
        ++stats.accepted;
        rebuild_critical();
        if (current.latency < best.latency) {
            best.latency = current.latency;
            best.step = current.step;
        }
    }
    stats.final_latency = best.latency;
    if (auto const *path = std::getenv("MLRCS_ORDER_SA_LOG")) {
        std::ofstream out(path, std::ios::app);
        out << g.n << '\t' << stats.initial_latency << '\t' << stats.final_latency
            << '\t' << uphill << '\t' << plateau << '\t' << downhill
            << '\t' << restarts << '\t' << work << '\n';
    }
    return stats;
}
} // namespace

OrderSearchStats improve_resource_order(OpGraph const &g, HeuristicResult &best,
                                         Clock const &clock, double deadline_elapsed) {
    auto const *method = std::getenv("MLRCS_ORDER_METHOD");
    if (method && std::string(method) == "anneal")
        return improve_resource_order_anneal(g, best, clock, deadline_elapsed);
    if (!method || std::string(method) != "beam")
        return improve_resource_order_single(g, best, clock, deadline_elapsed);
    OrderSearchStats stats;
    stats.initial_latency = stats.final_latency = best.latency;
    if (g.n < 2 || best.latency <= best.lower_bound || clock.elapsed() >= deadline_elapsed)
        return stats;
    // Sixteen linear-sized lane states plus scratch; cap this pass to modest RAM.
    constexpr std::uint64_t memory_limit = 96ULL * 1024 * 1024;
    if (static_cast<std::uint64_t>(g.n) * 3 * sizeof(Id) * 36 > memory_limit)
        return stats;
    constexpr std::size_t beam_width = 16;
    constexpr std::uint64_t work_limit = 1'500'000'000;
    auto const cost = static_cast<std::uint64_t>(3) * g.n + g.succ.item.size();
    std::uint64_t work = 0, destroys = 0, lowered = 0, completions = 0, expanded = 0;
    std::mt19937_64 rng(0x94d049bb133111ebULL);
    std::array<Id, OP_KINDS> base{}, lanes_count{};
    Id lane_total = 0;
    for (int k = 0; k < OP_KINDS; ++k) {
        base[k] = lane_total;
        lanes_count[k] = std::min(g.limit[k], g.count[k]);
        lane_total += lanes_count[k];
    }
    auto const available = [&]() {
        return work < work_limit && clock.elapsed() < deadline_elapsed;
    };
    auto const eval = [&](Lanes const &s, Evaluation &e, Id cutoff) {
        work += cost;
        ++stats.attempts;
        auto const ok = evaluate(g, s, e, cutoff);
        if (ok) ++stats.feasible;
        return ok;
    };
    Evaluation partial, scratch;
    while (best.latency > best.lower_bound && available()) {
        ++destroys;
        auto state = lanes_from_steps(g, best.step, &rng);
        if (!eval(state, partial, best.latency)) break;
        std::vector<Id> removed;
        std::array<int, 4> const sizes{6, 10, 14, 24};
        auto const maximum = std::min<Id>(g.n, sizes[(destroys - 1) % sizes.size()]);
        while (static_cast<Id>(removed.size()) < maximum && available()) {
            auto const u = critical_endpoint(g, state, partial, rng);
            if (u < 0) break;
            removed.push_back(u);
            detach(state, u);
            if (!eval(state, partial, best.latency)) break;
            // Extra removals allow several resource orders to change jointly.
            if (partial.latency < best.latency && removed.size() >= 6) break;
        }
        if (removed.empty() || partial.latency >= best.latency || !available()) continue;
        ++lowered;
        // Different rebuilding orders explore distinct beam prefixes, deterministically.
        if (destroys % 3 == 0) std::shuffle(removed.begin(), removed.end(), rng);
        else std::sort(removed.begin(), removed.end(), [&](Id a, Id b) {
            return best.step[a] != best.step[b] ?
                   (destroys % 3 == 1 ? best.step[a] < best.step[b] : best.step[a] > best.step[b]) : a < b;
        });
        std::vector<BeamState> beam;
        beam.push_back({std::move(state), partial.latency, partial.pressure});
        bool complete = true;
        for (auto const u : removed) {
            std::vector<BeamState> next;
            auto const k = g.kind[u];
            for (auto const &parent : beam) {
                if (!available()) { complete = false; break; }
                ++expanded;
                // All positions in a bounded time window, plus each lane's ends.
                std::vector<std::pair<Id, Id>> positions;
                auto const radius = destroys % 2 ? 4 : 10;
                for (Id v = 0; v < g.n; ++v) {
                    if (parent.lanes.lane[v] < 0 || g.kind[v] != k) continue;
                    if (std::abs(best.step[v] - best.step[u]) <= radius || parent.lanes.next[v] < 0)
                        positions.push_back({v, parent.lanes.lane[v]});
                }
                for (Id lane = base[k]; lane < base[k] + lanes_count[k]; ++lane)
                    positions.push_back({-1, lane});
                std::shuffle(positions.begin(), positions.end(), rng);
                for (auto const &[anchor, lane] : positions) {
                    if (!available()) { complete = false; break; }
                    auto candidate = parent.lanes;
                    insert_after(candidate, u, anchor, lane);
                    if (!eval(candidate, scratch, best.latency - 1)) continue;
                    BeamState child{std::move(candidate), scratch.latency, scratch.pressure};
                    auto const at = std::lower_bound(next.begin(), next.end(), child, beam_less);
                    if (next.size() < beam_width || at != next.end()) {
                        next.insert(at, std::move(child));
                        if (next.size() > beam_width) next.pop_back();
                    }
                }
                if (!complete) break;
            }
            beam = std::move(next);
            if (!complete || beam.empty()) { complete = false; break; }
        }
        if (!complete || beam.empty()) continue;
        ++completions;
        if (eval(beam.front().lanes, scratch, best.latency - 1)) {
            best.latency = scratch.latency;
            best.step = scratch.step;
            ++stats.accepted;
        }
    }
    stats.final_latency = best.latency;
    if (auto const *path = std::getenv("MLRCS_ORDER_LNS_LOG")) {
        std::ofstream out(path, std::ios::app);
        out << g.n << '\t' << stats.initial_latency << '\t' << stats.final_latency
            << '\t' << destroys << '\t' << lowered << '\t' << expanded
            << '\t' << completions << '\t' << work << '\n';
    }
    return stats;
}
