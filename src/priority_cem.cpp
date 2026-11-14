#include "priority_cem.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <numeric>
#include <queue>
#include <random>
#include <vector>

namespace {
using Id = std::int32_t;
struct SerialCandidate {
    std::vector<Id> step, rank;
    Id latency = 0;
    std::int64_t sink_sum = 0;
};

// Eligibility means all predecessor positions are assigned, even when those
// positions are in the future. Later choices fill any earlier usable holes. With
// unit-time operations the final schedule is still non-delay; the research change
// is priority-distribution learning, not a larger set of representable schedules.
bool serial_decode(OpGraph const &g, std::vector<double> const &key, Id horizon,
                   Clock const &clock, double deadline, SerialCandidate &out,
                   std::uint64_t &work) {
    out.step.assign(g.n, 0);
    out.rank.assign(g.n, 0);
    out.latency = 0;
    out.sink_sum = 0;
    if (horizon < 1 || key.size() != static_cast<std::size_t>(g.n)) return false;
    auto const words = static_cast<std::size_t>(horizon + 64) / 64;
    std::array<std::vector<Id>, OP_KINDS> used;
    std::array<std::vector<std::uint64_t>, OP_KINDS> free;
    for (int k = 0; k < OP_KINDS; ++k) {
        used[k].assign(static_cast<std::size_t>(horizon) + 1, 0);
        free[k].assign(words, g.limit[k] > 0 ? ~0ULL : 0);
        free[k][0] &= ~1ULL; // time zero is reserved for primary inputs
    }
    std::vector<Id> degree(g.n), est(g.n, 1);
    auto const compare = [&](Id a, Id b) {
        return key[a] != key[b] ? key[a] > key[b] : a > b;
    };
    std::priority_queue<Id, std::vector<Id>, decltype(compare)> ready(compare);
    for (Id u = 0; u < g.n; ++u) {
        degree[u] = static_cast<Id>(g.pred.row(u).size());
        if (degree[u] == 0) ready.push(u);
    }
    Id done = 0;
    while (!ready.empty()) {
        if ((done & 63) == 0 && clock.elapsed() >= deadline) return false;
        auto const u = ready.top(); ready.pop();
        auto const k = g.kind[u];
        auto const lower = est[u];
        if (lower > horizon) return false;
        auto word = static_cast<std::size_t>(lower / 64);
        auto bits = free[k][word] & (~0ULL << (lower % 64));
        while (bits == 0) {
            ++work;
            if (++word >= words) return false;
            bits = free[k][word];
        }
        auto const t = static_cast<Id>(word * 64 + __builtin_ctzll(bits));
        if (t > horizon) return false;
        out.step[u] = t;
        out.rank[u] = done++;
        out.latency = std::max(out.latency, t);
        if (++used[k][t] >= g.limit[k]) free[k][word] &= ~(1ULL << (t % 64));
        if (g.succ.row(u).empty()) out.sink_sum += t;
        ++work;
        for (auto const v : g.succ.row(u)) {
            ++work;
            est[v] = std::max(est[v], t + 1);
            if (--degree[v] == 0) ready.push(v);
        }
    }
    return done == g.n;
}

bool candidate_less(SerialCandidate const &a, SerialCandidate const &b) {
    if (a.latency != b.latency) return a.latency < b.latency;
    return a.sink_sum < b.sink_sum;
}

std::vector<double> step_priorities(std::vector<Id> const &step) {
    std::vector<Id> order(step.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](Id a, Id b) {
        return step[a] != step[b] ? step[a] < step[b] : a < b;
    });
    std::vector<double> key(step.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        key[order[i]] = static_cast<double>(i) / std::max<std::size_t>(1, order.size());
    return key;
}
} // namespace

