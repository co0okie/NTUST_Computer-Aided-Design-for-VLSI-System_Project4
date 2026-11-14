#include "opgraph.hpp"

#include <algorithm>

namespace {

Csr build_csr(std::int32_t rows, std::vector<std::pair<std::int32_t, std::int32_t>> const &edges,
              bool by_first) {
    Csr c;
    c.offset.assign(rows + 1, 0);
    for (auto const &e : edges) ++c.offset[(by_first ? e.first : e.second) + 1];
    for (std::int32_t i = 0; i < rows; ++i) c.offset[i + 1] += c.offset[i];
    c.item.resize(edges.size());
    auto pos = c.offset;
    for (auto const &e : edges) {
        auto const row = by_first ? e.first : e.second;
        c.item[pos[row]++] = by_first ? e.second : e.first;
    }
    return c;
}

} // namespace

OpGraph make_op_graph(Netlist const &n, Resources const &r) {
    OpGraph g;
    g.n = n.num_ops();
    g.limit = r.limit;
    g.count = n.op_count;
    g.node = n.topo;
    g.kind.resize(g.n);
    std::vector<std::int32_t> index(n.size(), -1);
    for (std::int32_t i = 0; i < g.n; ++i) {
        index[n.topo[i]] = i;
        g.kind[i] = static_cast<std::uint8_t>(op_index(n.kind[n.topo[i]]));
    }
    std::vector<std::pair<std::int32_t, std::int32_t>> edges;
    for (std::int32_t i = 0; i < g.n; ++i)
        for (auto const j : n.fanin.row(n.topo[i]))
            if (n.is_op(j)) edges.push_back({index[j], i});
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    g.succ = build_csr(g.n, edges, true);
    g.pred = build_csr(g.n, edges, false);
    return g;
}

OpGraph reversed(OpGraph const &g) {
    OpGraph m;
    m.n = g.n;
    m.limit = g.limit;
    m.count = g.count;
    m.kind.resize(g.n);
    m.node.resize(g.n);
    std::vector<std::pair<std::int32_t, std::int32_t>> edges;
    for (std::int32_t i = 0; i < g.n; ++i) {
        m.kind[g.n - 1 - i] = g.kind[i];
        m.node[g.n - 1 - i] = g.node[i];
        for (auto const j : g.succ.row(i)) edges.push_back({g.n - 1 - j, g.n - 1 - i});
    }
    std::sort(edges.begin(), edges.end());
    m.succ = build_csr(m.n, edges, true);
    m.pred = build_csr(m.n, edges, false);
    return m;
}

std::vector<std::int32_t> mirror_steps(std::vector<std::int32_t> const &step) {
    std::int32_t latency = 0;
    for (auto const s : step) latency = std::max(latency, s);
    std::vector<std::int32_t> out(step.size());
    for (std::size_t i = 0; i < step.size(); ++i) out[step.size() - 1 - i] = latency + 1 - step[i];
    return out;
}

Schedule to_schedule(Netlist const &n, OpGraph const &g, std::vector<std::int32_t> const &step) {
    Schedule s;
    s.step.assign(n.size(), 0);
    for (std::int32_t i = 0; i < g.n; ++i) {
        s.step[g.node[i]] = step[i];
        s.latency = std::max(s.latency, step[i]);
    }
    return s;
}
