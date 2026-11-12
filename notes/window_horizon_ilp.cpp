// Experimental 2x2 window/horizon diagnostic; recursive bounds from claude@55567d1.
// Experimental compact cumulative ML-RCS model. Mathematical step-variable
// inspiration: notes/codex.md and claude@ce1d6e4 (Q6); independently implemented.
// Unlike Q6's fixed-horizon feasibility model, retain scalar control's UB,
// ASAP/ALAP windows, latency-minimizing objective, and full heuristic MIP start.
#include "ilp.hpp"
#include "gurobi.hpp"
#include "bounds.hpp"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {
struct LpText {
    std::string text;
    Clock const &clock;
    int line_terms = 0;
    std::int64_t rhs = 0;
    std::int64_t rows = 0;
    bool oversized = false;
    bool timed_out = false;
    std::uint32_t checks = 0;
    explicit LpText(Clock const &c) : clock(c) {}
    bool stopped() {
        oversized = oversized || text.size() >= 128U * 1024U * 1024U;
        timed_out = timed_out || clock.remaining() <= 4.0;
        return oversized || timed_out;
    }
    void begin(std::string const &name, std::int64_t value) {
        text += ' ' + name + ':'; line_terms = 0; rhs = value; ++rows;
    }
    void term(std::int64_t c, std::string const &name) {
        if (oversized || timed_out) return;
        if (++checks == 4096) { checks = 0; if (stopped()) return; }
        if (line_terms == 8) { text += "\n  "; line_terms = 0; }
        text += c < 0 ? " - " : " + ";
        auto const a = c < 0 ? -c : c;
        if (a != 1) text += std::to_string(a) + ' ';
        text += name; ++line_terms;
    }
    void end(char const *sense) {
        // The fixed dummy provides a valid LP expression even for a row whose
        // z terms all reduced to boundary constants.
        if (line_terms == 0) text += " 0 dummy";
        text += ' '; text += sense; text += ' ' + std::to_string(rhs) + '\n';
    }
};
std::string z(NodeId i, std::int32_t t) { return "z" + std::to_string(i) + "_" + std::to_string(t); }
Result<Unit> write_file(std::string const &path, std::string const &text) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return err("cannot write " + path);
    f << text;
    if (!f) return err("cannot write " + path);
    return Unit{};
}
} // namespace

