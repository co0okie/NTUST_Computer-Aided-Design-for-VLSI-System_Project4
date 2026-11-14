#include "bounds.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <queue>
#include <unordered_set>

Closure descendants(OpGraph const &g, std::int64_t max_bytes) {
    Closure c;
    auto const n = g.n;
    auto const words = static_cast<std::int64_t>((n + 63) / 64);
    if (words * n * 8 > max_bytes) return c;
    c.words = words;
    c.bits.assign(static_cast<std::size_t>(words * n), 0);
    for (std::int32_t i = n - 1; i >= 0; --i) {
        auto *const mine = c.bits.data() + i * words;
        for (auto const j : g.succ.row(i)) {
            auto const *const theirs = c.row(j);
            // Descendants have larger indices, so only words from j / 64 on can be set
            for (auto w = static_cast<std::int64_t>(j / 64); w < words; ++w) mine[w] |= theirs[w];
            mine[j / 64] |= std::uint64_t{1} << (j % 64);
        }
    }
    return c;
}

std::vector<std::int32_t> resource_tails(OpGraph const &g, Closure const &closure) {
    auto const n = g.n;
    std::vector<std::int32_t> tail(n, 0);
    if (closure.empty()) {
        for (std::int32_t i = n - 1; i >= 0; --i)
            for (auto const j : g.succ.row(i)) tail[i] = std::max(tail[i], tail[j] + 1);
        return tail;
    }
    auto const words = closure.words;
    // hist[k][q]: descendants of kind k with tail q, for the op being processed
    std::array<std::vector<std::int32_t>, OP_KINDS> hist;
    for (auto &h : hist) h.assign(1, 0);
    for (std::int32_t i = n - 1; i >= 0; --i) {
        auto const *const mine = closure.row(i);
        std::int32_t top = -1;
        for (auto w = static_cast<std::int64_t>(i / 64); w < words; ++w) {
            auto bits = mine[w];
            while (bits != 0) {
                auto const j = static_cast<std::int32_t>(w * 64 + __builtin_ctzll(bits));
                bits &= bits - 1;
                auto const q = tail[j];
                if (q > top) {
                    top = q;
                    for (auto &h : hist)
                        if (static_cast<std::int32_t>(h.size()) <= q) h.resize(2 * q + 2, 0);
                }
                ++hist[g.kind[j]][q];
            }
        }
        std::int32_t best = 0;
        for (int k = 0; k < OP_KINDS; ++k) {
            std::int64_t count = 0;
            auto const c = std::max(1, g.limit[k]);
            for (auto q = top; q >= 0; --q) {
                auto &h = hist[k][q];
                if (h == 0) continue;
                count += h;
                h = 0;
                best = std::max(best, q + static_cast<std::int32_t>((count + c - 1) / c));
            }
        }
        tail[i] = best;
    }
    return tail;
}

std::int32_t jackson_bound(OpGraph const &g, std::vector<std::int32_t> const &head,
                           std::vector<std::int32_t> const &tail) {
    std::int32_t bound = 0;
    std::vector<std::int32_t> order;
    for (int k = 0; k < OP_KINDS; ++k) {
        order.clear();
        for (std::int32_t i = 0; i < g.n; ++i)
            if (g.kind[i] == k) order.push_back(i);
        std::sort(order.begin(), order.end(),
                  [&](std::int32_t a, std::int32_t b) { return head[a] < head[b]; });
        std::priority_queue<std::int32_t> ready; // tails
        std::size_t next = 0;
        std::int32_t t = 0;
        while (next < order.size() || !ready.empty()) {
            ++t;
            if (ready.empty()) t = std::max(t, head[order[next]]);
            while (next < order.size() && head[order[next]] <= t) ready.push(tail[order[next++]]);
            for (std::int32_t used = 0; used < g.limit[k] && !ready.empty(); ++used) {
                bound = std::max(bound, t + ready.top());
                ready.pop();
            }
        }
    }
    return bound;
}

