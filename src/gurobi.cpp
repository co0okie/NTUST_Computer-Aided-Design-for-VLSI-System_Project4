#include "gurobi.hpp"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <signal.h>
#include <spawn.h>
#include <sstream>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char **environ;

namespace fs = std::filesystem;

namespace {

struct Exit {
    bool exited = false; // false: terminated by a signal
    bool killed = false; // we had to send SIGKILL
    int code = -1;
};

using SteadyTime = std::chrono::steady_clock::time_point;

SteadyTime after(SteadyTime start, double seconds) {
    return start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                       std::chrono::duration<double>(seconds));
}

// Runs argv[0] with stdin from /dev/null and stdout+stderr to `log`. Sends SIGINT after
// `interrupt_after` seconds and SIGKILL after `kill_after` seconds.
Result<Exit> spawn_and_wait(std::vector<std::string> const &args, std::string const &log,
                            double interrupt_after, double kill_after) {
    std::vector<char *> argv;
    for (auto const &a : args) argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log.c_str(),
                                     O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    pid_t pid = 0;
    int const rc = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) return err("cannot start " + args[0] + ": " + std::strerror(rc));

    auto const start = std::chrono::steady_clock::now();
    auto const interrupt_at = after(start, interrupt_after);
    auto const kill_at = after(start, kill_after);
    bool interrupted = false;
    bool killed = false;
    int status = 0;
    for (;;) {
        pid_t const done = waitpid(pid, &status, WNOHANG);
        if (done == pid) break;
        if (done < 0 && errno != EINTR) return err(std::string("waitpid: ") + std::strerror(errno));
        auto const now = std::chrono::steady_clock::now();
        if (!interrupted && now >= interrupt_at) {
            kill(pid, SIGINT);
            interrupted = true;
        }
        if (!killed && now >= kill_at) {
            kill(pid, SIGKILL);
            killed = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    Exit e;
    e.exited = WIFEXITED(status);
    e.killed = killed;
    e.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return e;
}

bool contains(std::string const &haystack, char const *needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

Result<std::string> find_gurobi_cl() {
    std::vector<std::string> candidates;
    if (char const *path = std::getenv("PATH")) {
        std::string dirs = path;
        std::size_t start = 0;
        while (start <= dirs.size()) {
            auto const end = std::min(dirs.find(':', start), dirs.size());
            auto const dir = dirs.substr(start, end - start);
            candidates.push_back((dir.empty() ? std::string(".") : dir) + "/gurobi_cl");
            start = end + 1;
        }
    }
    if (char const *home = std::getenv("GUROBI_HOME"))
        candidates.push_back(std::string(home) + "/bin/gurobi_cl");

    std::string broken;
    for (auto const &c : candidates) {
        if (access(c.c_str(), X_OK) != 0) continue;
        auto const probe = spawn_and_wait({c, "--version"}, "/dev/null", 10, 12);
        if (probe && probe.value().exited && probe.value().code == 0) return c;
        if (broken.empty()) broken = c;
    }
    if (!broken.empty())
        return err("found " + broken +
                   " but it does not run (is LD_LIBRARY_PATH set to Gurobi's lib directory?)");
    return err("gurobi_cl not found on PATH or in $GUROBI_HOME/bin; -e needs Gurobi");
}

char const *status_name(SolveStatus s) {
    switch (s) {
    case SolveStatus::Optimal: return "optimal";
    case SolveStatus::TimeLimit: return "timelimit";
    case SolveStatus::Infeasible: return "infeasible";
    case SolveStatus::Other: return "other";
    }
    return "?";
}

Result<ScratchDir> ScratchDir::create(std::string const &requested) {
    std::error_code ec;
    if (!requested.empty()) {
        fs::create_directories(requested, ec);
        if (ec) return err("cannot create " + requested + ": " + ec.message());
        return ScratchDir(requested, true);
    }
    auto const base = fs::temp_directory_path(ec);
    if (ec) return err("no temporary directory: " + ec.message());
    std::string pattern = (base / "mlrcs.XXXXXX").string();
    if (mkdtemp(pattern.data()) == nullptr)
        return err("cannot create a directory in " + base.string() + ": " + std::strerror(errno));
    return ScratchDir(pattern, false);
}

ScratchDir::ScratchDir(ScratchDir &&other) noexcept
    : path_(std::move(other.path_)), keep_(other.keep_) {
    other.path_.clear();
}

ScratchDir::~ScratchDir() {
    if (keep_ || path_.empty()) return;
    std::error_code ec;
    fs::remove_all(path_, ec);
}

Result<GurobiResult> run_gurobi(std::string const &gurobi_cl, GurobiJob const &job) {
    std::vector<std::string> args{gurobi_cl};
    for (auto const &[key, value] : job.params) args.push_back(key + "=" + value);
    args.push_back("ResultFile=" + job.result_file);
    if (!job.start_file.empty()) args.push_back("InputFile=" + job.start_file);
    args.push_back(job.model_file);

    std::error_code ec;
    fs::remove(job.result_file, ec);
    TRY(auto const exit,
        spawn_and_wait(args, job.log_file, job.interrupt_after, job.kill_after));

    std::ifstream log_in(job.log_file);
    std::stringstream log_text;
    log_text << log_in.rdbuf();
    auto const log = log_text.str();

    GurobiResult result;
    if (exit.killed) {
        result.status = SolveStatus::TimeLimit;
        result.killed = true;
        return result;
    }
    if (contains(log, "Optimal solution found")) result.status = SolveStatus::Optimal;
    else if (contains(log, "Time limit reached") || contains(log, "Interrupt request received") ||
             contains(log, "Solve interrupted"))
        result.status = SolveStatus::TimeLimit;
    else if (contains(log, "Model is infeasible")) result.status = SolveStatus::Infeasible;
    if (!exit.exited)
        return err("gurobi_cl was terminated by a signal, see " + job.log_file);
    if (exit.code != 0)
        return err("gurobi_cl exited with status " + std::to_string(exit.code) + ", see " +
                   job.log_file);

    auto const bound_at = log.rfind("best bound ");
    if (bound_at != std::string::npos) {
        result.best_bound = std::strtod(log.c_str() + bound_at + 11, nullptr);
        result.has_bound = true;
    }

    std::ifstream sol(job.result_file);
    if (!sol) return result; // no solution file: no feasible solution found
    for (std::string line; std::getline(sol, line);) {
        if (line.empty()) continue;
        if (line[0] == '#') {
            auto const eq = line.find("Objective value =");
            if (eq != std::string::npos)
                result.objective = std::strtod(line.c_str() + eq + 17, nullptr);
            continue;
        }
        std::istringstream fields(line);
        std::string name;
        double v = 0;
        if (fields >> name >> v) {
            if (v != 0) result.value.emplace(std::move(name), v);
        }
    }
    result.has_solution = true;
    return result;
}
