#include "heuristic.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <iostream>
#include <queue>
#include <random>
#include <utility>

#include "bounds.hpp"

namespace {

// Work units charged for one list-scheduling pass: n, more on graphs with over 4 edges per op
// (calibrated on a 3000-op graph with 50 fan-ins per op)
std::int64_t pass_work(OpGraph const &g) {
    auto const n = static_cast<std::int64_t>(g.n);
    return std::max(n, n / 2 + static_cast<std::int64_t>(g.succ.item.size()) / 8);
}

std::int32_t latency_of(std::vector<std::int32_t> const &step) {
    std::int32_t latency = 0;
    for (auto const s : step) latency = std::max(latency, s);
    return latency;
}

} // namespace

std::vector<std::int32_t> list_schedule(OpGraph const &g, std::vector<std::int64_t> const &key) {
    using Entry = std::pair<std::int64_t, std::int32_t>; // (key, -index): smaller index first
    std::array<std::priority_queue<Entry>, OP_KINDS> ready;
    std::vector<std::int32_t> waiting(g.n);
    std::vector<std::int32_t> next, done;
    for (std::int32_t i = 0; i < g.n; ++i) {
        waiting[i] = static_cast<std::int32_t>(g.pred.row(i).size());
        if (waiting[i] == 0) next.push_back(i);
    }
    std::vector<std::int32_t> step(g.n, 0);
    auto remaining = g.n;
    for (std::int32_t t = 1; remaining > 0; ++t) {
        for (auto const i : next) ready[g.kind[i]].push({key[i], -i});
        next.clear();
        done.clear();
        for (int k = 0; k < OP_KINDS; ++k) {
            for (std::int32_t used = 0; used < g.limit[k] && !ready[k].empty(); ++used) {
                auto const i = -ready[k].top().second;
                ready[k].pop();
                step[i] = t;
                done.push_back(i);
            }
        }
        for (auto const i : done)
            for (auto const j : g.succ.row(i))
                if (--waiting[j] == 0) next.push_back(j);
        remaining -= static_cast<std::int32_t>(done.size());
    }
    return step;
}

