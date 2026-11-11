#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "result.hpp"

// Gurobi is used through its command-line tool, gurobi_cl: we write an LP file, run gurobi_cl
// as a child process and read back its solution file. No Gurobi headers or libraries are linked.

// Finds a working gurobi_cl: first on PATH, then in $GUROBI_HOME/bin. "Working" means
// `gurobi_cl --version` exits with status 0 (catches a missing LD_LIBRARY_PATH, for example).
Result<std::string> find_gurobi_cl();

// A scratch directory for the LP/solution/log files. Removed on destruction unless `keep`.
class ScratchDir {
public:
    // Creates `requested` if given (kept afterwards), else a fresh directory under TMPDIR.
    static Result<ScratchDir> create(std::string const &requested);
    ScratchDir(ScratchDir &&other) noexcept;
    ScratchDir &operator=(ScratchDir &&) = delete;
    ScratchDir(ScratchDir const &) = delete;
    ~ScratchDir();

    std::string const &path() const { return path_; }
    std::string file(std::string const &name) const { return path_ + "/" + name; }

private:
    ScratchDir(std::string path, bool keep) : path_(std::move(path)), keep_(keep) {}
    std::string path_;
    bool keep_ = false;
};

struct GurobiJob {
    std::string model_file;  // .lp file to solve
    std::string start_file;  // optional MIP start (.mst), empty for none
    std::string result_file; // where gurobi_cl writes the solution (.sol)
    std::string log_file;    // gurobi_cl stdout and stderr
    std::vector<std::pair<std::string, std::string>> params; // e.g. {"TimeLimit", "50"}
    // Seconds from launch. Gurobi checks TimeLimit only now and then (rarely during presolve and
    // the root LP), so it is interrupted with SIGINT (it then stops and writes its best solution)
    // and, if it still runs, killed.
    double interrupt_after = 60;
    double kill_after = 62;
};

enum class SolveStatus { Optimal, TimeLimit, Infeasible, Other };

char const *status_name(SolveStatus s);

struct GurobiResult {
    SolveStatus status = SolveStatus::Other;
    bool killed = false; // had to be killed; there is no solution
    bool has_solution = false;
    double objective = 0;
    double best_bound = 0; // from the log's final "best bound" (valid when has_bound)
    bool has_bound = false;
    std::unordered_map<std::string, double> value; // nonzero variables from the solution file
};

Result<GurobiResult> run_gurobi(std::string const &gurobi_cl, GurobiJob const &job);
