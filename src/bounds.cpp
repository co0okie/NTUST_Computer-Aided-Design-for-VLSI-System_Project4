#include "bounds.hpp"

#include <algorithm>
#include <queue>

std::vector<std::int32_t> resource_tails(OpGraph const &g, std::int64_t max_closure_bytes) {
    auto const n = g.n;
    std::vector<std::int32_t> tail(n, 0);
    auto const words = static_cast<std::int64_t>((n + 63) / 64);
    if (words * n * 8 > max_closure_bytes) {
        for (std::int32_t i = n - 1; i >= 0; --i)
            for (auto const j : g.succ.row(i)) tail[i] = std::max(tail[i], tail[j] + 1);
        return tail;
    }
    std::vector<std::uint64_t> desc(static_cast<std::size_t>(words * n), 0);
    // hist[k][q]: descendants of kind k with tail q, for the op being processed
    std::array<std::vector<std::int32_t>, OP_KINDS> hist;
    for (auto &h : hist) h.assign(1, 0);
    for (std::int32_t i = n - 1; i >= 0; --i) {
        auto *const mine = desc.data() + static_cast<std::int64_t>(i) * words;
        // Descendants have larger indices, so only words from i / 64 on can be set
        auto const first_word = static_cast<std::int64_t>(i / 64);
        for (auto const j : g.succ.row(i)) {
            auto const *const theirs = desc.data() + static_cast<std::int64_t>(j) * words;
            for (auto w = static_cast<std::int64_t>(j / 64); w < words; ++w) mine[w] |= theirs[w];
            mine[j / 64] |= std::uint64_t{1} << (j % 64);
        }
        std::int32_t top = -1;
        for (auto w = first_word; w < words; ++w) {
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

std::vector<std::int32_t> resource_heads(OpGraph const &g, std::int64_t max_closure_bytes) {
    auto const mirror_tail = resource_tails(reversed(g), max_closure_bytes);
    std::vector<std::int32_t> head(g.n);
    for (std::int32_t i = 0; i < g.n; ++i) head[i] = mirror_tail[g.n - 1 - i] + 1;
    return head;
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