std::vector<std::int32_t> unlock_schedule(OpGraph const &g, Closure const &closure,
                                          std::vector<std::int64_t> const &key,
                                          UnlockOptions const &o) {
    auto const n = g.n;
    auto const words = closure.words;
    auto const is_target = [&](std::int32_t i) { return (o.target_kinds >> g.kind[i] & 1U) != 0; };
    // need[a]: unscheduled ancestors of a
    std::vector<std::int32_t> need(n, 0);
    for (std::int32_t i = 0; i < n; ++i) {
        auto const *const row = closure.row(i);
        for (auto w = static_cast<std::int64_t>(i / 64); w < words; ++w)
            for (auto bits = row[w]; bits != 0; bits &= bits - 1)
                ++need[w * 64 + __builtin_ctzll(bits)];
    }
    // bucket[v]: targets with need v (entries go stale when need changes; a fresh entry is
    // pushed to the new bucket)
    std::vector<std::vector<std::int32_t>> bucket(o.horizon + 1);
    for (std::int32_t i = 0; i < n; ++i)
        if (is_target(i) && need[i] >= 1 && need[i] <= o.horizon) bucket[need[i]].push_back(i);

    using Entry = std::pair<std::int64_t, std::int32_t>;
    std::array<std::priority_queue<Entry>, OP_KINDS> ready;
    std::vector<std::int32_t> waiting(n), step(n, 0), seen(n, 0), stack, chosen;
    for (std::int32_t i = 0; i < n; ++i) {
        waiting[i] = static_cast<std::int32_t>(g.pred.row(i).size());
        if (waiting[i] == 0) ready[g.kind[i]].push({key[i], -i});
    }
    std::int32_t stamp = 0;
    auto remaining = n;
    for (std::int32_t t = 1; remaining > 0; ++t) {
        auto free = g.limit;
        chosen.clear();
        std::int32_t examined = 0;
        auto const any_free = [&] {
            for (int k = 0; k < OP_KINDS; ++k)
                if ((o.driven_kinds >> k & 1U) != 0 && free[k] > 0) return true;
            return false;
        };
        for (std::int32_t v = 1; v <= o.horizon && examined < o.max_targets && any_free(); ++v) {
            auto &list = bucket[v];
            for (std::size_t p = 0; p < list.size() && examined < o.max_targets && any_free();) {
                auto const a = list[p];
                if (step[a] != 0 || need[a] != v) {
                    list[p] = list.back();
                    list.pop_back();
                    continue;
                }
                ++p;
                ++examined;
                // Ready ancestors of a
                ++stamp;
                stack.assign(1, a);
                while (!stack.empty()) {
                    auto const x = stack.back();
                    stack.pop_back();
                    for (auto const y : g.pred.row(x)) {
                        if (step[y] != 0 || seen[y] == stamp) continue;
                        seen[y] = stamp;
                        if (waiting[y] > 0) {
                            stack.push_back(y);
                        } else if ((o.driven_kinds >> g.kind[y] & 1U) != 0 && free[g.kind[y]] > 0) {
                            --free[g.kind[y]];
                            step[y] = t;
                            chosen.push_back(y);
                        }
                    }
                }
            }
        }
        for (int k = 0; k < OP_KINDS; ++k) {
            while (free[k] > 0 && !ready[k].empty()) {
                auto const i = -ready[k].top().second;
                ready[k].pop();
                if (step[i] != 0) continue; // already taken through a target
                --free[k];
                step[i] = t;
                chosen.push_back(i);
            }
        }
        for (auto const i : chosen) {
            for (auto const j : g.succ.row(i))
                if (--waiting[j] == 0) ready[g.kind[j]].push({key[j], -j});
            auto const *const row = closure.row(i);
            for (auto w = static_cast<std::int64_t>(i / 64); w < words; ++w) {
                for (auto bits = row[w]; bits != 0; bits &= bits - 1) {
                    auto const a = static_cast<std::int32_t>(w * 64 + __builtin_ctzll(bits));
                    auto const v = --need[a];
                    if (v >= 1 && v <= o.horizon && is_target(a)) bucket[v].push_back(a);
                }
            }
        }
        remaining -= static_cast<std::int32_t>(chosen.size());
    }
    return step;
}

HeuristicResult heuristic_schedule(OpGraph const &g, Clock const &clock, double deadline_elapsed) {
    HeuristicResult r;
    auto const mirror = reversed(g);
    auto const closure = descendants(g);
    r.tail = resource_tails(g, closure);
    auto const mirror_closure = descendants(mirror);
    auto const mirror_tail = resource_tails(mirror, mirror_closure);
    r.head.resize(g.n);
    for (std::int32_t i = 0; i < g.n; ++i) r.head[i] = mirror_tail[g.n - 1 - i] + 1;
    r.lower_bound = jackson_bound(g, r.head, r.tail);

    std::vector<std::int64_t> key(g.n);
    auto const consider = [&](std::vector<std::int32_t> step) {
        ++r.passes;
        auto const latency = latency_of(step);
        if (r.latency == 0 || latency < r.latency) {
            r.latency = latency;
            r.step = std::move(step);
            return true;
        }
        return false;
    };

    // Forward pass: largest resource-aware tail first
    for (std::int32_t i = 0; i < g.n; ++i) key[i] = r.tail[i];
    consider(list_schedule(g, key));
    r.first_latency = r.latency;
    if (r.latency == r.lower_bound) return r;
    // Backward pass: the same on the mirror problem
    for (std::int32_t i = 0; i < g.n; ++i) key[i] = mirror_tail[i];
    consider(mirror_steps(list_schedule(mirror, key)));

    // Unlock-driven passes: keep the bottleneck kind's machines fed
    if (!closure.empty()) {
        int bottleneck = 0;
        std::int64_t load = -1;
        for (int k = 0; k < OP_KINDS; ++k) {
            if (g.count[k] == 0) continue;
            auto const l = (static_cast<std::int64_t>(g.count[k]) + g.limit[k] - 1) / g.limit[k];
            if (l > load) load = l, bottleneck = k;
        }
        for (int variant = 0; variant < 4 && r.latency > r.lower_bound; ++variant) {
            UnlockOptions o;
            o.target_kinds = variant < 2 ? 1U << bottleneck : 7U;
            o.driven_kinds = variant % 2 == 0 ? 7U & ~(1U << bottleneck) : 7U;
            for (std::int32_t i = 0; i < g.n; ++i) key[i] = r.tail[i];
            auto const f = unlock_schedule(g, closure, key, o);
            consider(f);
            for (std::int32_t i = 0; i < g.n; ++i) key[i] = mirror_tail[i];
            auto const b = unlock_schedule(mirror, mirror_closure, key, o);
            consider(mirror_steps(b));
        }
    }

    // Forward-backward improvement: re-schedule the mirror problem with the ops ordered by their
    // current step (latest first), then the original problem likewise, and so on.
    auto current = r.step;
    for (int stale = 0; stale < 4 && r.latency > r.lower_bound;) {
        if (clock.elapsed() > deadline_elapsed) break;
        auto const big = static_cast<std::int64_t>(g.n) + r.latency + 2;
        for (std::int32_t i = 0; i < g.n; ++i)
            key[g.n - 1 - i] = static_cast<std::int64_t>(current[i]) * big + mirror_tail[g.n - 1 - i];
        auto const back = list_schedule(mirror, key);
        for (std::int32_t i = 0; i < g.n; ++i)
            key[i] = static_cast<std::int64_t>(back[g.n - 1 - i]) * big + r.tail[i];
        current = list_schedule(g, key);
        auto const a = consider(mirror_steps(back));
        auto const b = consider(current);
        stale = (a || b) ? 0 : stale + 1;
    }
    return r;
}

