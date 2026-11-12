#include "ilp.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>

#include "gurobi.hpp"

namespace {

std::string y(std::int32_t i, std::int32_t t) {
    return "y" + std::to_string(i) + "_" + std::to_string(t);
}

Result<Unit> write_file(std::string const &path, std::string const &text) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return err("cannot write " + path);
    out << text;
    if (!out) return err("cannot write " + path);
    return Unit{};
}

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
        m.num_vars += std::max(0, m.late[i] - m.early[i]);
    }
    return m;
}

std::string write_step_model(OpGraph const &g, StepModel const &m) {
    std::string lp = "\\ ML-RCS step formulation, horizon " + std::to_string(m.horizon) +
                     "\nMinimize\n 0 dummy\nSubject To\n";
    auto const is_var = [&](std::int32_t i, std::int32_t t) {
        return t >= m.early[i] && t < m.late[i];
    };
    for (std::int32_t i = 0; i < g.n; ++i) {
        for (auto t = m.early[i]; t + 1 < m.late[i]; ++t)
            lp += " " + y(i, t) + " - " + y(i, t + 1) + " <= 0\n";
        for (auto const j : g.pred.row(i)) {
            for (auto t = m.early[i]; t < m.late[i]; ++t) {
                if (t - 1 >= m.late[j]) break; // j is surely done
                if (is_var(j, t - 1)) lp += " " + y(i, t) + " - " + y(j, t - 1) + " <= 0\n";
                else lp += " " + y(i, t) + " = 0\n"; // j cannot be done by t - 1
            }
        }
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
            int terms = 0;
            lp += " r" + std::to_string(k) + "_" + std::to_string(t) + ":";
            for (auto const i : ops) {
                if (is_var(i, t)) lp += " + " + y(i, t), ++terms;
                else --rhs; // t == l_i: y_{i,t} is 1
                if (is_var(i, t - 1)) lp += " - " + y(i, t - 1), ++terms;
                if (terms >= 8) lp += "\n ", terms = 0;
            }
            lp += " <= " + std::to_string(rhs) + "\n";
        }
    }
    lp += "Bounds\n dummy = 0\nBinaries\n";
    for (std::int32_t i = 0; i < g.n; ++i)
        for (auto t = m.early[i]; t < m.late[i]; ++t) lp += " " + y(i, t) + "\n";
    lp += "End\n";
    return lp;
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
// (heads = tail + 1) bounds the idle slots in the last T' steps.
struct CheckpointModel {
    std::vector<std::int32_t> tau; // tau[0] = 0, ..., tau[m] = T
    std::vector<std::int32_t> first; // per op: first checkpoint with a variable; m + 1 = not in the model
    std::int64_t vars = 0, rows = 0, nonzeros = 0;
};

std::string one_decimal(double x) {
    char text[32];
    std::snprintf(text, sizeof text, "%.1f", x);
    return text;
}

std::string z(std::int32_t i, std::int32_t j) {
    return "z" + std::to_string(i) + "_" + std::to_string(j);
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
    for (std::int32_t i = 0; i < g.n; ++i) {
        if (cm.first[i] > m) continue;
        auto const v = m - cm.first[i] + 1;
        auto const preds = static_cast<std::int64_t>(g.pred.row(i).size());
        cm.vars += v;
        cm.rows += (v - 1) + v * preds;
        cm.nonzeros += 2 * ((v - 1) + v * preds) + 2 * v;
    }
    return cm;
}

