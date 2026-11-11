#include "ilp.hpp"

#include <algorithm>
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

// --- Prefix relaxation --------------------------------------------------------------------
//
// "How many ops of kind b can run in the first T steps?" Only the kind-b ops with head <= T and
// their ancestors matter. Same step variables as above, but nothing is forced to run:
//   maximize sum_{i of kind b} y_{i,T}
// Any schedule, cut after step T, is feasible here, so the optimum M (or Gurobi's bound on it)
// bounds the kind-b ops in the first T steps of every schedule: at least limit_b * T - M unit
// slots of kind b stay idle there. Applied to the mirror problem it bounds the last T steps.
struct Prefix {
    bool solved = false;      // Gurobi returned a bound
    std::int64_t bound = 0;   // upper bound on the number of kind-b ops in the first T steps
    std::int64_t vars = 0;
    std::vector<std::int32_t> step; // per op: step in Gurobi's solution, 0 if not in the prefix
};

Prefix solve_prefix(OpGraph const &g, std::vector<std::int32_t> const &head, int b,
                    std::int32_t horizon, std::string const &base, IlpOptions const &options,
                    std::int64_t max_vars, double time_limit) {
    Prefix result;
    // Relevant ops: kind b with head <= horizon, and all their ancestors
    std::vector<char> in(g.n, 0);
    for (std::int32_t i = g.n - 1; i >= 0; --i) {
        if (g.kind[i] == b && head[i] <= horizon) in[i] = 1;
        if (in[i])
            for (auto const j : g.pred.row(i)) in[j] = 1;
    }
    auto const last = [&](std::int32_t i) { return g.kind[i] == b ? horizon : horizon - 1; };
    for (std::int32_t i = 0; i < g.n; ++i)
        if (in[i]) result.vars += std::max(0, last(i) - head[i] + 1);
    if (result.vars > max_vars || result.vars == 0) return result;

    auto const is_var = [&](std::int32_t i, std::int32_t t) {
        return in[i] && t >= head[i] && t <= last(i);
    };
    std::string lp = "\\ ML-RCS prefix relaxation, horizon " + std::to_string(horizon) +
                     "\nMaximize\n";
    int terms = 0;
    for (std::int32_t i = 0; i < g.n; ++i) {
        if (!in[i] || g.kind[i] != b) continue;
        lp += " + " + y(i, horizon);
        if (++terms % 8 == 0) lp += "\n";
    }
    lp += "\nSubject To\n";
    for (std::int32_t i = 0; i < g.n; ++i) {
        if (!in[i]) continue;
        for (auto t = head[i]; t < last(i); ++t)
            lp += " " + y(i, t) + " - " + y(i, t + 1) + " <= 0\n";
        for (auto const j : g.pred.row(i))
            for (auto t = head[i]; t <= last(i); ++t) {
                // After last(j) = horizon - 1 only kind-b ops remain, at step horizon
                auto const tj = std::min(t - 1, last(j));
                lp += " " + y(i, t) + " - " + y(j, tj) + " <= 0\n";
            }
    }
    std::vector<std::array<std::vector<std::int32_t>, OP_KINDS>> active(horizon + 1);
    for (std::int32_t i = 0; i < g.n; ++i)
        if (in[i])
            for (auto t = head[i]; t <= last(i); ++t) active[t][g.kind[i]].push_back(i);
    for (std::int32_t t = 1; t <= horizon; ++t) {
        for (int k = 0; k < OP_KINDS; ++k) {
            auto const &ops = active[t][k];
            if (static_cast<std::int32_t>(ops.size()) <= g.limit[k]) continue;
            terms = 0;
            lp += " r" + std::to_string(k) + "_" + std::to_string(t) + ":";
            for (auto const i : ops) {
                lp += " + " + y(i, t), ++terms;
                if (is_var(i, t - 1)) lp += " - " + y(i, t - 1), ++terms;
                if (terms >= 8) lp += "\n ", terms = 0;
            }
            lp += " <= " + std::to_string(g.limit[k]) + "\n";
        }
    }
    lp += "Binaries\n";
    for (std::int32_t i = 0; i < g.n; ++i)
        if (in[i])
            for (auto t = head[i]; t <= last(i); ++t) lp += " " + y(i, t) + "\n";
    lp += "End\n";

    GurobiJob job;
    job.model_file = base + ".lp";
    job.result_file = base + ".sol";
    job.log_file = base + ".log";
    if (!write_file(job.model_file, lp)) return result;
    job.params = {{"LogFile", ""}, {"TimeLimit", std::to_string(time_limit)}, {"MIPGap", "0"},
                  {"MIPGapAbs", "0.99"}};
    if (options.threads > 0) job.params.push_back({"Threads", std::to_string(options.threads)});
    job.interrupt_after = time_limit + 1.0;
    job.kill_after = time_limit + 2.0;
    auto const solved = run_gurobi(options.gurobi_cl, job);
    if (!solved) return result;
    auto const &s = solved.value();
    if (s.status == SolveStatus::Optimal && s.has_solution) {
        result.bound = static_cast<std::int64_t>(s.objective + 0.5);
        result.solved = true;
    } else if (s.has_bound) {
        result.bound = static_cast<std::int64_t>(s.best_bound + 1e-6);
        result.solved = true;
    }
    // Never more than every unit slot of the prefix (Gurobi's bound before the root LP ends)
    result.bound = std::min(result.bound, static_cast<std::int64_t>(g.limit[b]) * horizon);
    if (s.has_solution) {
        result.step.assign(g.n, 0);
        for (std::int32_t i = 0; i < g.n; ++i) {
            if (!in[i]) continue;
            for (auto t = head[i]; t <= last(i); ++t) {
                auto const it = s.value.find(y(i, t));
                if (it != s.value.end() && it->second > 0.5) {
                    result.step[i] = t;
                    break;
                }
            }
        }
    }
    return result;
}