void restart_schedule(OpGraph const &g, HeuristicResult &r, std::int64_t work_budget,
                      std::int64_t &work, std::uint64_t seed, Clock const &clock,
                      double deadline_elapsed) {
    if (g.n == 0 || r.latency <= r.lower_bound || clock.elapsed() >= deadline_elapsed) return;
    auto const mirror = reversed(g);
    auto const closure = descendants(g);
    auto const mirror_closure = descendants(mirror);
    auto const mirror_tail = resource_tails(mirror, mirror_closure);
    auto const &tail = r.tail;

    int bottleneck = 0;
    {
        std::int64_t load = -1;
        for (int k = 0; k < OP_KINDS; ++k) {
            if (g.count[k] == 0) continue;
            auto const l = (static_cast<std::int64_t>(g.count[k]) + g.limit[k] - 1) / g.limit[k];
            if (l > load) load = l, bottleneck = k;
        }
    }
    auto const unlock_options = [&](int variant) {
        UnlockOptions o;
        o.target_kinds = variant < 2 ? 1U << bottleneck : 7U;
        o.driven_kinds = variant % 2 == 0 ? 7U & ~(1U << bottleneck) : 7U;
        return o;
    };

    constexpr std::int64_t W = 1024;
    constexpr double amps[] = {0.5, 1, 2, 4};
    std::mt19937_64 rng(seed);
    std::vector<std::int64_t> key(g.n), mkey(g.n);
    // Work units: about 50 ns each on the development machine (see improve_deterministic)
    auto const list_work = 2 * pass_work(g);
    auto const unlock_work = list_work * (2 + closure.words / 4);
    std::vector<std::int32_t> cur, back;

    auto const consider = [&](std::vector<std::int32_t> const &step) {
        ++r.passes;
        auto const latency = latency_of(step);
        if (latency < r.latency) {
            r.latency = latency;
            r.step = step;
        }
    };

    for (std::uint64_t iter = 0; r.latency > r.lower_bound && work < work_budget &&
                                 clock.elapsed() < deadline_elapsed;
         ++iter) {
        auto const amp = amps[rng() % 4];
        auto const noise_range = static_cast<std::uint64_t>(amp * W);
        auto const from_step = rng() % 3 == 0; // base key: the best schedule so far
        auto const strategy = (iter < 6 && iter != 0) ? iter : rng() % 6; // 0 fwd, 1 mirror, 2..5 unlock
        for (std::int32_t i = 0; i < g.n; ++i) {
            auto const noise = static_cast<std::int64_t>(rng() % noise_range);
            auto const base = from_step ? static_cast<std::int64_t>(r.latency + 1 - r.step[i]) * W
                                        : tail[i] * W;
            key[i] = base + noise;
        }
        for (std::int32_t i = 0; i < g.n; ++i) {
            auto const noise = static_cast<std::int64_t>(rng() % noise_range);
            auto const base = from_step ? static_cast<std::int64_t>(r.step[g.n - 1 - i]) * W
                                        : mirror_tail[i] * W;
            mkey[i] = base + noise;
        }
        bool const use_closure = !closure.empty();
        work += strategy <= 1 || !use_closure ? list_work : unlock_work;
        if (strategy == 0 || (strategy >= 2 && !use_closure)) {
            cur = list_schedule(g, key);
        } else if (strategy == 1) {
            cur = mirror_steps(list_schedule(mirror, mkey));
        } else if (strategy % 2 == 0) {
            cur = unlock_schedule(g, closure, key, unlock_options(static_cast<int>(rng() % 4)));
        } else {
            cur = mirror_steps(
                unlock_schedule(mirror, mirror_closure, mkey, unlock_options(static_cast<int>(rng() % 4))));
        }
        consider(cur);
        auto const rounds = 2 + static_cast<int>(rng() % 3);
        for (int round = 0; round < rounds && r.latency > r.lower_bound; ++round) {
            if (clock.elapsed() >= deadline_elapsed) break;
            work += 2 * list_work;
            auto const big = static_cast<std::int64_t>(g.n) + latency_of(cur) + 2;
            for (std::int32_t i = 0; i < g.n; ++i)
                mkey[g.n - 1 - i] = static_cast<std::int64_t>(cur[i]) * big + mirror_tail[g.n - 1 - i];
            back = list_schedule(mirror, mkey);
            for (std::int32_t i = 0; i < g.n; ++i)
                key[i] = static_cast<std::int64_t>(back[g.n - 1 - i]) * big + tail[i];
            cur = list_schedule(g, key);
            consider(mirror_steps(back));
            consider(cur);
        }
    }
}

