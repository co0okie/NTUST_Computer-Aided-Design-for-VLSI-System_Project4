#include "ilp.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#include "gurobi.hpp"

namespace {

// Builds an LP-format file. Long expressions are wrapped, since LP readers limit line length.
struct LpText {
    std::string text;
    int terms_on_line = 0;
    Clock const &clock;
    bool too_large = false;
    bool timed_out = false;
    std::uint32_t terms_since_check = 0;

    explicit LpText(Clock const &run_clock) : clock(run_clock) {}

    bool stopped() {
        // Bound text storage independently of the binary count: high-fanout graphs
        // repeat each binary in many precedence constraints.
        too_large = too_large || text.size() >= 128U * 1024U * 1024U;
        timed_out = timed_out || clock.remaining() <= 4.0;
        return too_large || timed_out;
    }

    void begin(std::string const &label) {
        if (stopped()) return;
        text += ' ';
        text += label;
        text += ':';
        terms_on_line = 0;
    }
    void term(std::int64_t coef, std::string const &var) {
        if (too_large || timed_out) return;
        if (++terms_since_check == 4096) {
            terms_since_check = 0;
            if (stopped()) return;
        }
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
        if (too_large || timed_out) return;
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
    result.status = ExactStatus::TimeLimit;
    result.lower_bound = lower_bound(n, r);
    auto const ub = heuristic.latency;
    if (ub <= result.lower_bound) {
        result.status = ExactStatus::Bound;
        return result;
    }

    if (clock.remaining() <= 4.0) return result;

    // Each operation can only start within [ASAP, ALAP(ub)]
    auto const early = asap(n);
    auto const late = alap(n, ub);
    for (auto const i : n.topo) result.num_vars += late[i] - early[i] + 1;
    // A million binaries already approaches the shared machine's per-process
    // memory budget in Gurobi; the text limit below also limits repeated terms.
    if (result.num_vars > std::min<std::int64_t>(options.max_vars, 1'000'000)) {
        result.status = ExactStatus::TooLarge;
        return result;
    }

    LpText lp(clock);
    auto const stop_building = [&]() {
        if (!lp.stopped()) return false;
        result.status = lp.too_large ? ExactStatus::TooLarge : ExactStatus::TimeLimit;
        return true;
    };
    lp.text = "\\ ML-RCS time-indexed model for " + n.model + "\nMinimize\n L\nSubject To\n";
    // Every operation starts exactly once
    for (auto const i : n.topo) {
        if (stop_building()) return result;
        lp.begin("a" + std::to_string(i));
        for (auto t = early[i]; t <= late[i]; ++t) lp.term(1, x(i, t));
        lp.end("=", 1);
    }
    // Precedence: start(i) >= start(j) + 1 for every operation edge j -> i
    for (auto const i : n.topo) {
        for (auto const j : n.fanin.row(i)) {
            if (stop_building()) return result;
            if (!n.is_op(j) || late[j] < early[i]) continue; // windows already ordered
            lp.begin("p" + std::to_string(j) + "_" + std::to_string(i));
            for (auto t = early[i]; t <= late[i]; ++t) lp.term(t, x(i, t));
            for (auto t = early[j]; t <= late[j]; ++t) lp.term(-t, x(j, t));
            lp.end(">=", 1);
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
            lp.begin("r" + std::to_string(k) + "_" + std::to_string(t));
            for (auto const i : ops) lp.term(1, x(i, t));
            lp.end("<=", r.limit[k]);
        }
    }
    // Latency: L >= start(i) for every operation with no operation fanout
    for (auto const i : n.topo) {
        if (stop_building()) return result;
        if (!n.fanout.row(i).empty()) continue;
        lp.begin("l" + std::to_string(i));
        lp.term(1, "L");
        for (auto t = early[i]; t <= late[i]; ++t) lp.term(-t, x(i, t));
        lp.end(">=", 0);
    }
    if (stop_building()) return result;
    lp.text += "Bounds\n " + std::to_string(result.lower_bound) + " <= L <= " + std::to_string(ub) +
               "\nGenerals\n L\nBinaries\n";
    for (auto const i : n.topo) {
        if (stop_building()) return result;
        for (auto t = early[i]; t <= late[i]; ++t) lp.text += ' ' + x(i, t) + '\n';
    }
    lp.text += "End\n";

    // MIP start from the heuristic schedule
    std::string start = "L " + std::to_string(ub) + "\n";
    for (auto const i : n.topo) start += x(i, heuristic.step[i]) + " 1\n";

    if (stop_building()) return result;
    auto dir_result = ScratchDir::create(options.keep_dir);
    if (!dir_result) return result;
    auto const dir = std::move(dir_result).value();
    GurobiJob job;
    job.model_file = dir.file("model.lp");
    job.start_file = dir.file("start.mst");
    job.result_file = dir.file("model.sol");
    job.log_file = dir.file("gurobi.log");
    if (!write_file(job.model_file, lp.text) || !write_file(job.start_file, start)) return result;
    // Release construction buffers before the solver's large allocation phase.
    std::string{}.swap(lp.text);
    std::string{}.swap(start);
    decltype(active){}.swap(active);
    // Leave time for Gurobi to wind down, and for us to print the schedule
    auto const remaining = clock.remaining();
    if (remaining <= 4.0) return result;
    auto const time_limit = std::max(1.0, remaining - 4.0);
    // LogFile= (empty): no gurobi.log in the working directory; stdout is captured instead
    job.params = {{"LogFile", ""}, {"TimeLimit", std::to_string(time_limit)}, {"MIPGap", "0"}};
    if (options.threads > 0) job.params.push_back({"Threads", std::to_string(options.threads)});
    job.interrupt_after = std::max(1.5, remaining - 2.5);
    job.kill_after = std::max(2.0, remaining - 1.0);
    auto const solved_result = run_gurobi(options.gurobi_cl, job);
    if (!solved_result) return result;
    auto const &solved = solved_result.value();
    if (solved.status == SolveStatus::Infeasible || !solved.has_solution) return result;

    Schedule s;
    s.step.assign(n.size(), 0);
    for (auto const i : n.topo) {
        for (auto t = early[i]; t <= late[i]; ++t) {
            auto const it = solved.value.find(x(i, t));
            if (it == solved.value.end()) continue;
            auto const value = it->second;
            if (!std::isfinite(value) || (std::abs(value) > 1e-5 && std::abs(value - 1.0) > 1e-5))
                return result;
            if (value > 0.5) {
                if (s.step[i] != 0) return result; // one-hot assignment is mandatory
                s.step[i] = t;
            }
        }
        if (s.step[i] == 0) return result;
        s.latency = std::max(s.latency, s.step[i]);
    }
    if (s.latency > heuristic.latency || !validate(n, r, s)) return result;
    result.schedule = std::move(s);
    result.status = solved.status == SolveStatus::Optimal ? ExactStatus::Optimal
                                                          : ExactStatus::TimeLimit;
    if (result.status == ExactStatus::Optimal) result.lower_bound = result.schedule.latency;
    return result;
}
