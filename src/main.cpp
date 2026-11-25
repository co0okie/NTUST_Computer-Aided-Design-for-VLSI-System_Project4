#include <cerrno>
#include <cstdio>
#include <chrono>
#include <condition_variable>
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
    "  --threads N      Gurobi threads (default: Gurobi's choice)\n"
    "  --keep DIR       write the ILP model (model.lp) and the Gurobi log (gurobi.log) to DIR\n"
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

Result<Unit> run(Options const &o, Clock &clock) {
    clock.budget = o.time;
    auto const out_path = output_path(o);

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
    TRY(auto const heuristic, list_schedule(netlist, o.resources));
    TRY_VOID(validate(netlist, o.resources, heuristic));

    if (o.mode == Mode::Heuristic) {
        TRY_VOID(write_output(out_path, netlist, heuristic, "Heuristic Scheduling Result"));
        print_schedule(std::cout, netlist, heuristic, "Heuristic Scheduling Result");
        if (o.stats)
            std::cerr << "mlrcs-stats mode=h latency=" << heuristic.latency
                      << " lower_bound=" << lower_bound(netlist, o.resources)
                      << " ops=" << netlist.num_ops() << " time=" << clock.elapsed() << '\n';
        return Unit{};
    }

    // From here on the output file always holds a valid schedule
    TRY_VOID(write_output(out_path, netlist, heuristic, "ILP-based Scheduling Result"));
    HardDeadline const hard_deadline(clock.remaining() + 2.5);

    IlpOptions ilp;
    ilp.keep_dir = o.keep_dir;
    TRY(auto const exact, exact_schedule(netlist, o.resources, heuristic, *gurobi, ilp, clock));
    TRY_VOID(validate(netlist, o.resources, exact.schedule));
    TRY_VOID(write_output(out_path, netlist, exact.schedule, "ILP-based Scheduling Result"));
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