void guided_schedule(OpGraph const &g, HeuristicResult &r, std::vector<std::int64_t> const &key,
                     bool on_mirror, Clock const &clock, double deadline_elapsed) {
    if (g.n == 0 || r.latency <= r.lower_bound) return;
    auto const mirror = reversed(g);
    auto const &graph = on_mirror ? mirror : g;
    auto const closure = descendants(graph);
    std::vector<std::int32_t> mirror_tail(g.n);
    for (std::int32_t i = 0; i < g.n; ++i) mirror_tail[g.n - 1 - i] = r.head[i] - 1;
    int bottleneck = 0;
    {
        std::int64_t load = -1;
        for (int k = 0; k < OP_KINDS; ++k) {
            if (g.count[k] == 0) continue;
            auto const l = (static_cast<std::int64_t>(g.count[k]) + g.limit[k] - 1) / g.limit[k];
            if (l > load) load = l, bottleneck = k;
        }
    }
    std::vector<std::int32_t> best;
    std::int32_t best_latency = 0;
    auto const consider = [&](std::vector<std::int32_t> step) {
        ++r.passes;
        if (on_mirror) step = mirror_steps(step);
        auto const latency = latency_of(step);
        if (best_latency == 0 || latency < best_latency) {
            best_latency = latency;
            best = step;
        }
        if (latency < r.latency) {
            r.latency = latency;
            r.step = std::move(step);
        }
    };
    consider(list_schedule(graph, key));
    for (int variant = 0; variant < 4 && !closure.empty(); ++variant) {
        if (clock.elapsed() >= deadline_elapsed) break;
        UnlockOptions o;
        o.target_kinds = variant < 2 ? 1U << bottleneck : 7U;
        o.driven_kinds = variant % 2 == 0 ? 7U & ~(1U << bottleneck) : 7U;
        consider(unlock_schedule(graph, closure, key, o));
    }
    // Forward-backward rounds from the best guided schedule (in original indexing from here on)
    auto current = best;
    std::vector<std::int64_t> k2(g.n);
    for (int stale = 0; stale < 4 && r.latency > r.lower_bound;) {
        if (clock.elapsed() >= deadline_elapsed) break;
        auto const big = static_cast<std::int64_t>(g.n) + latency_of(current) + 2;
        for (std::int32_t i = 0; i < g.n; ++i)
            k2[g.n - 1 - i] = static_cast<std::int64_t>(current[i]) * big + mirror_tail[g.n - 1 - i];
        auto const back = mirror_steps(list_schedule(mirror, k2));
        for (std::int32_t i = 0; i < g.n; ++i)
            k2[i] = static_cast<std::int64_t>(latency_of(back) + 1 - back[i]) * big + r.tail[i];
        current = list_schedule(g, k2);
        auto const before = r.latency;
        if (latency_of(back) < r.latency) r.latency = latency_of(back), r.step = back;
        if (latency_of(current) < r.latency) r.latency = latency_of(current), r.step = current;
        stale = r.latency < before ? 0 : stale + 1;
    }
}

