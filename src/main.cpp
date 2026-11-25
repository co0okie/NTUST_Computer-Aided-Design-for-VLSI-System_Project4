#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

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
    "  -e               exact (ILP, solved with Gurobi)\n"
    "The schedule is written to <BLIF base name>_h.out or _e.out in the current directory\n"
    "(and also printed to stdout).\n"
    "options:\n"
    "  --time SECONDS   wall-clock budget for the whole run (default 55)\n"
    "  --heffort F      scale the heuristic's deterministic work budget (default 1; 0: none;\n"
    "                   the budget is min(2e8, 3e5 * ops) units, about 10 s at most)\n"
    "  --threads N      Gurobi threads (default: Gurobi's choice)\n"
    "  --keep DIR       write the ILP models (.lp) and the Gurobi log (gurobi.log) to DIR\n"
    "  --stats          print one line of statistics to stderr\n"
    "testing (-e, end relaxations used when the full ILP is too large):\n"
    "  --ends H,T,M[,G] spans H (head) and T (tail), M checkpoints, spacing exponent G\n"
    "                   (0 = automatic); --ends-test: same, but run them on every instance\n"
    "                   and without the size cap, then stop (bound sanity checks)\n"
    "  --ends-time S    seconds for the end relaxations (default 10)\n";

enum class Mode { Heuristic, Exact };

struct Options {
    Mode mode = Mode::Heuristic;
    std::string blif;
    Resources resources;
    double time = 55;
    double heffort = 1;
    int threads = 0;
    std::string keep_dir;
    bool stats = false;
    EndsOptions ends;
};

// "H,T,M" or "H,T,M,G"
Result<Unit> parse_ends(char const *text, EndsOptions &ends) {
    char *end = nullptr;
    long v[3] = {0, 0, 0};
    char const *at = text;
    for (int i = 0; i < 3; ++i) {
        errno = 0;
        v[i] = std::strtol(at, &end, 10);
        if (end == at || errno != 0 || v[i] < 0 || v[i] > 1'000'000 || (i < 2 && *end != ',') ||
            (i == 2 && *end != ',' && *end != '\0'))
            return err(std::string("bad value for --ends: ") + text);
        at = end + 1;
    }
    ends.head_span = static_cast<std::int32_t>(v[0]);
    ends.tail_span = static_cast<std::int32_t>(v[1]);
    ends.checkpoints = static_cast<std::int32_t>(v[2]); // 0: automatic
    if (*end == ',') {
        ends.gamma = std::strtod(end + 1, &end);
        if (*end != '\0' || !(ends.gamma > 0) || ends.gamma > 10)
            return err(std::string("bad value for --ends: ") + text);
    }
    return Unit{};
}

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
        if (v < 0 || v > 2'147'483'647) return err(std::string("bad ") + names[k]);
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
            if (end == value || *end != '\0' || !std::isfinite(o.time) || !(o.time > 0))
                return err("bad value for --time");
        } else if (flag == "--heffort") {
            char *end = nullptr;
            o.heffort = std::strtod(value, &end);
            if (*end != '\0' || !(o.heffort >= 0) || o.heffort > 1000)
                return err("bad value for --heffort");
        } else if (flag == "--threads") {
            TRY(auto const v, parse_int(value, "--threads"));
            if (v < 0) return err("bad value for --threads");
            o.threads = static_cast<int>(v);
        } else if (flag == "--ends" || flag == "--ends-test") {
            TRY_VOID(parse_ends(value, o.ends));
            o.ends.force = flag == "--ends-test";
        } else if (flag == "--ends-time") {
            char *end = nullptr;
            o.ends.time = std::strtod(value, &end);
            if (*end != '\0' || !(o.ends.time >= 0)) return err("bad value for --ends-time");
        } else if (flag == "--keep") {
            o.keep_dir = value;
        } else {
            return err("unknown option " + std::string(flag));
        }
    }
    return o;
}

namespace fs = std::filesystem;

