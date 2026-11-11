#include "schedule.hpp"

#include <algorithm>
#include <queue>
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
        if (r.limit[i] > 0) {
            auto const count = n.op_count[i];
            bound = std::max(bound, count / r.limit[i] + (count % r.limit[i] != 0));
        }
    }
    if (n.topo.empty()) return bound;

    // With a horizon H, every operation must lie in [early[i], H - tail[i]],
    // where tail counts the operations strictly after i on a longest path.
    // Dropping precedence but keeping these windows and each kind's capacity
    // is a relaxation: infeasibility proves that H is below the true optimum.
    std::vector<std::int32_t> tail(n.size(), 0);
    for (auto it = n.topo.rbegin(); it != n.topo.rend(); ++it)
        for (auto const j : n.fanout.row(*it)) tail[*it] = std::max(tail[*it], tail[j] + 1);
    std::array<std::vector<NodeId>, OP_KINDS> by_release;
    for (auto const i : n.topo) by_release[op_index(n.kind[i])].push_back(i);
    for (auto &ops : by_release)
        std::sort(ops.begin(), ops.end(), [&](NodeId a, NodeId b) { return early[a] < early[b]; });

    auto const feasible = [&](std::int32_t horizon) {
        for (int k = 0; k < OP_KINDS; ++k) {
            auto const &ops = by_release[k];
            if (ops.empty()) continue;
            if (r.limit[k] <= 0) return false; // normally rejected by check_resources
            // Unit-time jobs with integral releases/deadlines admit an EDF
            // schedule whenever any feasible schedule exists (exchange jobs
            // in the first differing slot). Largest tail = earliest deadline.
            std::priority_queue<std::int32_t> ready;
            std::size_t next = 0;
            std::int32_t t = early[ops.front()];
            while (next < ops.size() || !ready.empty()) {
                if (ready.empty()) t = std::max(t, early[ops[next]]);
                while (next < ops.size() && early[ops[next]] <= t)
                    ready.push(tail[ops[next++]]);
                for (std::int32_t used = 0; used < r.limit[k] && !ready.empty(); ++used) {
                    if (t > horizon - ready.top()) return false;
                    ready.pop();
                }
                if (next == ops.size() && ready.empty()) break;
                ++t;
            }
        }
        return true;
    };

    // Serial topological scheduling is feasible within num_ops() steps for
    // valid resources. Windows only expand with H, so binary search is valid.
    // Cost: O(edges + ops log(ops)^2) time and O(nodes + ops) memory.
    if (feasible(bound)) return bound;
    auto upper = n.num_ops();
    while (bound < upper) {
        auto const middle = bound + (upper - bound) / 2;
        if (feasible(middle)) upper = middle;
        else bound = middle + 1;
    }
    return bound;
}