namespace {

// (latency, excess) with excess = sum over ops of max(0, step + tail - target): 0 iff every op
// leaves room for its resource-aware tail before `target`. Used to pick among equal latencies.
std::pair<std::int32_t, std::int64_t> schedule_measure(std::vector<std::int32_t> const &step,
                                                       std::vector<std::int32_t> const &tail,
                                                       std::int32_t target) {
    std::int32_t latency = 0;
    std::int64_t excess = 0;
    for (std::size_t i = 0; i < step.size(); ++i) {
        latency = std::max(latency, step[i]);
        excess += std::max(0, step[i] + tail[i] - target);
    }
    return {latency, excess};
}

// Depth-first search state for target_dfs.
struct TargetDfs {
    explicit TargetDfs(OpGraph const &graph) : g(graph) {}

    OpGraph const &g;
    std::vector<std::int32_t> lst;  // latest step: target - tail
    std::vector<std::int64_t> rank; // branching order inside a kind (smaller first)
    std::int64_t node_limit = 0;
    std::int64_t nodes = 0;
    std::vector<std::int32_t> step, est; // step 0: not scheduled yet
    std::vector<std::vector<std::pair<std::int32_t, std::int32_t>>> buckets; // by est: (lst, op)
    std::vector<std::pair<std::int32_t, std::int32_t>> heap;
    std::int32_t remaining = 0;

    // Steps 1..t are decided. Earliest steps of the open ops along precedence, then each kind
    // on its own: unit jobs with release est and deadline lst on limit[k] machines, which EDF
    // decides exactly. False: no completion meets the target.
    bool propagate(std::int32_t t) {
        for (std::int32_t i = 0; i < g.n; ++i) {
            if (step[i] != 0) continue;
            std::int32_t e = t + 1;
            for (auto const p : g.pred.row(i)) e = std::max(e, step[p] != 0 ? step[p] + 1 : est[p] + 1);
            est[i] = e;
            if (e > lst[i]) return false;
        }
        auto const cmp = std::greater<std::pair<std::int32_t, std::int32_t>>();
        for (int k = 0; k < OP_KINDS; ++k) {
            std::int32_t count = 0;
            for (std::int32_t i = 0; i < g.n; ++i) {
                if (step[i] != 0 || g.kind[i] != k) continue;
                auto const b = static_cast<std::size_t>(est[i] - t - 1);
                if (b >= buckets.size()) buckets.resize(b + 1);
                buckets[b].push_back({lst[i], i});
                ++count;
            }
            heap.clear();
            bool ok = true;
            for (std::size_t s = 0; ok && (count > 0 || !heap.empty()); ++s) {
                if (s < buckets.size()) {
                    for (auto const &d : buckets[s]) {
                        heap.push_back(d);
                        std::push_heap(heap.begin(), heap.end(), cmp);
                        --count;
                    }
                }
                for (std::int32_t u = 0; u < g.limit[k] && !heap.empty(); ++u) {
                    std::pop_heap(heap.begin(), heap.end(), cmp);
                    if (heap.back().first < t + 1 + static_cast<std::int32_t>(s)) ok = false;
                    heap.pop_back();
                }
            }
            for (auto &b : buckets) b.clear();
            if (!ok) return false;
        }
        return true;
    }