char const *exact_status_name(ExactStatus status) {
    switch (status) {
    case ExactStatus::Optimal: return "optimal";
    case ExactStatus::Bound: return "bound";
    case ExactStatus::TimeLimit: return "timelimit";
    case ExactStatus::TooLarge: return "too_large";
    }
    return "?";
}
Result<ExactResult> exact_schedule(Netlist const &n, Resources const &r, Schedule const &h,
                                  IlpOptions const &options, Clock const &clock) {
    ExactResult result;
    result.schedule = h;
    result.status = ExactStatus::TimeLimit;
    result.lower_bound = lower_bound(n, r);
    auto const ub = h.latency;
    auto const *mode_env = std::getenv("MLRCS_DIAG_MODE");
    std::string const mode = mode_env ? mode_env : "path-min";
    bool const recursive = mode == "resource-min" || mode == "resource-feas";
    bool const feasibility = mode == "path-feas" || mode == "resource-feas";
    auto const horizon = ub - (feasibility ? 1 : 0);
    bool const force = std::getenv("MLRCS_DIAG_FORCE") != nullptr;
    std::cerr << "window-horizon mode=" << mode << " ub=" << ub << " H=" << horizon << " start=" << !feasibility << '\n';
    if (!force && ub <= result.lower_bound) { result.status = ExactStatus::Bound; return result; }
    if (clock.remaining() <= 4.0) return result;
    auto early = asap(n);
    auto late = alap(n, horizon);
    if (recursive) {
        auto const graph = make_op_graph(n, r);
        auto const tail = resource_tails(graph, descendants(graph, 64LL * 1024 * 1024));
        auto const mirror = reversed(graph);
        auto const reverse_tail = resource_tails(mirror, descendants(mirror, 64LL * 1024 * 1024));
        for (std::int32_t j = 0; j < graph.n; ++j) {
            auto const i = graph.node[j];
            early[i] = std::max(early[i], reverse_tail[graph.n - 1 - j] + 1);
            late[i] = std::min(late[i], horizon - tail[j]);
        }
    }
    for (auto const i : n.topo) {
        if (early[i] <= late[i]) continue;
        // Sound head/tail bounds prove there is no feasible schedule by H.
        if (feasibility) { result.status = ExactStatus::Optimal; result.lower_bound = ub; }
        std::cerr << "window-horizon solver=empty-window-proof\n";
        return result;
    }
    for (auto const i : n.topo) result.num_vars += late[i] - early[i];
    if (result.num_vars > std::min<std::int64_t>(options.max_vars, 1'000'000)) {
        result.status = ExactStatus::TooLarge; return result;
    }
    LpText lp(clock);
    auto const stop = [&]() {
        if (!lp.stopped()) return false;
        result.status = lp.oversized ? ExactStatus::TooLarge : ExactStatus::TimeLimit;
        return true;
    };
    auto const add_z = [&](NodeId i, std::int32_t t, std::int64_t c) {
        if (t < early[i]) return; // fixed zero
        if (t >= late[i]) lp.rhs -= c; // fixed one
        else lp.term(c, z(i, t));
    };
    lp.text = "\\ window horizon diagnostic\nMinimize\n " + std::string(feasibility ? "0 dummy" : "L") + "\nSubject To\n";
    // z(i,t)=1 iff i starts at or before t; monotonicity gives exactly one
    // 0->1 transition, including fixed 0 before early and fixed 1 from late.
    for (auto const i : n.topo) {
        if (stop()) return result;
        for (auto t = early[i]; t + 1 < late[i]; ++t) {
            lp.begin("m" + std::to_string(i) + "_" + std::to_string(t), 0);
            add_z(i, t, 1); add_z(i, t + 1, -1); lp.end("<=");
        }
        for (auto const j : n.fanin.row(i)) {
            if (!n.is_op(j)) continue;
            // Outside this range, child is fixed 0 or parent is fixed 1.
            // add_z also handles all variable/fixed boundary combinations.
            for (auto t = early[i]; t <= late[j]; ++t) {
                if (stop()) return result;
                lp.begin("p" + std::to_string(j) + "_" + std::to_string(i) + "_" + std::to_string(t), 0);
                add_z(i, t, 1); add_z(j, t - 1, -1); lp.end("<=");
            }
        }
    }
    // Starts at t equal z(i,t)-z(i,t-1). Store only operations whose
    // transition can occur at each step; total entries <= vars + operations.
    std::vector<std::array<std::vector<NodeId>, OP_KINDS>> active(horizon + 1);
    for (auto const i : n.topo) {
        if (stop()) return result;
        for (auto t = early[i]; t <= late[i]; ++t) active[t][op_index(n.kind[i])].push_back(i);
    }
    for (std::int32_t t = 1; t <= horizon; ++t) {
        if (stop()) return result;
        for (int k = 0; k < OP_KINDS; ++k) {
            auto const &ops = active[t][k];
            if (static_cast<std::int32_t>(ops.size()) <= r.limit[k]) continue;
            lp.begin("r" + std::to_string(k) + "_" + std::to_string(t), r.limit[k]);
            for (auto const i : ops) { add_z(i, t, 1); add_z(i, t - 1, -1); }
            lp.end("<=");
        }
    }
    // start(i)=late(i)-sum_t z(i,t), an identity for a cumulative step vector.
    for (auto const i : n.topo) {
        if (stop()) return result;
        if (feasibility || !n.fanout.row(i).empty()) continue;
        lp.begin("l" + std::to_string(i), late[i]); lp.term(1, "L");
        for (auto t = early[i]; t < late[i]; ++t) lp.term(1, z(i, t));
        lp.end(">=");
    }
    if (stop()) return result;
    lp.text += "Bounds\n dummy = 0\n";
    if (!feasibility) lp.text += " " + std::to_string(result.lower_bound) + " <= L <= " + std::to_string(ub) + "\nGenerals\n L\n";
    lp.text += "Binaries\n";
    for (auto const i : n.topo) {
        if (stop()) return result;
        for (auto t = early[i]; t < late[i]; ++t) lp.text += ' ' + z(i, t) + '\n';
    }
    lp.text += "End\n";
    std::string start = "L " + std::to_string(ub) + "\ndummy 0\n";
    for (auto const i : n.topo) {
        if (stop()) return result;
        for (auto t = early[i]; t < late[i]; ++t)
            start += z(i, t) + (h.step[i] <= t ? " 1\n" : " 0\n");
    }
    if (stop()) return result;
    auto d = ScratchDir::create(options.keep_dir);
    if (!d) return result;
    auto const dir = std::move(d).value();
    GurobiJob job;
    job.model_file = dir.file("model.lp"); job.start_file = feasibility ? "" : dir.file("start.mst");
    job.result_file = dir.file("model.sol"); job.log_file = dir.file("gurobi.log");
    if (!write_file(job.model_file, lp.text) || (!feasibility && !write_file(job.start_file, start))) return result;
    if (auto const *meta = std::getenv("MLRCS_ABLATION_META")) {
        std::ofstream out(meta, std::ios::app);
        out << mode << '\t' << n.model << '\t' << ub << '\t' << horizon << '\t' << result.num_vars << '\t' << lp.text.size() << '\t' << lp.rows << '\n';
    }
    std::string{}.swap(lp.text); std::string{}.swap(start); decltype(active){}.swap(active);
    auto const remaining = clock.remaining();
    if (remaining <= 4.0) return result;
    job.params = {{"LogFile", ""}, {"TimeLimit", std::to_string(std::max(1.0, remaining - 4.0))}, {"MIPGap", "0"}, {"Method", "2"}, {"Presolve", "2"}, {"DualReductions", "0"}};
    if (options.threads > 0) job.params.push_back({"Threads", std::to_string(options.threads)});
    job.interrupt_after = std::max(1.5, remaining - 2.5); job.kill_after = std::max(2.0, remaining - 1.0);
    auto const solved_result = run_gurobi(options.gurobi_cl, job);
    if (!solved_result) return result;
    auto const &solved = solved_result.value();
    std::cerr << "window-horizon solver=" << status_name(solved.status) << " killed=" << solved.killed << " has_solution=" << solved.has_solution << '\n';
    if (solved.status == SolveStatus::Infeasible) {
        if (feasibility) { result.status = ExactStatus::Optimal; result.lower_bound = ub; }
        return result;
    }
    if (!solved.has_solution) return result;
    Schedule s;
    s.step.assign(n.size(), 0);
    for (auto const i : n.topo) {
        bool seen_one = false;
        s.step[i] = late[i];
        for (auto t = early[i]; t < late[i]; ++t) {
            auto const it = solved.value.find(z(i, t));
            auto const v = it == solved.value.end() ? 0.0 : it->second;
            if (!std::isfinite(v) || (std::abs(v) > 1e-5 && std::abs(v - 1.0) > 1e-5)) return result;
            if (v > 0.5) {
                if (!seen_one) s.step[i] = t;
                seen_one = true;
            } else if (seen_one) return result; // cumulative sequence must be monotone
        }
        s.latency = std::max(s.latency, s.step[i]);
    }
    if (s.latency > h.latency || !validate(n, r, s)) return result;
    result.schedule = std::move(s);
    result.status = !feasibility && solved.status == SolveStatus::Optimal ? ExactStatus::Optimal : ExactStatus::TimeLimit;
    if (result.schedule.latency == result.lower_bound) result.status = ExactStatus::Bound;
    if (result.status == ExactStatus::Optimal) result.lower_bound = result.schedule.latency;
    return result;
}
