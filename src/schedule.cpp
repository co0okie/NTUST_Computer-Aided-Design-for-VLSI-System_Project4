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

namespace {

// Strengthen windows using the work that must occur strictly before/after a
// node. Exact ancestor/descendant sets avoid counting shared paths twice.
void resource_work_windows(Netlist const &n, Resources const &r,
                           std::vector<std::int32_t> &early,
                           std::vector<std::int32_t> &tail) {
    auto const count = static_cast<std::uint64_t>(n.num_ops());
    auto const words = (count + 63) / 64;
    std::uint64_t edges = 0;
    for (auto const i : n.topo)
        for (auto const j : n.fanin.row(i))
            if (n.is_op(j)) ++edges;
    // Keep this optional pass within a fixed memory/work budget; the EDF
    // relaxation remains valid when it is skipped. Reuse the closure matrix.
    auto const bytes = (count + OP_KINDS) * words * sizeof(std::uint64_t) +
                       static_cast<std::uint64_t>(n.size()) * sizeof(std::size_t);
    if (bytes > 64ULL * 1024 * 1024 || (count + edges) * words > 100000000ULL) return;

    std::vector<std::size_t> position(n.size(), 0);
    std::array<std::vector<std::uint64_t>, OP_KINDS> kind_mask;
    for (auto &mask : kind_mask) mask.assign(words, 0);
    for (std::size_t p = 0; p < n.topo.size(); ++p) {
        auto const i = n.topo[p];
        position[i] = p;
        kind_mask[op_index(n.kind[i])][p / 64] |= std::uint64_t{1} << (p % 64);
    }
    std::vector<std::uint64_t> closure(count * words, 0);
    // Portable constant-work popcount, requiring only standard C++17.
    auto const popcount = [](std::uint64_t bits) {
        bits -= (bits >> 1) & 0x5555555555555555ULL;
        bits = (bits & 0x3333333333333333ULL) + ((bits >> 2) & 0x3333333333333333ULL);
        bits = (bits + (bits >> 4)) & 0x0f0f0f0f0f0f0f0fULL;
        return static_cast<std::int32_t>((bits * 0x0101010101010101ULL) >> 56);
    };
    auto const resource_steps = [&](std::size_t row) {
        std::array<std::int32_t, OP_KINDS> totals{};
        for (std::size_t w = 0; w < words; ++w)
            for (int k = 0; k < OP_KINDS; ++k)
                totals[k] += popcount(closure[row + w] & kind_mask[k][w]);
        std::int32_t steps = 0;
        for (int k = 0; k < OP_KINDS; ++k)
            if (r.limit[k] > 0)
                steps = std::max(steps, totals[k] / r.limit[k] + (totals[k] % r.limit[k] != 0));
        return steps;
    };
    auto const merge = [&](std::size_t row, NodeId j) {
        auto const p = position[j];
        auto const other = p * words;
        for (std::size_t w = 0; w < words; ++w) closure[row + w] |= closure[other + w];
        closure[row + p / 64] |= std::uint64_t{1} << (p % 64);
    };

    for (auto const i : n.topo) {
        auto const row = position[i] * words;
        for (auto const j : n.fanin.row(i)) {
            if (!n.is_op(j)) continue;
            merge(row, j);
            early[i] = std::max(early[i], early[j] + 1);
        }
        // Every strict ancestor must finish before i. Per-kind ancestor work
        // therefore occupies at least ceil(work/capacity) preceding slots.
        early[i] = std::max(early[i], resource_steps(row) + 1);
    }
    std::fill(closure.begin(), closure.end(), 0);
    for (auto it = n.topo.rbegin(); it != n.topo.rend(); ++it) {
        auto const i = *it;
        auto const row = position[i] * words;
        for (auto const j : n.fanout.row(i)) {
            merge(row, j);
            tail[i] = std::max(tail[i], tail[j] + 1);
        }
        // Symmetrically, strict descendants require this many slots after i.
        tail[i] = std::max(tail[i], resource_steps(row));
    }
}

} // namespace

std::int32_t lower_bound(Netlist const &n, Resources const &r) {
    auto early = asap(n);
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
    // where tail lower-bounds the number of required slots strictly after i.
    // Dropping precedence but keeping these windows and each kind's capacity
    // is a relaxation: infeasibility proves that H is below the true optimum.
    std::vector<std::int32_t> tail(n.size(), 0);
    for (auto it = n.topo.rbegin(); it != n.topo.rend(); ++it)
        for (auto const j : n.fanout.row(*it)) tail[*it] = std::max(tail[*it], tail[j] + 1);
    resource_work_windows(n, r, early, tail);
    for (auto const i : n.topo) bound = std::max(bound, early[i] + tail[i]);
    std::array<std::vector<NodeId>, OP_KINDS> by_release;
    for (auto const i : n.topo) by_release[op_index(n.kind[i])].push_back(i);
    for (auto &ops : by_release)
        std::sort(ops.begin(), ops.end(), [&](NodeId a, NodeId b) { return early[a] < early[b]; });

    // Comparing the Jackson-style bound on claude@9e7a4cd exposes a useful
    // simplification of our EDF feasibility search: H shifts every deadline
    // equally, so the EDF order does not depend on H. Its max(t + tail) is
    // exactly the smallest feasible horizon. This implementation is original.
    // EDF cost: O(edges + ops log(ops)) time and O(nodes + ops) memory;
    // the optional closure pass above has its own fixed work/memory limits.
    for (int k = 0; k < OP_KINDS; ++k) {
        auto const &ops = by_release[k];
        if (ops.empty()) continue;
        if (r.limit[k] <= 0) return n.num_ops(); // rejected by check_resources
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
                bound = std::max(bound, t + ready.top());
                ready.pop();
            }
            if (next == ops.size() && ready.empty()) break;
            ++t;
        }
    }
    return bound;
}