// Lower bound from the two ends of the schedule, for instances too large for the full model.
// The bottleneck kind b needs ceil(N_b / c_b) steps even if its machines never idle. The prefix
// relaxation proves that at least idle_head unit slots stay idle in the first T steps, the same
// on the mirror problem gives idle_tail for the last T' steps, and so
//   latency >= ceil((N_b + idle_head + idle_tail) / c_b)        (given T + T' <= latency).
// T and T' are taken from the heuristic schedule: where its kind-b machines last idle near the
// start, and first idle near the end.
void ends_bound(OpGraph const &g, HeuristicResult const &h, ScratchDir const &dir,
                IlpOptions const &options, Clock const &clock, ExactResult &result) {
    int b = 0;
    std::int64_t load = -1;
    for (int k = 0; k < OP_KINDS; ++k) {
        if (g.count[k] == 0) continue;
        auto const l = (static_cast<std::int64_t>(g.count[k]) + g.limit[k] - 1) / g.limit[k];
        if (l > load) load = l, b = k;
    }
    auto const c = g.limit[b];
    std::vector<std::int32_t> used(h.latency + 2, 0);
    for (std::int32_t i = 0; i < g.n; ++i)
        if (g.kind[i] == b) ++used[h.step[i]];
    std::int32_t head_span = 0, tail_span = 0;
    for (std::int32_t t = 1; t <= h.latency; ++t) {
        if (used[t] >= c) continue;
        if (t <= h.latency / 2) head_span = t;
        else if (tail_span == 0) tail_span = h.latency + 1 - t;
    }
    auto const mirror = reversed(g);
    std::vector<std::int32_t> mirror_head(g.n);
    for (std::int32_t i = 0; i < g.n; ++i) mirror_head[g.n - 1 - i] = h.tail[i] + 1;

    std::int64_t idle_head = 0, idle_tail = 0;
    for (int side = 0; side < 2; ++side) {
        auto span = side == 0 ? head_span : tail_span;
        if (span == 0) continue;
        span += 2;
        if (head_span + tail_span + 4 > result.lower_bound) return; // the ends would overlap
        auto const budget = (clock.remaining() - 4.0) / (side == 0 ? 2.0 : 1.0);
        if (budget < 2.0) return;
        ++result.solves;
        auto const p = solve_prefix(side == 0 ? g : mirror, side == 0 ? h.head : mirror_head, b,
                                    span, dir.file(side == 0 ? "head" : "tail"), options,
                                    options.max_vars, budget);
        result.num_vars = p.vars;
        result.note += (side == 0 ? "head:" : ",tail:") + std::to_string(span) + "/" +
                       std::to_string(p.vars) + "/" +
                       (p.solved ? std::to_string(static_cast<std::int64_t>(c) * span - p.bound)
                                 : std::string("x"));
        if (!p.solved) continue;
        (side == 0 ? idle_head : idle_tail) =
            std::max<std::int64_t>(0, static_cast<std::int64_t>(c) * span - p.bound);
    }
    auto const bound = (g.count[b] + idle_head + idle_tail + c - 1) / c;
    result.lower_bound = std::max(result.lower_bound, static_cast<std::int32_t>(bound));
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
    if (result.latency <= result.lower_bound) {
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

    while (result.latency > result.lower_bound) {
        auto const horizon = result.latency - 1;
        auto const model = make_step_model(g, heuristic, horizon);
        if (model.trivially_infeasible) {
            result.lower_bound = result.latency;
            break;
        }
        result.num_vars = model.num_vars;
        if (model.num_vars > options.max_vars) {
            ends_bound(g, heuristic, dir, options, clock, result);
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
                      {"MIPFocus", "1"}};
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