    using Lists = std::array<std::vector<std::int32_t>, OP_KINDS>;

    bool search_step(std::int32_t t) {
        if (remaining == 0) return true;
        Lists list; // ready ops per kind, in branching order
        for (std::int32_t i = 0; i < g.n; ++i)
            if (step[i] == 0 && est[i] <= t) list[g.kind[i]].push_back(i);
        for (auto &l : list)
            std::sort(l.begin(), l.end(), [&](std::int32_t a, std::int32_t b) {
                return rank[a] != rank[b] ? rank[a] < rank[b] : a < b;
            });
        return choose(t, list, 0, 0, g.limit[0]);
    }

    // Decide the ready ops c[k][pos..] of kind k (then the next kinds) for step t with
    // `capacity` units of kind k left. Only full selections min(ready, limit) are tried: for unit
    // times some list schedule is optimal, and list schedules never idle a unit next to a ready op.
    bool choose(std::int32_t t, Lists const &c, int k, std::size_t pos, std::int32_t capacity) {
        if (k == OP_KINDS) {
            if (++nodes > node_limit) return false;
            return propagate(t) && search_step(t + 1);
        }
        auto const &list = c[k];
        if (capacity == 0 || pos == list.size()) {
            for (auto p = pos; p < list.size(); ++p)
                if (lst[list[p]] <= t) return false; // postponed, but due now
            return choose(t, c, k + 1, 0, k + 1 < OP_KINDS ? g.limit[k + 1] : 0);
        }
        auto const x = list[pos];
        step[x] = t;
        --remaining;
        if (choose(t, c, k, pos + 1, capacity - 1)) return true;
        step[x] = 0;
        ++remaining;
        if (nodes > node_limit) return false;
        if (lst[x] > t && list.size() - pos - 1 >= static_cast<std::size_t>(capacity))
            return choose(t, c, k, pos + 1, capacity);
        return false;
    }
};

} // namespace

DfsResult target_dfs(OpGraph const &g, std::vector<std::int32_t> const &tail,
                     std::vector<std::int32_t> const &incumbent, std::int32_t target,
                     std::int64_t node_limit) {
    TargetDfs s(g);
    s.lst.resize(g.n);
    s.rank.resize(g.n);
    s.node_limit = node_limit;
    for (std::int32_t i = 0; i < g.n; ++i) {
        s.lst[i] = target - tail[i];
        // Earliest deadline first; ties: earlier in the incumbent schedule
        s.rank[i] = static_cast<std::int64_t>(s.lst[i]) * 262144 + incumbent[i];
    }
    s.step.assign(g.n, 0);
    s.est.assign(g.n, 1);
    s.remaining = g.n;
    DfsResult r;
    bool const found = s.propagate(0) && s.search_step(1);
    r.nodes = std::min(s.nodes, node_limit);
    if (found) r.step = std::move(s.step);
    else r.infeasible = s.nodes <= node_limit;
    return r;
}