// The graded output: <BLIF base name>_h.out or _e.out in the current directory
std::string output_path(Options const &o) {
    return fs::path(o.blif).stem().string() + (o.mode == Mode::Heuristic ? "_h.out" : "_e.out");
}

// Writes the schedule to `path` atomically (temporary file, then rename), so the file always
// holds a complete schedule, even if the program is killed while writing.
Result<Unit> write_output(std::string const &path, Netlist const &n, Schedule const &s,
                          char const *title) {
    std::ostringstream text;
    print_schedule(text, n, s, title);
    auto const tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return err("cannot write " + tmp);
        out << text.str();
        out.flush();
        if (!out) return err("cannot write " + tmp);
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) return err("cannot rename " + tmp + " to " + path + ": " + ec.message());
    return Unit{};
}

// Last resort against overrunning the time limit (a solver call that does not return): once the
// output file holds a valid schedule, ends the process at the hard deadline. The file is written
// atomically, so it is complete at any moment.
class HardDeadline {
public:
    explicit HardDeadline(double seconds_from_now)
        : thread_([this, seconds_from_now] {
              std::unique_lock<std::mutex> lock(mutex_);
              auto const deadline =
                  std::chrono::steady_clock::now() +
                  std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                      std::chrono::duration<double>(seconds_from_now));
              if (!cv_.wait_until(lock, deadline, [this] { return done_; })) {
                  std::cerr << "mlrcs: hard deadline reached; the output file holds the best "
                               "schedule found\n";
                  std::_Exit(0);
              }
          }) {}
    HardDeadline(HardDeadline const &) = delete;
    HardDeadline &operator=(HardDeadline const &) = delete;
    ~HardDeadline() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            done_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool done_ = false;
    std::thread thread_; // last member: starts after the others are constructed
};

// A quick first schedule, written before the long heuristic: list scheduling by height (longest
// path to a sink, in ops)
std::vector<std::int32_t> quick_schedule(OpGraph const &g) {
    std::vector<std::int64_t> height(g.n, 1);
    for (std::int32_t i = g.n - 1; i >= 0; --i)
        for (auto const j : g.succ.row(i)) height[i] = std::max(height[i], height[j] + 1);
    return list_schedule(g, height);
}

