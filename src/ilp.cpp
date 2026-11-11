#include "ilp.hpp"

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include "gurobi.hpp"

namespace {

// Builds an LP-format file. Long expressions are wrapped, since LP readers limit line length.
struct LpText {
    std::string text;
    int terms_on_line = 0;

    void begin(std::string const &label) {
        text += ' ';
        text += label;
        text += ':';
        terms_on_line = 0;
    }
    void term(std::int64_t coef, std::string const &var) {
        if (terms_on_line == 8) {
            text += "\n  ";
            terms_on_line = 0;
        }
        text += coef < 0 ? " - " : " + ";
        auto const a = coef < 0 ? -coef : coef;
        if (a != 1) {
            text += std::to_string(a);
            text += ' ';
        }
        text += var;
        ++terms_on_line;
    }
    void end(char const *sense, std::int64_t rhs) {
        text += ' ';
        text += sense;
        text += ' ';
        text += std::to_string(rhs);
        text += '\n';
    }
};

std::string x(NodeId i, std::int32_t t) { return "x" + std::to_string(i) + "_" + std::to_string(t); }

Result<Unit> write_file(std::string const &path, std::string const &text) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return err("cannot write " + path);
    out << text;
    if (!out) return err("cannot write " + path);
    return Unit{};
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

Result<ExactResult> exact_schedule(Netlist const &n, Resources const &r, Schedule const &heuristic,
                                   IlpOptions const &options, Clock const &clock) {
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

    LpText lp;
    lp.text = "\\ ML-RCS time-indexed model for " + n.model + "\nMinimize\n L\nSubject To\n";
    // Every operation starts exactly once
    for (auto const i : n.topo) {
        lp.begin("a" + std::to_string(i));
        for (auto t = early[i]; t <= late[i]; ++t) lp.term(1, x(i, t));
        lp.end("=", 1);
    }
    // Precedence: start(i) >= start(j) + 1 for every operation edge j -> i
    for (auto const i : n.topo) {
        for (auto const j : n.fanin.row(i)) {
            if (!n.is_op(j) || late[j] < early[i]) continue; // windows already ordered
            lp.begin("p" + std::to_string(j) + "_" + std::to_string(i));
            for (auto t = early[i]; t <= late[i]; ++t) lp.term(t, x(i, t));
            for (auto t = early[j]; t <= late[j]; ++t) lp.term(-t, x(j, t));
            lp.end(">=", 1);
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
            lp.begin("r" + std::to_string(k) + "_" + std::to_string(t));
            for (auto const i : ops) lp.term(1, x(i, t));
            lp.end("<=", r.limit[k]);
        }
    }
    // Latency: L >= start(i) for every operation with no operation fanout
    for (auto const i : n.topo) {
        if (!n.fanout.row(i).empty()) continue;
        lp.begin("l" + std::to_string(i));
        lp.term(1, "L");
        for (auto t = early[i]; t <= late[i]; ++t) lp.term(-t, x(i, t));
        lp.end(">=", 0);
    }
    lp.text += "Bounds\n " + std::to_string(result.lower_bound) + " <= L <= " + std::to_string(ub) +
               "\nGenerals\n L\nBinaries\n";
    for (auto const i : n.topo)
        for (auto t = early[i]; t <= late[i]; ++t) lp.text += ' ' + x(i, t) + '\n';
    lp.text += "End\n";

    // MIP start from the heuristic schedule
    std::string start = "L " + std::to_string(ub) + "\n";
    for (auto const i : n.topo) start += x(i, heuristic.step[i]) + " 1\n";

    TRY(auto const dir, ScratchDir::create(options.keep_dir));
    GurobiJob job;
    job.model_file = dir.file("model.lp");
    job.start_file = dir.file("start.mst");
    job.result_file = dir.file("model.sol");
    job.log_file = dir.file("gurobi.log");
    TRY_VOID(write_file(job.model_file, lp.text));
    TRY_VOID(write_file(job.start_file, start));
    // Leave time for Gurobi to wind down, and for us to print the schedule
    auto const remaining = clock.remaining();
    auto const time_limit = std::max(1.0, remaining - 4.0);
    // LogFile= (empty): no gurobi.log in the working directory; stdout is captured instead
    job.params = {{"LogFile", ""}, {"TimeLimit", std::to_string(time_limit)}, {"MIPGap", "0"}};
    if (options.threads > 0) job.params.push_back({"Threads", std::to_string(options.threads)});
    job.interrupt_after = std::max(1.5, remaining - 2.5);
    job.kill_after = std::max(2.0, remaining - 1.0);
    TRY(auto const solved, run_gurobi(options.gurobi_cl, job));

    if (solved.status == SolveStatus::Infeasible)
        return err("Gurobi reports the ILP infeasible, but the heuristic schedule is feasible; "
                   "see " + job.log_file);
    if (!solved.has_solution) {
        result.status = ExactStatus::TimeLimit; // keep the heuristic schedule
        return result;
    }

    Schedule s;
    s.step.assign(n.size(), 0);
    for (auto const i : n.topo) {
        for (auto t = early[i]; t <= late[i]; ++t) {
            auto const it = solved.value.find(x(i, t));
            if (it != solved.value.end() && it->second > 0.5) s.step[i] = t;
        }
        if (s.step[i] == 0) return err("solution misses operation " + n.name[i]);
        s.latency = std::max(s.latency, s.step[i]);
    }
    result.schedule = std::move(s);
    result.status = solved.status == SolveStatus::Optimal ? ExactStatus::Optimal
                                                          : ExactStatus::TimeLimit;
    return result;
}