OrderSearchStats improve_priority_cem(OpGraph const &g, HeuristicResult &best,
                                      Clock const &clock, double deadline_elapsed) {
    OrderSearchStats stats;
    stats.initial_latency = stats.final_latency = best.latency;
    if (g.n < 2 || g.n > 5000 || best.latency <= best.lower_bound ||
        clock.elapsed() >= deadline_elapsed) return stats;
    // Population 32 * two int vectors, plus O(n) decoder/bitset scratch: <4 MiB
    // at n=5000. An edge-work cap prevents dense graphs consuming all 2048 decodes.
    constexpr std::uint64_t work_limit = 100'000'000;
    constexpr int population = 32, generations = 64, elite_count = 8;
    auto const cost = static_cast<std::uint64_t>(g.n) + g.succ.item.size();
    if (cost > work_limit || best.step.size() != static_cast<std::size_t>(g.n)) return stats;
    std::uint64_t work = 0, completed_generations = 0;
    auto const initial = step_priorities(best.step);
    auto mean = initial;
    std::vector<double> deviation(g.n, 0.25), key(g.n);
    std::mt19937_64 random(0x82a2b175229d6a5bULL);
    std::normal_distribution<double> normal(0.0, 1.0);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    SerialCandidate candidate;
    // Ascending incumbent step is topological. Earliest insertion cannot be later
    // than each operation's old step: all operations with later old steps remain
    // unassigned, so its original slot still has capacity. This is also tested.
    ++stats.attempts;
    if (!serial_decode(g, initial, g.n, clock, deadline_elapsed, candidate, work)) return stats;
    ++stats.feasible;
    if (candidate.latency < best.latency) {
        best.step = candidate.step; best.latency = candidate.latency; ++stats.accepted;
    }
    for (int generation = 0; generation < generations && best.latency > best.lower_bound; ++generation) {
        std::vector<SerialCandidate> elite;
        elite.reserve(elite_count + 1);
        for (int sample = 0; sample < population; ++sample) {
            if (work + cost > work_limit || clock.elapsed() >= deadline_elapsed) break;
            if (sample == 0) key = step_priorities(best.step);
            else for (Id u = 0; u < g.n; ++u) {
                // 20% wholly exploratory samples and 20% seed-centered samples
                // preserve diversity despite diagonal elite-distribution collapse.
                key[u] = sample % 5 == 0 ? uniform(random) :
                         sample % 5 == 1 ? initial[u] + 0.4 * normal(random) :
                         mean[u] + deviation[u] * normal(random);
            }
            ++stats.attempts;
            if (!serial_decode(g, key, g.n, clock, deadline_elapsed, candidate, work)) continue;
            ++stats.feasible;
            if (candidate.latency < best.latency) {
                best.step = candidate.step; best.latency = candidate.latency; ++stats.accepted;
            }
            auto const position = std::upper_bound(elite.begin(), elite.end(), candidate,
                [](SerialCandidate const &a, SerialCandidate const &b) { return candidate_less(a, b); });
            if (position != elite.end() || elite.size() < elite_count) {
                elite.insert(position, candidate);
                if (elite.size() > elite_count) elite.pop_back();
            }
            if (best.latency <= best.lower_bound) break;
        }
        if (elite.empty()) break;
        ++completed_generations;
        for (Id u = 0; u < g.n; ++u) {
            double average = 0, variance = 0;
            for (auto const &e : elite) average += static_cast<double>(e.rank[u]) / g.n;
            average /= static_cast<double>(elite.size());
            for (auto const &e : elite) {
                auto const delta = static_cast<double>(e.rank[u]) / g.n - average;
                variance += delta * delta;
            }
            variance /= static_cast<double>(elite.size());
            mean[u] = 0.7 * mean[u] + 0.3 * average;
            deviation[u] = std::max(0.10, 0.7 * deviation[u] + 0.3 * std::sqrt(variance));
        }
        if (work + cost > work_limit || clock.elapsed() >= deadline_elapsed) break;
    }
    stats.final_latency = best.latency;
    if (auto const *path = std::getenv("MLRCS_ORDER_CEM_LOG")) {
        std::ofstream out(path, std::ios::app);
        out << g.n << '\t' << stats.initial_latency << '\t' << stats.final_latency
            << '\t' << completed_generations << '\t' << work << '\n';
    }
    return stats;
}
