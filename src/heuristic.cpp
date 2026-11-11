#include "heuristic.hpp"

#include <algorithm>
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

HeuristicResult heuristic_schedule(OpGraph const &g, Clock const &clock, double time_budget) {
    HeuristicResult r;
    auto const mirror = reversed(g);
    r.tail = resource_tails(g);
    auto const mirror_tail = resource_tails(mirror);
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