namespace {

std::uint64_t mix64(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

struct PairHash {
    std::size_t operator()(std::pair<std::uint64_t, std::uint64_t> const &p) const {
        return static_cast<std::size_t>(p.first ^ (p.second * 0x9e3779b97f4a7c15ULL));
    }
};

} // namespace

StartIdle start_idle_bound(OpGraph const &g, Closure const &ancestors, std::vector<std::int32_t> const &head,
                           int b, std::int32_t max_steps, std::int64_t max_work) {
    StartIdle best;
    auto const n = g.n;
    auto const c = static_cast<std::int64_t>(g.limit[b]);
    if (g.count[b] == 0 || max_steps < 1) return best;
    // Release-step count: at most `by_head[t]` b ops can be finished by step t
    std::vector<std::int64_t> released(max_steps + 2, 0), by_head(max_steps + 1, 0);
    for (std::int32_t i = 0; i < n; ++i)
        if (g.kind[i] == b && head[i] <= max_steps) ++released[std::max(1, head[i])];
    {
        std::int64_t avail = 0, done = 0;
        for (std::int32_t t = 1; t <= max_steps; ++t) {
            avail += released[t];
            auto const x = std::min(c, avail);
            avail -= x;
            done += x;
            by_head[t] = done;
        }
    }
    // Union bound (only with a closure): by_union[t] for t <= complete
    std::vector<std::int64_t> by_union(max_steps + 1, std::numeric_limits<std::int64_t>::max());
    if (!ancestors.empty()) {
        std::int64_t cap = 0;
        for (int k = 0; k < OP_KINDS; ++k)
            if (k != b) cap += std::int64_t{g.limit[k]} * (max_steps - 1);
        std::map<std::vector<std::int32_t>, std::int32_t> group_of;
        std::vector<std::vector<std::int32_t>> items;
        std::vector<std::int64_t> count;
        std::int64_t free_ops = 0, stored = 0;
        std::vector<std::int32_t> set;
        auto const words = ancestors.words;
        bool complete_sets = true;
        for (std::int32_t i = 0; i < n; ++i) {
            if (g.kind[i] != b) continue;
            set.clear();
            auto const m = n - 1 - i;
            auto const *const row = ancestors.row(m);
            bool fits = true;
            for (auto w = static_cast<std::int64_t>(m / 64); w < words && fits; ++w) {
                for (auto bits = row[w]; bits != 0; bits &= bits - 1) {
                    auto const j = n - 1 - static_cast<std::int32_t>(w * 64 + __builtin_ctzll(bits));
                    if (g.kind[j] == b) continue;
                    set.push_back(j);
                    if (static_cast<std::int64_t>(set.size()) > cap) {
                        fits = false;
                        break;
                    }
                }
            }
            if (!fits) continue; // can never fit within max_steps
            if (set.empty()) {
                ++free_ops;
                continue;
            }
            std::sort(set.begin(), set.end());
            auto const [it, fresh] = group_of.emplace(set, static_cast<std::int32_t>(items.size()));
            if (fresh) {
                stored += static_cast<std::int64_t>(set.size());
                items.push_back(set);
                count.push_back(0);
            }
            ++count[it->second];
            if (stored > 8'000'000) {
                complete_sets = false;
                break;
            }
        }
        group_of.clear();
        auto const groups = static_cast<std::int32_t>(items.size());
        std::vector<std::array<std::int32_t, OP_KINDS>> size(groups);
        for (std::int32_t q = 0; q < groups; ++q) {
            size[q] = {};
            for (auto const x : items[q]) ++size[q][g.kind[x]];
        }
        std::vector<std::vector<std::int32_t>> containing(n); // eligible groups only, per T
        std::vector<std::uint64_t> zob_a(n), zob_b(n);
        for (std::int32_t i = 0; i < n; ++i) {
            zob_a[i] = mix64(static_cast<std::uint64_t>(i) * 2 + 1);
            zob_b[i] = mix64(static_cast<std::uint64_t>(i) * 2 + 2 + (std::uint64_t{1} << 40));
        }
        std::int64_t work = 0;
        // Iterative deepening: exhaustive search over the unions within the budgets of step T
        for (std::int32_t T = 1; T <= max_steps && complete_sets; ++T) {
            std::array<std::int64_t, OP_KINDS> budget{};
            for (int k = 0; k < OP_KINDS; ++k) budget[k] = k == b ? 0 : std::int64_t{g.limit[k]} * (T - 1);
            std::vector<std::int32_t> eligible;
            for (std::int32_t q = 0; q < groups; ++q) {
                bool ok = true;
                for (int k = 0; k < OP_KINDS; ++k) ok = ok && size[q][k] <= budget[k];
                if (ok) eligible.push_back(q);
            }
            for (auto &list : containing) list.clear();
            for (auto const q : eligible)
                for (auto const x : items[q]) containing[x].push_back(q);
            std::vector<std::array<std::int32_t, OP_KINDS>> hit(groups);
            std::vector<std::uint8_t> in(n, 0);
            std::array<std::int64_t, OP_KINDS> used{};
            std::int64_t value = free_ops;
            std::uint64_t ha = 0, hb = 0;
            std::unordered_set<std::pair<std::uint64_t, std::uint64_t>, PairHash> seen;
            std::vector<std::int64_t> best_at(T + 1, 0); // best value with all ancestors done by step t - 1
            std::vector<std::int32_t> added;
            bool aborted = false;
            auto const record = [&] {
                std::int64_t need = 1;
                for (int k = 0; k < OP_KINDS; ++k)
                    if (k != b && used[k] > 0) need = std::max(need, (used[k] + g.limit[k] - 1) / g.limit[k] + 1);
                if (need <= T) best_at[need] = std::max(best_at[need], value);
            };
            auto const total = [&](std::int32_t q) {
                std::int32_t s = 0;
                for (int k = 0; k < OP_KINDS; ++k) s += size[q][k] - hit[q][k];
                return s;
            };
            std::function<void()> dfs = [&] {
                record();
                for (auto const q : eligible) {
                    if (aborted) return;
                    ++work;
                    if (total(q) == 0) continue;
                    bool ok = true;
                    for (int k = 0; k < OP_KINDS; ++k) ok = ok && used[k] + size[q][k] - hit[q][k] <= budget[k];
                    if (!ok) continue;
                    auto na = ha, nb = hb;
                    for (auto const x : items[q])
                        if (in[x] == 0) na ^= zob_a[x], nb ^= zob_b[x];
                    if (!seen.insert({na, nb}).second) continue;
                    if (work > max_work) {
                        aborted = true;
                        return;
                    }
                    auto const mark = added.size();
                    for (auto const x : items[q]) {
                        if (in[x] != 0) continue;
                        in[x] = 1;
                        added.push_back(x);
                        ++used[g.kind[x]];
                        work += static_cast<std::int64_t>(containing[x].size()) + 1;
                        for (auto const p : containing[x])
                            if (++hit[p][g.kind[x]], total(p) == 0) value += count[p];
                    }
                    ha = na, hb = nb;
                    dfs(); // then undo, restoring the hash element by element
                    for (auto a = added.size(); a > mark; --a) {
                        auto const x = added[a - 1];
                        for (auto const p : containing[x]) {
                            if (total(p) == 0) value -= count[p];
                            --hit[p][g.kind[x]];
                        }
                        --used[g.kind[x]];
                        in[x] = 0;
                        ha ^= zob_a[x], hb ^= zob_b[x];
                    }
                    added.resize(mark);
                }
            };
            dfs();
            if (aborted) break;
            std::int64_t running = 0;
            for (std::int32_t t = 1; t <= T; ++t) {
                running = std::max(running, best_at[t]);
                by_union[t] = running;
            }
        }
    }
    for (std::int32_t t = 1; t <= max_steps; ++t) {
        auto const done = std::min({c * t, by_head[t], by_union[t]});
        if (c * t - done > best.slots) best.slots = c * t - done, best.steps = t;
    }
    return best;
}