std::string write_checkpoint_model(OpGraph const &g, CheckpointModel const &cm, int b) {
    auto const m = static_cast<std::int32_t>(cm.tau.size()) - 1;
    std::string lp = "\\ ML-RCS checkpoint relaxation, T = " + std::to_string(cm.tau[m]) +
                     ", m = " + std::to_string(m) + "\nMaximize\n";
    int terms = 0;
    for (std::int32_t i = 0; i < g.n; ++i) {
        if (cm.first[i] > m || g.kind[i] != b) continue;
        lp += " + " + z(i, m);
        if (++terms % 8 == 0) lp += "\n";
    }
    lp += "\nSubject To\n";
    for (std::int32_t i = 0; i < g.n; ++i) {
        if (cm.first[i] > m) continue;
        for (auto j = cm.first[i]; j < m; ++j) lp += " " + z(i, j) + " - " + z(i, j + 1) + " <= 0\n";
        for (auto const p : g.pred.row(i))
            for (auto j = cm.first[i]; j <= m; ++j)
                lp += " " + z(i, j) + " - " + z(p, pred_checkpoint(cm.tau, j)) + " <= 0\n";
    }
    std::vector<std::int32_t> ops;
    for (std::int32_t j = 1; j <= m; ++j) {
        for (int k = 0; k < OP_KINDS; ++k) {
            ops.clear();
            for (std::int32_t i = 0; i < g.n; ++i)
                if (g.kind[i] == k && cm.first[i] <= j) ops.push_back(i);
            auto const rhs = static_cast<std::int64_t>(g.limit[k]) * (cm.tau[j] - cm.tau[j - 1]);
            if (static_cast<std::int64_t>(ops.size()) <= rhs) continue;
            terms = 0;
            lp += " c" + std::to_string(k) + "_" + std::to_string(j) + ":";
            for (auto const i : ops) {
                lp += " + " + z(i, j), ++terms;
                if (cm.first[i] <= j - 1) lp += " - " + z(i, j - 1), ++terms;
                if (terms >= 8) lp += "\n ", terms = 0;
            }
            lp += " <= " + std::to_string(rhs) + "\n";
        }
    }
    lp += "Binaries\n";
    for (std::int32_t i = 0; i < g.n; ++i)
        for (auto j = cm.first[i]; j <= m; ++j) lp += " " + z(i, j) + "\n";
    lp += "End\n";
    return lp;
}

struct CheckpointResult {
    bool solved = false;           // `bound` is a proven upper bound
    std::int64_t bound = 0;        // kind-b ops that any schedule can run by step T
    std::vector<std::int32_t> reached; // per op: first checkpoint with z = 1 in the incumbent, 0 = none
    double seconds = 0;
};

CheckpointResult solve_checkpoint_model(OpGraph const &g, CheckpointModel const &cm, int b,
                                        std::string const &base, IlpOptions const &options,
                                        double time_limit) {
    CheckpointResult r;
    auto const m = static_cast<std::int32_t>(cm.tau.size()) - 1;
    if (cm.vars == 0) { // no kind-b op can run by T
        r.solved = true;
        return r;
    }
    GurobiJob job;
    job.model_file = base + ".lp";
    job.result_file = base + ".sol";
    job.log_file = base + ".log";
    Clock const timer;
    if (!write_file(job.model_file, write_checkpoint_model(g, cm, b))) return r;
    auto const left = std::max(0.5, time_limit - timer.elapsed());
    job.params = {{"LogFile", ""},
                  {"TimeLimit", std::to_string(left)},
                  {"MIPGap", "0"},
                  {"MIPGapAbs", "0.999"}};
    if (options.threads > 0) job.params.push_back({"Threads", std::to_string(options.threads)});
    job.interrupt_after = left + 0.5;
    job.kill_after = left + 1.5;
    auto const solved = run_gurobi(options.gurobi_cl, job);
    r.seconds = timer.elapsed();
    if (!solved) return r;
    auto const &s = solved.value();
    if (s.has_bound) {
        // The objective is integral; a tiny tolerance only ever rounds the bound up
        r.bound = static_cast<std::int64_t>(std::floor(s.best_bound + 1e-6));
        r.solved = true;
    }
    if (s.has_solution) {
        r.reached.assign(g.n, 0);
        for (std::int32_t i = 0; i < g.n; ++i)
            for (auto j = cm.first[i]; j <= m; ++j) {
                auto const it = s.value.find(z(i, j));
                if (it != s.value.end() && it->second > 0.5) {
                    r.reached[i] = j;
                    break;
                }
            }
    }
    return r;
}

