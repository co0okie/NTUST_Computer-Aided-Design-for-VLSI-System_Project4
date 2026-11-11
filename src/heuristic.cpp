#include "heuristic.hpp"

#include <algorithm>
#include <cstdio>
#include <queue>
#include <utility>

#include "bounds.hpp"

namespace {

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

HeuristicResult heuristic_schedule(OpGraph const &g, Clock const &clock, double time_budget) {
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
        if (clock.elapsed() > time_budget) break;
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