Result<Unit> run(Options const &o, Clock &clock) {
    clock.budget = o.time;
    auto const out_path = output_path(o);
    char const *const title =
        o.mode == Mode::Heuristic ? "Heuristic Scheduling Result" : "ILP-based Scheduling Result";

    // Start Gurobi before doing any work, so a missing solver or license fails fast
    std::optional<Gurobi> gurobi;
    if (o.mode == Mode::Exact) {
        std::string log_file;
        if (!o.keep_dir.empty()) {
            std::error_code ec;
            fs::create_directories(o.keep_dir, ec);
            if (ec) return err("cannot create " + o.keep_dir + ": " + ec.message());
            log_file = o.keep_dir + "/gurobi.log";
        }
        TRY(auto g, Gurobi::open(log_file, o.threads));
        gurobi.emplace(std::move(g));
    }

    TRY(auto const netlist, read_blif(o.blif));
    TRY_VOID(check_resources(netlist, o.resources));
    // A limit above the number of operations of its kind never binds; clamping it (to at least
    // 1, which also covers a 0 limit for a kind that has no operations) keeps every later
    // computation (ceil(N/c), c * steps, ...) far from overflow without changing any result.
    Resources resources = o.resources;
    for (int k = 0; k < OP_KINDS; ++k)
        resources.limit[k] = std::max(1, std::min(resources.limit[k], netlist.op_count[k]));
    auto const graph = make_op_graph(netlist, resources);

    // From here on the output file always holds a valid schedule: first a quick one, replaced
    // by every better one
    auto const quick = to_schedule(netlist, graph, quick_schedule(graph));
    TRY_VOID(validate(netlist, resources, quick));
    TRY_VOID(write_output(out_path, netlist, quick, title));
    HardDeadline const hard_deadline(clock.remaining() + 2.5);

    // The same deterministic heuristic for -h and -e (so -e, which only improves on it, is never
    // worse). Wall-clock guards only fire on a machine several times slower than ours.
    auto found = heuristic_schedule(graph, clock, 20.0);
    std::int64_t hwork = 0;
    auto const budget = static_cast<std::int64_t>(
        o.heffort * static_cast<double>(std::min<std::int64_t>(200'000'000, std::int64_t{300'000} * graph.n)));
    improve_schedule(graph, found, budget, clock, std::min(30.0, o.time - 20.0), hwork);
    auto const heuristic = to_schedule(netlist, graph, found.step);
    TRY_VOID(validate(netlist, resources, heuristic));
    if (heuristic.latency <= quick.latency) {
        TRY_VOID(write_output(out_path, netlist, heuristic, title));
    }
    auto const &best_heuristic = heuristic.latency <= quick.latency ? heuristic : quick;

    if (o.mode == Mode::Heuristic) {
        print_schedule(std::cout, netlist, best_heuristic, title);
        if (o.stats)
            std::cerr << "mlrcs-stats mode=h latency=" << best_heuristic.latency
                      << " lower_bound=" << found.lower_bound << " first=" << found.first_latency
                      << " quick=" << quick.latency << " passes=" << found.passes << " window_vars=" << [&] {
                             std::int64_t v = 0;
                             for (std::int32_t i = 0; i < graph.n; ++i)
                                 v += std::max(0, found.latency - 1 - found.tail[i] - found.head[i] + 1);
                             return v;
                         }()
                      << " work=" << hwork << " ops=" << netlist.num_ops() << " time=" << clock.elapsed() << '\n';
        return Unit{};
    }

    IlpOptions ilp;
    ilp.keep_dir = o.keep_dir;
    ilp.ends = o.ends;
    auto exact = exact_schedule(graph, found, *gurobi, ilp, clock);
    if (exact.status == ExactStatus::TooLarge && exact.latency > exact.lower_bound) {
        // The model is too big to solve: spend the remaining time on randomized heuristics
        HeuristicResult more = found;
        more.step = exact.step;
        more.latency = exact.latency;
        more.lower_bound = exact.lower_bound; // stop as soon as the (maybe raised) bound is met
        std::int64_t more_work = 0;
        restart_schedule(graph, more, std::int64_t{1} << 62, more_work, 0x2545f4914f6cdd1dULL, clock,
                         o.time - 3.0);
        if (more.latency < exact.latency) {
            exact.step = more.step;
            exact.latency = more.latency;
        }
        // Met a bound the ILP side raised (end relaxations): proven by the solver, so optimal
        if (exact.latency <= exact.lower_bound)
            exact.status = exact.lower_bound > found.lower_bound ? ExactStatus::Optimal
                                                                 : ExactStatus::Bound;
    }
    auto const schedule = to_schedule(netlist, graph, exact.step);
    auto const valid = validate(netlist, resources, schedule);
    // Whatever happened in the solver, the heuristic schedule is always a legal answer
    auto const &final_schedule =
        valid && schedule.latency <= best_heuristic.latency ? schedule : best_heuristic;
    TRY_VOID(write_output(out_path, netlist, final_schedule, title));
    print_schedule(std::cout, netlist, final_schedule, title);
    if (o.stats)
        std::cerr << "mlrcs-stats mode=e latency=" << final_schedule.latency
                  << " heuristic=" << heuristic.latency << " lower_bound=" << exact.lower_bound
                  << " status=" << (valid ? exact_status_name(exact.status) : "invalid")
                  << " ends_lb=" << exact.ends_bound << " vars=" << exact.num_vars << " nnz=" << exact.num_nonzeros << " solves=" << exact.solves
                  << " build=" << exact.build_seconds
                  << " note=" << (exact.note.empty() ? "-" : exact.note)
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