void local_search(OpGraph const &g, HeuristicResult &r, std::int64_t work_budget,
                  std::int64_t &work, std::uint64_t seed, Clock const &clock,
                  double deadline_elapsed) {
    if (g.n == 0 || r.latency <= r.lower_bound) return;
    auto const mirror = reversed(g);
    std::vector<std::int32_t> mtail(g.n);
    for (std::int32_t i = 0; i < g.n; ++i) mtail[g.n - 1 - i] = r.head[i] - 1;
    std::mt19937_64 rng(seed);
    constexpr std::int64_t W = 1 << 16; // one step in key units
    constexpr double amps[] = {0.25, 0.5, 1, 2};
    auto const measure = [&](std::vector<std::int32_t> const &s) {
        return schedule_measure(s, r.tail, r.latency - 1);
    };
    auto cur = r.step;
    auto cur_latency = r.latency;
    std::vector<std::int64_t> key(g.n);
    while (work < work_budget && r.latency > r.lower_bound && clock.elapsed() < deadline_elapsed) {
        // Perturb the current schedule's step order, schedule forward or on the mirror
        auto const range = static_cast<std::uint64_t>(amps[rng() % 4] * W) + 1;
        auto const lc = latency_of(cur);
        std::vector<std::int32_t> s;
        if (rng() % 2 == 0) {
            for (std::int32_t i = 0; i < g.n; ++i)
                key[i] = static_cast<std::int64_t>(lc + 1 - cur[i]) * W +
                         static_cast<std::int64_t>(rng() % range) + r.tail[i] * 16;
            s = list_schedule(g, key);
        } else {
            for (std::int32_t i = 0; i < g.n; ++i)
                key[g.n - 1 - i] = static_cast<std::int64_t>(cur[i]) * W +
                                   static_cast<std::int64_t>(rng() % range) + mtail[g.n - 1 - i] * 16;
            s = mirror_steps(list_schedule(mirror, key));
        }
        work += 5 * pass_work(g);
        // Two forward-backward rounds; keep the best of the five schedules by (latency, excess)
        auto best = s;
        auto best_m = measure(s);
        for (int round = 0; round < 2; ++round) {
            for (std::int32_t i = 0; i < g.n; ++i)
                key[g.n - 1 - i] = static_cast<std::int64_t>(s[i]) * W + mtail[g.n - 1 - i];
            auto const b = mirror_steps(list_schedule(mirror, key));
            auto const lb = latency_of(b);
            for (std::int32_t i = 0; i < g.n; ++i)
                key[i] = static_cast<std::int64_t>(lb + 1 - b[i]) * W + r.tail[i];
            s = list_schedule(g, key);
            auto const mb = measure(b);
            if (mb < best_m) best_m = mb, best = b;
            auto const ms = measure(s);
            if (ms < best_m) best_m = ms, best = s;
        }
        if (best_m.first < r.latency) {
            r.latency = best_m.first;
            r.step = best;
        }
        // Walk: accept any move that keeps the latency (plateaus are where these cases sit)
        if (best_m.first <= cur_latency) {
            cur = std::move(best);
            cur_latency = best_m.first;
        }
    }
}

void improve_schedule(OpGraph const &g, HeuristicResult &r, std::int64_t work_budget,
                      Clock const &clock, double deadline_elapsed, std::int64_t &work) {
    if (g.n == 0 || r.latency <= r.lower_bound) return;
    // Phase 1 (20%): depth-first search for latency - 1, repeated while it succeeds. Only for
    // n <= 5000: the recursion depth is about n + latency. A search node costs about n / 4 units.
    constexpr double dfs_share = 0.2, restart_share = 0.3;
    auto const dfs_budget = static_cast<std::int64_t>(static_cast<double>(work_budget) * dfs_share);
    if (g.n <= 5000) {
        auto const nodes = dfs_budget * 4 / pass_work(g);
        std::int64_t used = 0;
        while (r.latency > r.lower_bound && used < nodes && clock.elapsed() < deadline_elapsed) {
            auto const d = target_dfs(g, r.tail, r.step, r.latency - 1, nodes - used);
            used += d.nodes;
            if (d.infeasible) r.lower_bound = r.latency; // exhausted: latency - 1 is impossible
            if (d.step.empty()) break;
            r.step = d.step;
            r.latency = latency_of(d.step);
        }
    }
    work += dfs_budget;
    // Phase 2 (30%): randomized restarts; phase 3 (the rest): local search from the best
    auto const restart_budget = work + static_cast<std::int64_t>(static_cast<double>(work_budget) * restart_share);
    restart_schedule(g, r, restart_budget, work, 0x9e3779b97f4a7c15ULL, clock, deadline_elapsed);
    local_search(g, r, work_budget, work, 0x5eed1234abcdULL, clock, deadline_elapsed);
}
