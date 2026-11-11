#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include "blif.hpp"
#include "clock.hpp"
#include "gurobi.hpp"
#include "heuristic.hpp"
#include "ilp.hpp"
#include "opgraph.hpp"
#include "result.hpp"
#include "schedule.hpp"

namespace {

constexpr char const *USAGE =
    "usage: mlrcs -h|-e BLIF_FILE AND_CONSTRAINT OR_CONSTRAINT NOT_CONSTRAINT [options]\n"
    "  -h               heuristic (list scheduling)\n"
    "  -e               exact (ILP, solved with gurobi_cl)\n"
    "options:\n"
    "  --time SECONDS   wall-clock budget for the whole run (default 55)\n"
    "  --threads N      Gurobi threads (default: Gurobi's choice)\n"
    "  --keep DIR       keep the ILP model, MIP start, solution and Gurobi log in DIR\n"
    "  --stats          print one line of statistics to stderr\n";

enum class Mode { Heuristic, Exact };

struct Options {
    Mode mode = Mode::Heuristic;
    std::string blif;
    Resources resources;
    double time = 55;
    int threads = 0;
    std::string keep_dir;
    bool stats = false;
};

Result<long> parse_int(char const *text, char const *what) {
    char *end = nullptr;
    errno = 0;
    long const v = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || errno != 0)
        return err(std::string("bad ") + what + ": " + text);
    return v;
}

Result<Options> parse_args(int argc, char **argv) {
    if (argc < 6) return err("not enough arguments");
    Options o;
    std::string_view const mode = argv[1];
    if (mode == "-h") o.mode = Mode::Heuristic;
    else if (mode == "-e") o.mode = Mode::Exact;
    else return err("first argument must be -h or -e, got " + std::string(mode));
    o.blif = argv[2];
    char const *const names[OP_KINDS] = {"AND constraint", "OR constraint", "NOT constraint"};
    for (int k = 0; k < OP_KINDS; ++k) {
        TRY(auto const v, parse_int(argv[3 + k], names[k]));
        if (v < 0 || v > 1'000'000'000) return err(std::string("bad ") + names[k]);
        o.resources.limit[k] = static_cast<std::int32_t>(v);
    }
    for (int i = 6; i < argc; ++i) {
        std::string_view const flag = argv[i];
        if (flag == "--stats") {
            o.stats = true;
            continue;
        }
        if (i + 1 >= argc) return err("missing value for " + std::string(flag));
        char const *value = argv[++i];
        if (flag == "--time") {
            char *end = nullptr;
            o.time = std::strtod(value, &end);
            if (*end != '\0' || !(o.time > 0)) return err("bad value for --time");
        } else if (flag == "--threads") {
            TRY(auto const v, parse_int(value, "--threads"));
            if (v < 0) return err("bad value for --threads");
            o.threads = static_cast<int>(v);
        } else if (flag == "--keep") {
            o.keep_dir = value;
        } else {
            return err("unknown option " + std::string(flag));
        }
    }
    return o;
}

Result<Unit> run(Options const &o, Clock &clock) {
    clock.budget = o.time;

    // Check for Gurobi before doing any work, so a missing solver fails fast
    std::string gurobi_cl;
    if (o.mode == Mode::Exact) {
        TRY(gurobi_cl, find_gurobi_cl());
    }

    TRY(auto const netlist, read_blif(o.blif));
    TRY_VOID(check_resources(netlist, o.resources));
    auto const graph = make_op_graph(netlist, o.resources);
    auto const found = heuristic_schedule(graph, clock, o.mode == Mode::Heuristic ? 5.0 : 3.0);
    auto const heuristic = to_schedule(netlist, graph, found.step);
    TRY_VOID(validate(netlist, o.resources, heuristic));

    if (o.mode == Mode::Heuristic) {
        print_schedule(std::cout, netlist, heuristic, "Heuristic Scheduling Result");
        if (o.stats)
            std::cerr << "mlrcs-stats mode=h latency=" << heuristic.latency
                      << " lower_bound=" << found.lower_bound << " first=" << found.first_latency
                      << " passes=" << found.passes
                      << " ops=" << netlist.num_ops() << " time=" << clock.elapsed() << '\n';
        return Unit{};
    }

    IlpOptions ilp;
    ilp.gurobi_cl = gurobi_cl;
    ilp.keep_dir = o.keep_dir;
    ilp.threads = o.threads;
    TRY(auto const exact,
        exact_schedule(netlist, o.resources, heuristic, found.lower_bound, ilp, clock));
    TRY_VOID(validate(netlist, o.resources, exact.schedule));
    print_schedule(std::cout, netlist, exact.schedule, "ILP-based Scheduling Result");
    if (o.stats)
        std::cerr << "mlrcs-stats mode=e latency=" << exact.schedule.latency
                  << " heuristic=" << heuristic.latency << " lower_bound=" << exact.lower_bound
                  << " status=" << exact_status_name(exact.status) << " vars=" << exact.num_vars
                  << " ops=" << netlist.num_ops() << " time=" << clock.elapsed() << '\n';
    return Unit{};
}

} // namespace

int main(int argc, char **argv) {
    Clock clock;
    auto const options = parse_args(argc, argv);
    if (!options) {
        std::cerr << "mlrcs: " << options.error().message << '\n' << USAGE;
        return 1;
    }
    auto const result = run(options.value(), clock);
    if (!result) {
        std::cerr << "mlrcs: " << result.error().message << '\n';
        return 1;
    }
    return 0;
}
