#include "ilp.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

#include "gurobi.hpp"

namespace {

// Step formulation of "is there a schedule with latency <= horizon?".
//
// Op i may run in its window [e_i, l_i] = [head_i, horizon - tail_i]. Binary y_{i,t} = 1 iff op i
// runs at step t or earlier; it is fixed to 0 before e_i and to 1 from l_i on, so only
// t in [e_i, l_i - 1] are variables.
//   monotone:    y_{i,t} <= y_{i,t+1}
//   precedence:  y_{i,t} <= y_{j,t-1}            for every edge j -> i
//   resource:    sum_{i of kind k} (y_{i,t} - y_{i,t-1}) <= limit_k   for every step t
struct StepModel {
    std::int32_t horizon = 0;
    std::vector<std::int32_t> early, late;
    std::int64_t num_vars = 0;
    std::int64_t num_nonzeros = 0; // estimate: monotone + precedence rows (2 each) + resource terms
    bool trivially_infeasible = false;
};

StepModel make_step_model(OpGraph const &g, HeuristicResult const &h, std::int32_t horizon) {
    StepModel m;
    m.horizon = horizon;
    m.early = h.head;
    m.late.resize(g.n);
    for (std::int32_t i = 0; i < g.n; ++i) {
        m.late[i] = horizon - h.tail[i];
        if (m.late[i] < m.early[i]) m.trivially_infeasible = true;
        auto const w = std::max(0, m.late[i] - m.early[i]);
        m.num_vars += w;
        m.num_nonzeros += static_cast<std::int64_t>(w) * (2 * static_cast<std::int64_t>(g.pred.row(i).size()) + 4);
    }
    return m;
}

// Rows are handed to Gurobi in batches of about this many nonzeros (bounds the buffer)
constexpr std::size_t BATCH_NONZEROS = 1u << 22;

Result<Unit> flush_if_full(Gurobi &gurobi, RowBatch &rows) {
    if (rows.nonzeros() < BATCH_NONZEROS) return Unit{};
    TRY_VOID(gurobi.add_constrs(rows));
    rows.clear();
    return Unit{};
}

// Builds the step model into `gurobi` (which must hold an empty model). Variable y_{i,t} is
// base[i] + t - early[i]. Returns false if a resource row is infeasible on its own (more ops
// fixed at a step than units).
Result<bool> build_step_model(OpGraph const &g, StepModel const &m, Gurobi &gurobi,
                              std::vector<std::int32_t> &base) {
    base.assign(g.n, 0);
    std::int64_t total = 0;
    for (std::int32_t i = 0; i < g.n; ++i) {
        base[i] = static_cast<std::int32_t>(total);
        total += std::max(0, m.late[i] - m.early[i]);
    }
    if (total > 0) {
        TRY(auto const first, gurobi.add_vars(static_cast<std::int32_t>(total), 0, 1, 0, VarType::Binary));
        if (first != 0) return err("build_step_model: model not empty");
    }
    auto const is_var = [&](std::int32_t i, std::int32_t t) {
        return t >= m.early[i] && t < m.late[i];
    };
    auto const y = [&](std::int32_t i, std::int32_t t) { return base[i] + (t - m.early[i]); };
    RowBatch rows;
    for (std::int32_t i = 0; i < g.n; ++i) {
        for (auto t = m.early[i]; t + 1 < m.late[i]; ++t) {
            rows.term(y(i, t), 1);
            rows.term(y(i, t + 1), -1);
            rows.end_row(Sense::LessEqual, 0);
        }
        for (auto const j : g.pred.row(i)) {
            for (auto t = m.early[i]; t < m.late[i]; ++t) {
                if (t - 1 >= m.late[j]) break; // j is surely done
                rows.term(y(i, t), 1);
                if (is_var(j, t - 1)) {
                    rows.term(y(j, t - 1), -1);
                    rows.end_row(Sense::LessEqual, 0);
                } else {
                    rows.end_row(Sense::Equal, 0); // j cannot be done by t - 1
                }
            }
        }
        TRY_VOID(flush_if_full(gurobi, rows));
    }
    // Resource rows: op i contributes y_{i,t} - y_{i,t-1} for t in [e_i, l_i]
    std::vector<std::array<std::vector<std::int32_t>, OP_KINDS>> active(m.horizon + 2);
    for (std::int32_t i = 0; i < g.n; ++i)
        for (auto t = m.early[i]; t <= m.late[i]; ++t) active[t][g.kind[i]].push_back(i);
    for (std::int32_t t = 1; t <= m.horizon; ++t) {
        for (int k = 0; k < OP_KINDS; ++k) {
            auto const &ops = active[t][k];
            if (static_cast<std::int32_t>(ops.size()) <= g.limit[k]) continue;
            std::int64_t rhs = g.limit[k];
            std::size_t terms = 0;
            for (auto const i : ops) {
                if (is_var(i, t)) rows.term(y(i, t), 1), ++terms;
                else --rhs; // t == l_i: y_{i,t} is 1
                if (is_var(i, t - 1)) rows.term(y(i, t - 1), -1), ++terms;
            }
            if (terms == 0) {
                if (rhs < 0) return false;
                continue;
            }
            rows.end_row(Sense::LessEqual, static_cast<double>(rhs));
        }
        TRY_VOID(flush_if_full(gurobi, rows));
    }
    TRY_VOID(gurobi.add_constrs(rows));
    return true;
}

// Checks precedence, resources and the horizon; the caller validates again on the netlist.
bool feasible(OpGraph const &g, std::vector<std::int32_t> const &step, std::int32_t horizon) {
    std::vector<std::array<std::int32_t, OP_KINDS>> used(horizon + 1);
    for (std::int32_t i = 0; i < g.n; ++i) {
        if (step[i] < 1 || step[i] > horizon) return false;
        for (auto const j : g.pred.row(i))
            if (step[j] >= step[i]) return false;
        if (++used[step[i]][g.kind[i]] > g.limit[g.kind[i]]) return false;
    }
    return true;
}

// --- Checkpoint relaxation of the two ends of the schedule ------------------------------------
//
// How many ops of the bottleneck kind b can a schedule run in its first T steps? Choose
// checkpoints 0 = tau_0 < tau_1 < ... < tau_m = T. Binary z_{i,j} = 1 means "op i has run by
// step tau_j" (z_{i,0} = 0):
//   window:      z_{i,j} = 0 unless head_i <= tau_j
//   monotone:    z_{i,j} <= z_{i,j+1}
//   precedence:  z_{i,j} <= z_{p,j'} for every predecessor p of i, where j' = j - 1 when
//                tau_{j-1} = tau_j - 1 (unit gap) and j' = j otherwise
//   capacity:    sum_{i of kind k} (z_{i,j} - z_{i,j-1}) <= c_k (tau_j - tau_{j-1})
//   maximize     sum_{i of kind b} z_{i,m}
// Validity. Take any feasible schedule s and set z_{i,j} = [s_i <= tau_j].
//   window: head_i (resource-aware head) is a lower bound on s_i.  monotone: tau_j < tau_{j+1}.
//   precedence: s_p <= s_i - 1 <= tau_j - 1, and tau_j - 1 <= tau_{j'} in both cases.
//   capacity: the kind-k ops with tau_{j-1} < s_i <= tau_j share tau_j - tau_{j-1} steps.
// So every schedule is a feasible point whose objective is its number of kind-b ops in steps
// 1..T: the kind-b ops left out of the model cannot run by T (their head, or a chain of
// predecessors through the precedence rule, puts them after T). Hence any upper bound M on the
// optimum (Gurobi's best bound, never its incumbent) gives: every schedule leaves at least
// c_b T - M unit slots of kind b idle in its first T steps. The same model on the mirror graph
// (heads = tail + 1) bounds the idle slots in the last T' steps. Dropping ops that are neither
// of kind b nor ancestors of a modelled kind-b op only drops constraints (a further relaxation).
//
// Experience (aoi_big2, all four limits): the LP relaxation only ever proves the trivial idle slots
// (the ones before the first kind-b head) because fractional z spreads the ancestor sets ("union"
// effect); the integer bound needs branching that does not finish beyond ~10k variables. Hence
// the small time budget and the size cap in EndsOptions.
struct CheckpointModel {
    std::vector<std::int32_t> tau; // tau[0] = 0, ..., tau[m] = T
    std::vector<std::int32_t> first; // per op: first checkpoint with a variable; m + 1 = not in the model
    std::int64_t vars = 0;
};

std::string one_decimal(double x) {
    char text[32];
    std::snprintf(text, sizeof text, "%.1f", x);
    return text;
}

// Checkpoint j' that a predecessor must have reached when its successor has reached j
std::int32_t pred_checkpoint(std::vector<std::int32_t> const &tau, std::int32_t j) {
    return (j >= 1 && tau[j - 1] == tau[j] - 1) ? j - 1 : j;
}

CheckpointModel make_checkpoint_model(OpGraph const &g, std::vector<std::int32_t> const &head,
                                      int b, std::vector<std::int32_t> tau) {
    CheckpointModel cm;
    cm.tau = std::move(tau);
    auto const m = static_cast<std::int32_t>(cm.tau.size()) - 1;
    auto const T = cm.tau[m];
    // Earliest checkpoint of each op, in topological order: its head, and every predecessor
    // must be able to reach checkpoint pred_checkpoint(j)
    cm.first.assign(g.n, m + 1);
    for (std::int32_t i = 0; i < g.n; ++i) {
        if (head[i] > T) continue;
        std::int32_t e = 1;
        while (cm.tau[e] < head[i]) ++e;
        bool possible = true;
        for (auto const p : g.pred.row(i)) {
            auto const ep = cm.first[p];
            if (ep > m) {
                possible = false;
                break;
            }
            auto j = std::max(e, ep);
            if (pred_checkpoint(cm.tau, j) < ep) ++j;
            e = j;
        }
        if (possible && e <= m) cm.first[i] = e;
    }
    // Keep only kind-b ops and their ancestors
    std::vector<char> useful(g.n, 0);
    for (std::int32_t i = g.n - 1; i >= 0; --i) {
        if (cm.first[i] > m) continue;
        if (g.kind[i] == b) useful[i] = 1;
        if (!useful[i]) {
            for (auto const s : g.succ.row(i))
                if (useful[s]) {
                    useful[i] = 1;
                    break;
                }
        }
        if (!useful[i]) cm.first[i] = m + 1;
    }
    for (std::int32_t i = 0; i < g.n; ++i)
        if (cm.first[i] <= m) cm.vars += m - cm.first[i] + 1;
    return cm;
}

// Builds the checkpoint model into `gurobi` (empty model), as a minimization of
// -sum z_{i,m} over the kind-b ops. Variable z_{i,j} is base[i] + j - first[i].
Result<Unit> build_checkpoint_model(OpGraph const &g, CheckpointModel const &cm, int b,
                                    Gurobi &gurobi, std::vector<std::int32_t> &base) {
    auto const m = static_cast<std::int32_t>(cm.tau.size()) - 1;
    base.assign(g.n, -1);
    for (std::int32_t i = 0; i < g.n; ++i) {
        if (cm.first[i] > m) continue;
        auto const count = m - cm.first[i] + 1;
        if (g.kind[i] == b) {
            // z_{i,first..m-1} (no objective), then z_{i,m} (objective -1), consecutive
            TRY(base[i], gurobi.add_vars(count - 1, 0, 1, 0, VarType::Binary));
            TRY(auto const last, gurobi.add_vars(1, 0, 1, -1, VarType::Binary));
            if (last != base[i] + count - 1) return err("build_checkpoint_model: variable order");
        } else {
            TRY(base[i], gurobi.add_vars(count, 0, 1, 0, VarType::Binary));
        }
    }
    auto const z = [&](std::int32_t i, std::int32_t j) { return base[i] + (j - cm.first[i]); };
    RowBatch rows;
    for (std::int32_t i = 0; i < g.n; ++i) {
        if (cm.first[i] > m) continue;
        for (auto j = cm.first[i]; j < m; ++j) {
            rows.term(z(i, j), 1);
            rows.term(z(i, j + 1), -1);
            rows.end_row(Sense::LessEqual, 0);
        }
        for (auto const p : g.pred.row(i))
            for (auto j = cm.first[i]; j <= m; ++j) {
                rows.term(z(i, j), 1);
                rows.term(z(p, pred_checkpoint(cm.tau, j)), -1);
                rows.end_row(Sense::LessEqual, 0);
            }
    }
    for (std::int32_t j = 1; j <= m; ++j) {
        for (int k = 0; k < OP_KINDS; ++k) {
            std::int64_t ops = 0;
            for (std::int32_t i = 0; i < g.n; ++i)
                if (g.kind[i] == k && cm.first[i] <= j) ++ops;
            auto const rhs = static_cast<std::int64_t>(g.limit[k]) * (cm.tau[j] - cm.tau[j - 1]);
            if (ops <= rhs) continue;
            for (std::int32_t i = 0; i < g.n; ++i) {
                if (g.kind[i] != k || cm.first[i] > j) continue;
                rows.term(z(i, j), 1);
                if (cm.first[i] <= j - 1) rows.term(z(i, j - 1), -1);
            }
            rows.end_row(Sense::LessEqual, static_cast<double>(rhs));
        }
    }
    return gurobi.add_constrs(rows);
}

struct CheckpointResult {
    bool solved = false;           // `bound` is a proven upper bound
    std::int64_t bound = 0;        // kind-b ops that any schedule can run by step T
    std::vector<std::int32_t> reached; // per op: first checkpoint with z = 1 in the incumbent, 0 = none
    double seconds = 0;
};

Result<CheckpointResult> solve_checkpoint_model(OpGraph const &g, CheckpointModel const &cm,
                                                int b, std::string const &name, Gurobi &gurobi,
                                                IlpOptions const &options, double time_limit) {
    CheckpointResult r;
    auto const m = static_cast<std::int32_t>(cm.tau.size()) - 1;
    if (cm.vars == 0) { // no kind-b op can run by T
        r.solved = true;
        return r;
    }
    Clock const timer;
    TRY_VOID(gurobi.reset_model());
    std::vector<std::int32_t> base;
    TRY_VOID(build_checkpoint_model(g, cm, b, gurobi, base));
    TRY_VOID(gurobi.set_double_param("MIPGap", 0));
    TRY_VOID(gurobi.set_double_param("MIPGapAbs", 0.999));
    if (!options.keep_dir.empty()) TRY_VOID(gurobi.write(options.keep_dir + "/" + name + ".lp"));
    auto const left = std::max(0.5, time_limit - timer.elapsed());
    TRY(auto const s, gurobi.solve(left));
    r.seconds = timer.elapsed();
    if (s.has_bound) {
        // Maximum = -(minimum of the negated objective). The objective is integral; a tiny
        // tolerance only ever rounds the bound up
        r.bound = static_cast<std::int64_t>(std::floor(-s.bound + 1e-6));
        r.solved = true;
    }
    if (s.has_solution) {
        r.reached.assign(g.n, 0);
        for (std::int32_t i = 0; i < g.n; ++i)
            for (auto j = cm.first[i]; j <= m; ++j)
                if (s.x[base[i] + (j - cm.first[i])] > 0.5) {
                    r.reached[i] = j;
                    break;
                }
    }
    return r;
}

// Checkpoints for the first T steps. m >= T: one per step (the exact prefix problem, given the
// heads). Otherwise kind-b ops cannot run before `first_b`, so one checkpoint at first_b - 1,
// then m intervals up to T, denser early when gamma > 1.
std::vector<std::int32_t> make_checkpoints(std::int32_t T, std::int32_t first_b, std::int32_t m,
                                           double gamma) {
    std::vector<std::int32_t> tau{0};
    auto const s = m >= T ? 0 : std::max(0, std::min(first_b - 1, T - 1));
    if (s > 0) tau.push_back(s);
    for (std::int32_t j = 1; j <= m; ++j) {
        auto const x = std::pow(static_cast<double>(j) / m, gamma);
        auto const t = s + static_cast<std::int32_t>(std::lround(x * (T - s)));
        if (t > tau.back()) tau.push_back(t);
    }
    if (tau.back() != T) tau.push_back(T);
    return tau;
}

int bottleneck_kind(OpGraph const &g) {
    int b = 0;
    std::int64_t load = -1;
    for (int k = 0; k < OP_KINDS; ++k) {
        if (g.count[k] == 0) continue;
        auto const l = (static_cast<std::int64_t>(g.count[k]) + g.limit[k] - 1) / g.limit[k];
        if (l > load) load = l, b = k;
    }
    return b;
}

// Lower bound from the two ends of the schedule, for instances too large for the full model:
// kind b needs ceil(N_b / c_b) steps if its units never idle; the checkpoint relaxation proves
// idle_head idle slots in the first T steps and idle_tail in the last T' steps, so
//   latency >= ceil((N_b + idle_head + idle_tail) / c_b)        (valid since T + T' <= old LB).
// The relaxations' incumbents also order the ops for a few guided list-scheduling passes.
void ends_relaxation(OpGraph const &g, HeuristicResult const &h, Gurobi &gurobi,
                     IlpOptions const &options, Clock const &clock, ExactResult &result) {
    auto const &eo = options.ends;
    auto const b = bottleneck_kind(g);
    auto const c = static_cast<std::int64_t>(g.limit[b]);
    if (g.count[b] == 0 || c <= 0) return;
    // Spans where the current schedule's kind-b units idle near the start and near the end
    auto const L = result.latency;
    std::vector<std::int32_t> used(L + 2, 0);
    for (std::int32_t i = 0; i < g.n; ++i)
        if (g.kind[i] == b) ++used[result.step[i]];
    std::int32_t head_span = 0, tail_span = 0;
    for (std::int32_t t = 1; t <= L; ++t) {
        if (used[t] >= c) continue;
        if (t <= L / 2) head_span = t;
        else if (tail_span == 0) tail_span = L + 1 - t;
    }
    std::array<std::int32_t, 2> span{eo.head_span > 0 ? eo.head_span : head_span + eo.extra,
                                     eo.tail_span > 0 ? eo.tail_span : tail_span + eo.extra};
    if (head_span == 0 && eo.head_span == 0) span[0] = 0;
    if (tail_span == 0 && eo.tail_span == 0) span[1] = 0;
    if (span[0] + span[1] > result.lower_bound) return; // the two ends would overlap

    auto const mirror = reversed(g);
    std::vector<std::int32_t> mirror_head(g.n);
    for (std::int32_t i = 0; i < g.n; ++i) mirror_head[g.n - 1 - i] = h.tail[i] + 1;

    auto const deadline = clock.elapsed() + eo.time;
    std::array<std::int64_t, 2> idle{0, 0};
    std::array<std::vector<std::int32_t>, 2> reached;
    std::array<std::int32_t, 2> checkpoints{0, 0};
    for (int side = 0; side < 2; ++side) {
        if (span[side] <= 0) continue;
        auto const &graph = side == 0 ? g : mirror;
        auto const &head = side == 0 ? h.head : mirror_head;
        std::int32_t first_b = span[side] + 1;
        for (std::int32_t i = 0; i < g.n; ++i)
            if (graph.kind[i] == b) first_b = std::min(first_b, head[i]);
        auto const intervals =
            eo.checkpoints > 0 ? eo.checkpoints : (span[side] <= 10 ? span[side] : 8);
        auto const tau = make_checkpoints(span[side], first_b, intervals, eo.gamma);
        auto const cm = make_checkpoint_model(graph, head, b, tau);
        auto const m = static_cast<std::int32_t>(tau.size()) - 1;
        // Remaining budget: the head side gets at most 60% of it unless the tail side is empty
        auto budget = std::min(deadline, clock.budget - 8.0) - clock.elapsed();
        if (side == 0 && span[1] > 0) budget *= 0.6;
        std::string note = std::string(side == 0 ? "head" : "tail") + ":T" +
                           std::to_string(span[side]) + "m" + std::to_string(m) + "v" +
                           std::to_string(cm.vars);
        if ((cm.vars > eo.max_vars && !eo.force) || budget < 1.0) {
            result.note += (result.note.empty() ? "" : ",") + note + "/skip";
            continue;
        }
        ++result.solves;
        auto const solved = solve_checkpoint_model(graph, cm, b, side == 0 ? "head" : "tail",
                                                   gurobi, options, budget);
        // Solver trouble: no bound from this side (the schedule is unaffected)
        auto const r = solved ? solved.value() : CheckpointResult{};
        if (!solved) result.note += (result.note.empty() ? "" : ",") + std::string("gurobi_failed");
        auto const slots = c * span[side];
        if (r.solved) idle[side] = std::max<std::int64_t>(0, slots - r.bound);
        note += "/" + (r.solved ? std::to_string(slots - std::min(slots, r.bound)) : std::string("x")) +
                "/" + one_decimal(r.seconds) + "s";
        result.note += (result.note.empty() ? "" : ",") + note;
        reached[side] = r.reached;
        checkpoints[side] = m;
    }
    auto const bound = (g.count[b] + idle[0] + idle[1] + c - 1) / c;
    result.ends_bound = static_cast<std::int32_t>(bound);
    result.lower_bound = std::max(result.lower_bound, result.ends_bound);

    // Guided schedules: ops the relaxation ran at earlier checkpoints get higher priority
    if (!eo.guide) return;
    for (int side = 0; side < 2 && result.latency > result.lower_bound; ++side) {
        if (reached[side].empty()) continue;
        auto const m = checkpoints[side];
        std::int64_t w = 1;
        for (auto const t : h.tail) w = std::max<std::int64_t>(w, t + 1);
        for (auto const t : h.head) w = std::max<std::int64_t>(w, t + 1);
        std::vector<std::int64_t> key(g.n);
        for (std::int32_t i = 0; i < g.n; ++i) {
            // Tie-break by the resource tail of the graph being scheduled
            auto const tail = side == 0 ? h.tail[i] : h.head[g.n - 1 - i] - 1;
            auto const r = reached[side][i];
            key[i] = (r > 0 ? m + 1 - r : 0) * w + tail;
        }
        HeuristicResult cur = h;
        cur.step = result.step;
        cur.latency = result.latency;
        cur.lower_bound = result.lower_bound;
        guided_schedule(g, cur, key, side == 1, clock, clock.elapsed() + 1.5);
        if (cur.latency < result.latency) {
            result.step = cur.step;
            result.latency = cur.latency;
            result.note += side == 0 ? ",guided_head" : ",guided_tail";
        }
    }
}

} // namespace