// Checkpoints for the first T steps: kind-b ops cannot run before `first_b`, so one checkpoint
// at first_b - 1, then m intervals up to T, denser early when gamma > 1.
std::vector<std::int32_t> make_checkpoints(std::int32_t T, std::int32_t first_b, std::int32_t m,
                                           double gamma) {
    std::vector<std::int32_t> tau{0};
    auto const s = std::max(0, std::min(first_b - 1, T - 1));
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
void ends_relaxation(OpGraph const &g, HeuristicResult const &h, ScratchDir const &dir,
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
        auto const tau = make_checkpoints(span[side], first_b, eo.checkpoints, eo.gamma);
        auto const cm = make_checkpoint_model(graph, head, b, tau);
        auto const m = static_cast<std::int32_t>(tau.size()) - 1;
        // Remaining budget: the head side gets at most 60% of it unless the tail side is empty
        auto budget = std::min(deadline, clock.budget - 8.0) - clock.elapsed();
        if (side == 0 && span[1] > 0) budget *= 0.6;
        std::string note = std::string(side == 0 ? "head" : "tail") + ":T" +
                           std::to_string(span[side]) + "m" + std::to_string(m) + "v" +
                           std::to_string(cm.vars);
        if (cm.nonzeros > eo.max_nonzeros || budget < 1.0) {
            result.note += (result.note.empty() ? "" : ",") + note + "/skip";
            continue;
        }
        ++result.solves;
        auto const r = solve_checkpoint_model(graph, cm, b, dir.file(side == 0 ? "head" : "tail"),
                                              options, budget);
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

ExactResult exact_schedule(OpGraph const &g, HeuristicResult const &heuristic,
                           IlpOptions const &options, Clock const &clock) {
    ExactResult result;
    result.step = heuristic.step;
    result.latency = heuristic.latency;
    result.lower_bound = heuristic.lower_bound;
    if (result.latency <= result.lower_bound && !options.ends.force) {
        result.status = ExactStatus::Bound;
        return result;
    }
    auto scratch = ScratchDir::create(options.keep_dir);
    if (!scratch) {
        result.status = ExactStatus::TimeLimit;
        result.note = "no_scratch_dir";
        return result;
    }
    auto const &dir = scratch.value();
    if (options.ends.force) { // test hook: only the end relaxations
        ends_relaxation(g, heuristic, dir, options, clock, result);
        result.status = result.latency <= result.lower_bound ? ExactStatus::Bound
                                                             : ExactStatus::TimeLimit;
        return result;
    }

    while (result.latency > result.lower_bound) {
        auto const horizon = result.latency - 1;
        auto const model = make_step_model(g, heuristic, horizon);
        if (model.trivially_infeasible) {
            result.lower_bound = result.latency;
            break;
        }
        result.num_vars = model.num_vars;
        if (model.num_vars > options.max_vars) {
            ends_relaxation(g, heuristic, dir, options, clock, result);
            if (result.latency <= result.lower_bound) break;
            result.status = ExactStatus::TooLarge;
            return result;
        }
        auto const remaining = clock.remaining();
        if (remaining < 4.0) {
            result.status = ExactStatus::TimeLimit;
            return result;
        }
        GurobiJob job;
        auto const tag = std::to_string(horizon);
        job.model_file = dir.file("model" + tag + ".lp");
        job.result_file = dir.file("model" + tag + ".sol");
        job.log_file = dir.file("gurobi" + tag + ".log");
        if (!write_file(job.model_file, write_step_model(g, model))) {
            result.status = ExactStatus::TimeLimit;
            result.note = "cannot_write_model";
            return result;
        }
        auto const left = clock.remaining();
        job.params = {{"LogFile", ""},
                      {"TimeLimit", std::to_string(std::max(1.0, left - 3.0))},
                      {"DualReductions", "0"},
                      {"Method", "2"},
                      {"Presolve", "2"}}; // barrier + aggressive presolve: see notes (C1355:3,2,2)
        if (options.threads > 0) job.params.push_back({"Threads", std::to_string(options.threads)});
        job.interrupt_after = std::max(1.0, left - 2.0);
        job.kill_after = std::max(1.5, left - 1.0);
        ++result.solves;
        auto const solved = run_gurobi(options.gurobi_cl, job);
        if (!solved) {
            result.status = ExactStatus::TimeLimit;
            result.note = "gurobi_failed";
            return result;
        }
        auto const &s = solved.value();
        if (s.status == SolveStatus::Infeasible) {
            result.lower_bound = result.latency;
            break;
        }
        if (!s.has_solution) {
            result.status = ExactStatus::TimeLimit;
            return result;
        }
        std::vector<std::int32_t> step(g.n);
        for (std::int32_t i = 0; i < g.n; ++i) {
            step[i] = model.late[i];
            for (auto t = model.early[i]; t < model.late[i]; ++t) {
                auto const it = s.value.find(y(i, t));
                if (it != s.value.end() && it->second > 0.5) {
                    step[i] = t;
                    break;
                }
            }
        }
        if (!feasible(g, step, horizon)) {
            result.status = ExactStatus::TimeLimit;
            result.note = "bad_solution";
            return result;
        }
        result.step = std::move(step);
        result.latency = 0;
        for (auto const t : result.step) result.latency = std::max(result.latency, t);
    }
    result.status = ExactStatus::Optimal;
    return result;
}
