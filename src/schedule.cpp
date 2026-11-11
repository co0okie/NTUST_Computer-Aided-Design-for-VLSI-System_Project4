#include "schedule.hpp"

#include <algorithm>
#include <string>

Result<Unit> check_resources(Netlist const &n, Resources const &r) {
    for (auto const k : OP_KIND_LIST) {
        auto const i = op_index(k);
        if (r.limit[i] < 0) return err(std::string("negative ") + kind_name(k) + " constraint");
        if (n.op_count[i] > 0 && r.limit[i] == 0)
            return err(std::string("the circuit has ") + kind_name(k) + " operations but the " +
                       kind_name(k) + " constraint is 0");
    }
    return Unit{};
}

Result<Unit> validate(Netlist const &n, Resources const &r, Schedule const &s) {
    if (static_cast<NodeId>(s.step.size()) != n.size()) return err("schedule has wrong size");
    std::int32_t latency = 0;
    for (NodeId i = 0; i < n.size(); ++i) {
        if (!n.is_op(i)) {
            if (s.step[i] != 0) return err("primary input " + n.name[i] + " is not at step 0");
            continue;
        }
        if (s.step[i] < 1) return err("operation " + n.name[i] + " is not scheduled");
        latency = std::max(latency, s.step[i]);
        for (auto const j : n.fanin.row(i))
            if (s.step[j] >= s.step[i])
                return err("precedence violated: " + n.name[j] + " (step " +
                           std::to_string(s.step[j]) + ") -> " + n.name[i] + " (step " +
                           std::to_string(s.step[i]) + ")");
    }
    if (latency != s.latency)
        return err("latency is " + std::to_string(latency) + ", schedule claims " +
                   std::to_string(s.latency));

    std::vector<std::array<std::int32_t, OP_KINDS>> used(latency + 1);
    for (auto const i : n.topo) {
        auto const k = op_index(n.kind[i]);
        if (++used[s.step[i]][k] > r.limit[k])
            return err(std::string("too many ") + kind_name(n.kind[i]) + " operations at step " +
                       std::to_string(s.step[i]));
    }
    return Unit{};
}

void print_schedule(std::ostream &out, Netlist const &n, Schedule const &s, char const *title) {
    // Bucket operations by (step, kind), in id order (= BLIF file order)
    std::vector<std::array<std::vector<NodeId>, OP_KINDS>> bucket(s.latency + 1);
    for (NodeId i = 0; i < n.size(); ++i)
        if (n.is_op(i)) bucket[s.step[i]][op_index(n.kind[i])].push_back(i);

    std::string line;
    out << title << '\n';
    for (std::int32_t t = 1; t <= s.latency; ++t) {
        line = std::to_string(t) + ":";
        for (auto const &ops : bucket[t]) {
            line += " {";
            for (std::size_t j = 0; j < ops.size(); ++j) {
                if (j > 0) line += ' ';
                line += n.name[ops[j]];
            }
            line += '}';
        }
        out << line << '\n';
    }
    out << "LATENCY: " << s.latency << '\n' << "END" << std::endl;
}

std::vector<std::int32_t> asap(Netlist const &n) {
    std::vector<std::int32_t> t(n.size(), 0);
    for (auto const i : n.topo)
        for (auto const j : n.fanin.row(i)) t[i] = std::max(t[i], t[j] + 1);
    return t;
}

std::vector<std::int32_t> alap(Netlist const &n, std::int32_t latency) {
    std::vector<std::int32_t> t(n.size(), latency);
    for (auto it = n.topo.rbegin(); it != n.topo.rend(); ++it)
        for (auto const j : n.fanout.row(*it)) t[*it] = std::min(t[*it], t[j] - 1);
    for (auto const i : n.inputs) t[i] = 0;
    return t;
}

std::int32_t lower_bound(Netlist const &n, Resources const &r) {
    auto const early = asap(n);
    std::int32_t bound = 0;
    for (auto const i : n.topo) bound = std::max(bound, early[i]);
    for (auto const k : OP_KIND_LIST) {
        auto const i = op_index(k);
        if (r.limit[i] > 0)
            bound = std::max(bound, (n.op_count[i] + r.limit[i] - 1) / r.limit[i]);
    }
    return bound;
}
