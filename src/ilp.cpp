#include "ilp.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
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

static Result<ExactResult> build_and_solve(Netlist const &n, Resources const &r, Schedule const &heuristic,
                                   Gurobi &gurobi, IlpOptions const &options, Clock const &clock) {
    ExactResult result;
    result.schedule = heuristic;
    result.status = ExactStatus::TimeLimit;
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
    if (result.num_vars > std::min<std::int64_t>(options.max_vars, 1'000'000)) {
        result.status = ExactStatus::TooLarge;
        return result;
    }

    // Cap repeated precedence terms as well as binary storage. The old LP-text
    // guard served the same purpose; C API storage depends on nonzeros instead.
    std::int64_t nonzeros = result.num_vars;
    for (auto const i : n.topo) {
        if (clock.remaining() <= 4.0) return result;
        auto const width = late[i] - early[i] + 1;
        nonzeros += width; // resource rows (conservative even when redundant)
        if (n.fanout.row(i).empty()) nonzeros += width + 1;
        for (auto const j : n.fanin.row(i))
            if (n.is_op(j) && late[j] >= early[i])
                nonzeros += width + late[j] - early[j] + 1;
        if (nonzeros > 16'000'000) {
            result.status = ExactStatus::TooLarge;
            return result;
        }
    }
    auto const stop_building = [&] { return clock.remaining() <= 4.0; };
    // Variables: L (index 0), then x(i, t) for t in [early[i], late[i]] at base[i] + t - early[i]
    TRY(auto const latency_var, gurobi.add_vars(1, result.lower_bound, ub, 1, VarType::Integer));
    std::vector<std::int32_t> base(n.size(), -1);
    for (auto const i : n.topo) {
        if (clock.remaining() <= 4.0) return result;
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
        if (clock.remaining() <= 4.0) return result;
        clear();
        for (auto t = early[i]; t <= late[i]; ++t) {
            ind.push_back(x(i, t));
            val.push_back(1);
        }
        TRY_VOID(gurobi.add_constr(ind, val, Sense::Equal, 1));
    }
    // Precedence: start(i) >= start(j) + 1 for every operation edge j -> i
    for (auto const i : n.topo) {
        if (clock.remaining() <= 4.0) return result;
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
    for (auto const i : n.topo) {
        if (stop_building()) return result;
        for (auto t = early[i]; t <= late[i]; ++t) active[t][op_index(n.kind[i])].push_back(i);
    }
    for (std::int32_t t = 1; t <= ub; ++t) {
        if (stop_building()) return result;
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
        if (clock.remaining() <= 4.0) return result;
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

    // Release construction buffers before Gurobi's presolve/root allocations.
    decltype(active){}.swap(active);
    std::vector<double>{}.swap(start);
    std::vector<std::int32_t>{}.swap(ind);
    std::vector<double>{}.swap(val);
    // Leave time to write the schedule
    if (stop_building()) return result;
    auto const solved_result = gurobi.solve(clock.remaining() - 2.5);
    if (!solved_result) return result;
    auto const &solved = solved_result.value();
    if (solved.status == SolveStatus::Infeasible)
        return err("Gurobi reports the ILP infeasible, but the heuristic schedule is feasible");
    if (!solved.has_solution) {
        result.status = ExactStatus::TimeLimit; // keep the heuristic schedule
        return result;
    }

    Schedule s;
    s.step.assign(n.size(), 0);
    for (auto const i : n.topo) {
        for (auto t = early[i]; t <= late[i]; ++t) {
            auto const value = solved.x[x(i, t)];
            if (!std::isfinite(value) ||
                (std::abs(value) > 1e-5 && std::abs(value - 1.0) > 1e-5)) return result;
            if (value > 0.5) {
                if (s.step[i] != 0) return result;
                s.step[i] = t;
            }
        }
        if (s.step[i] == 0) return result;
        s.latency = std::max(s.latency, s.step[i]);
    }
    if (s.latency > heuristic.latency || !validate(n, r, s)) return result; // never worse than the heuristic
    result.schedule = std::move(s);
    result.status = solved.status == SolveStatus::Optimal ? ExactStatus::Optimal
                                                          : ExactStatus::TimeLimit;
    if (result.status == ExactStatus::Optimal) result.lower_bound = result.schedule.latency;
    return result;
}

// Once licensing succeeds, solver/model errors retain the independently validated
// incumbent. They cannot turn an interrupted or ambiguous solve into a proof.
Result<ExactResult> exact_schedule(Netlist const &n, Resources const &r, Schedule const &heuristic,
                                   Gurobi &gurobi, IlpOptions const &options, Clock const &clock) {
    auto solved = build_and_solve(n, r, heuristic, gurobi, options, clock);
    if (solved) return solved;
    std::cerr << "mlrcs: retaining heuristic: " << solved.error().message << '\n';
    ExactResult fallback;
    fallback.schedule = heuristic;
    fallback.lower_bound = lower_bound(n, r);
    fallback.status = ExactStatus::TimeLimit;
    return fallback;
}