char const *exact_status_name(ExactStatus s) {
    switch (s) {
    case ExactStatus::Optimal: return "optimal";
    case ExactStatus::Bound: return "bound";
    case ExactStatus::TimeLimit: return "timelimit";
    case ExactStatus::TooLarge: return "too_large";
    }
    return "?";
}

ExactResult exact_schedule(OpGraph const &g, HeuristicResult const &heuristic, Gurobi &gurobi,
                           IlpOptions const &options, Clock const &clock) {
    ExactResult result;
    result.step = heuristic.step;
    result.latency = heuristic.latency;
    result.lower_bound = heuristic.lower_bound;
    if (result.latency <= result.lower_bound && !options.ends.force) {
        result.status = ExactStatus::Bound;
        return result;
    }
    if (options.ends.force) { // test hook: only the end relaxations
        ends_relaxation(g, heuristic, gurobi, options, clock, result);
        result.status = result.latency <= result.lower_bound ? ExactStatus::Bound
                                                             : ExactStatus::TimeLimit;
        return result;
    }

    auto const give_up = [&](char const *note) {
        result.status = ExactStatus::TimeLimit;
        result.note = note;
        return result;
    };
    while (result.latency > result.lower_bound) {
        auto const horizon = result.latency - 1;
        auto const model = make_step_model(g, heuristic, horizon);
        if (model.trivially_infeasible) {
            result.lower_bound = result.latency;
            break;
        }
        result.num_vars = model.num_vars;
        result.num_nonzeros = model.num_nonzeros;
        if (model.num_vars > options.max_vars || model.num_nonzeros > options.max_nonzeros) {
            ends_relaxation(g, heuristic, gurobi, options, clock, result);
            if (result.latency <= result.lower_bound) break;
            result.status = ExactStatus::TooLarge;
            return result;
        }
        if (clock.remaining() < 4.0) {
            result.status = ExactStatus::TimeLimit;
            return result;
        }
        Clock const build_timer;
        if (!gurobi.reset_model()) return give_up("gurobi_failed");
        std::vector<std::int32_t> base;
        auto const built = build_step_model(g, model, gurobi, base);
        if (!built) return give_up("gurobi_failed");
        result.build_seconds += build_timer.elapsed();
        if (!built.value()) { // a step needs more units than there are
            result.lower_bound = result.latency;
            break;
        }
        // Barrier + aggressive presolve: see notes (C1355:3,2,2)
        if (!gurobi.set_int_param("DualReductions", 0) || !gurobi.set_int_param("Method", 2) ||
            !gurobi.set_int_param("Presolve", 2))
            return give_up("gurobi_failed");
        if (!options.keep_dir.empty())
            (void)gurobi.write(options.keep_dir + "/model" + std::to_string(horizon) + ".lp");
        ++result.solves;
        auto const solved = gurobi.solve(std::max(1.0, clock.remaining() - 3.0));
        if (!solved) return give_up("gurobi_failed");
        auto const &s = solved.value();
        if (s.status == SolveStatus::Infeasible) {
            result.lower_bound = result.latency;
            break;
        }
        // A model without variables has the single solution "every op at its latest step"
        bool const all_fixed = model.num_vars == 0 && s.status == SolveStatus::Optimal;
        if (!s.has_solution && !all_fixed) {
            result.status = ExactStatus::TimeLimit;
            return result;
        }
        std::vector<std::int32_t> step(g.n);
        for (std::int32_t i = 0; i < g.n; ++i) {
            step[i] = model.late[i];
            if (all_fixed) continue;
            for (auto t = model.early[i]; t < model.late[i]; ++t)
                if (s.x[base[i] + (t - model.early[i])] > 0.5) {
                    step[i] = t;
                    break;
                }
        }
        if (!feasible(g, step, horizon)) return give_up("bad_solution");
        result.step = std::move(step);
        result.latency = 0;
        for (auto const t : result.step) result.latency = std::max(result.latency, t);
    }
    result.status = ExactStatus::Optimal;
    return result;
}
