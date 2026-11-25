#include "ilp.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "gurobi.hpp"

char const *exact_status_name(ExactStatus s) {
    switch (s) {
    case ExactStatus::Optimal: return "optimal";
    case ExactStatus::Bound: return "bound";
    case ExactStatus::TimeLimit: return "timelimit";
    case ExactStatus::TooLarge: return "too_large";
    }
    return "?";
}

Result<ExactResult> exact_schedule(Netlist const &n, Resources const &r, Schedule const &heuristic,
                                   Gurobi &gurobi, IlpOptions const &options, Clock const &clock) {
    ExactResult result;
    result.schedule = heuristic;
    result.lower_bound = lower_bound(n, r);
    auto const ub = heuristic.latency;
    if (ub <= result.lower_bound) {
        result.status = ExactStatus::Bound;
        return result;
    }

    // Each operation can only start within [ASAP, ALAP(ub)]
    auto const early = asap(n);
    auto const late = alap(n, ub);
    for (auto const i : n.topo) result.num_vars += late[i] - early[i] + 1;
    if (result.num_vars > options.max_vars) {
        result.status = ExactStatus::TooLarge;
        return result;
    }

    // Variables: L (index 0), then x(i, t) for t in [early[i], late[i]] at base[i] + t - early[i]
    TRY(auto const latency_var, gurobi.add_vars(1, result.lower_bound, ub, 1, VarType::Integer));
    std::vector<std::int32_t> base(n.size(), -1);
    for (auto const i : n.topo) {
        TRY(base[i], gurobi.add_vars(late[i] - early[i] + 1, 0, 1, 0, VarType::Binary));
    }
    auto const x = [&](NodeId i, std::int32_t t) { return base[i] + (t - early[i]); };

    std::vector<std::int32_t> ind;
    std::vector<double> val;
    auto const clear = [&] {
        ind.clear();
        val.clear();
    };
    // Every operation starts exactly once
    for (auto const i : n.topo) {
        clear();
        for (auto t = early[i]; t <= late[i]; ++t) {
            ind.push_back(x(i, t));
            val.push_back(1);
        }
        TRY_VOID(gurobi.add_constr(ind, val, Sense::Equal, 1));
    }
    // Precedence: start(i) >= start(j) + 1 for every operation edge j -> i
    for (auto const i : n.topo) {
        for (auto const j : n.fanin.row(i)) {
            if (!n.is_op(j) || late[j] < early[i]) continue; // windows already ordered
            clear();
            for (auto t = early[i]; t <= late[i]; ++t) {
                ind.push_back(x(i, t));
                val.push_back(t);
            }
            for (auto t = early[j]; t <= late[j]; ++t) {
                ind.push_back(x(j, t));
                val.push_back(-t);
            }
            TRY_VOID(gurobi.add_constr(ind, val, Sense::GreaterEqual, 1));
        }
    }
    // Resources: at most limit[k] operations of kind k in each step
    std::vector<std::array<std::vector<NodeId>, OP_KINDS>> active(ub + 1);
    for (auto const i : n.topo)
        for (auto t = early[i]; t <= late[i]; ++t) active[t][op_index(n.kind[i])].push_back(i);
    for (std::int32_t t = 1; t <= ub; ++t) {
        for (int k = 0; k < OP_KINDS; ++k) {
            auto const &ops = active[t][k];
            if (static_cast<std::int32_t>(ops.size()) <= r.limit[k]) continue;
            clear();
            for (auto const i : ops) {
                ind.push_back(x(i, t));
                val.push_back(1);
            }
            TRY_VOID(gurobi.add_constr(ind, val, Sense::LessEqual, r.limit[k]));
        }
    }
    // Latency: L >= start(i) for every operation with no operation fanout
    for (auto const i : n.topo) {
        if (!n.fanout.row(i).empty()) continue;
        clear();
        ind.push_back(latency_var);
        val.push_back(1);
        for (auto t = early[i]; t <= late[i]; ++t) {
            ind.push_back(x(i, t));
            val.push_back(-t);
        }
        TRY_VOID(gurobi.add_constr(ind, val, Sense::GreaterEqual, 0));
    }

    // MIP start from the heuristic schedule
    std::vector<double> start(gurobi.num_vars(), 0);
    start[latency_var] = ub;
    for (auto const i : n.topo) start[x(i, heuristic.step[i])] = 1;
    TRY_VOID(gurobi.set_start(start));
    TRY_VOID(gurobi.set_double_param("MIPGap", 0));
    if (!options.keep_dir.empty()) TRY_VOID(gurobi.write(options.keep_dir + "/model.lp"));

    // Leave time to write the schedule
    TRY(auto const solved, gurobi.solve(clock.remaining() - 1.0));
    if (solved.status == SolveStatus::Infeasible)
        return err("Gurobi reports the ILP infeasible, but the heuristic schedule is feasible");
    if (!solved.has_solution) {
        result.status = ExactStatus::TimeLimit; // keep the heuristic schedule
        return result;
    }

    Schedule s;
    s.step.assign(n.size(), 0);
    for (auto const i : n.topo) {
        for (auto t = early[i]; t <= late[i]; ++t)
            if (solved.x[x(i, t)] > 0.5) s.step[i] = t;
        if (s.step[i] == 0) return err("solution misses operation " + n.name[i]);
        s.latency = std::max(s.latency, s.step[i]);
    }
    if (s.latency > heuristic.latency) return result; // never worse than the heuristic
    result.schedule = std::move(s);
    result.status = solved.status == SolveStatus::Optimal ? ExactStatus::Optimal
                                                          : ExactStatus::TimeLimit;
    return result;
}
