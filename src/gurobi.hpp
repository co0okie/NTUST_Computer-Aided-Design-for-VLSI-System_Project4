#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "result.hpp"

// Gurobi through its C API (gurobi_c.h, libgurobiNNN.so), found through $GUROBI_HOME by CMake.
// Built without Gurobi (MLRCS_HAVE_GUROBI undefined), Gurobi::open returns an error.
// The header hides every Gurobi type, so the rest of the program compiles either way.

enum class VarType { Binary, Integer, Continuous };
enum class Sense { LessEqual, GreaterEqual, Equal };

enum class SolveStatus { Optimal, TimeLimit, Infeasible, Other };

char const *status_name(SolveStatus s);

struct SolveResult {
    SolveStatus status = SolveStatus::Other;
    bool has_solution = false;   // x holds a feasible solution
    double objective = 0;        // of that solution
    double bound = 0;            // best proven bound on the objective (minimization)
    std::vector<double> x;       // one value per variable, if has_solution
    double seconds = 0;          // wall time spent in optimize
};

struct GurobiImpl;

// One Gurobi environment with one model, minimizing. Console output is off.
class Gurobi {
public:
    // Starts the environment (license check). `log_file` empty: no Gurobi log. `threads` 0:
    // Gurobi's default.
    static Result<Gurobi> open(std::string const &log_file, int threads);
    Gurobi(Gurobi &&) noexcept;
    Gurobi &operator=(Gurobi &&) noexcept;
    Gurobi(Gurobi const &) = delete;
    Gurobi &operator=(Gurobi const &) = delete;
    ~Gurobi();

    // Adds `count` variables with the same bounds, objective and type; returns the index of the
    // first one. Indices are consecutive, starting at 0 for the first variable of the model.
    Result<std::int32_t> add_vars(std::int32_t count, double lb, double ub, double obj, VarType type);
    // sum(val[j] * x[ind[j]]) sense rhs
    Result<Unit> add_constr(std::vector<std::int32_t> const &ind, std::vector<double> const &val,
                            Sense sense, double rhs);
    std::int32_t num_vars() const;

    Result<Unit> set_int_param(char const *name, int value);
    Result<Unit> set_double_param(char const *name, double value);
    // MIP start: one value per variable
    Result<Unit> set_start(std::vector<double> const &start);
    // Writes the model (format from the extension, e.g. .lp or .mps), for debugging
    Result<Unit> write(std::string const &path);

    // Optimizes for at most `seconds` of wall time. Besides Gurobi's own TimeLimit, a watchdog
    // thread calls GRBterminate at the deadline, since TimeLimit is only checked now and then.
    Result<SolveResult> solve(double seconds);

private:
    explicit Gurobi(std::unique_ptr<GurobiImpl> impl);
    std::unique_ptr<GurobiImpl> impl_;
};
