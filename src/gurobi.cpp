#include "gurobi.hpp"

#include <chrono>
#include <cmath>
#include <limits>
#include <condition_variable>
#include <mutex>
#include <thread>

#ifdef MLRCS_HAVE_GUROBI
extern "C" {
#include <gurobi_c.h>
}
#endif

char const *status_name(SolveStatus s) {
    switch (s) {
    case SolveStatus::Optimal: return "optimal";
    case SolveStatus::TimeLimit: return "timelimit";
    case SolveStatus::Infeasible: return "infeasible";
    case SolveStatus::Other: return "other";
    }
    return "?";
}

#ifdef MLRCS_HAVE_GUROBI

struct GurobiImpl {
    GRBenv *env = nullptr;     // owned
    GRBmodel *model = nullptr; // owned
    std::int32_t vars = 0;

    GurobiImpl() = default;
    GurobiImpl(GurobiImpl const &) = delete;
    GurobiImpl &operator=(GurobiImpl const &) = delete;
    ~GurobiImpl() {
        if (model != nullptr) GRBfreemodel(model);
        if (env != nullptr) GRBfreeenv(env);
    }

    GRBenv *model_env() const { return GRBgetenv(model); }
    Error error(char const *what) const {
        char const *msg = GRBgeterrormsg(model != nullptr ? model_env() : env);
        return Error{std::string("Gurobi: ") + what + ": " + (msg != nullptr ? msg : "?")};
    }
};

namespace {

Result<Unit> check(GurobiImpl const &g, int code, char const *what) {
    if (code != 0) return g.error(what);
    return Unit{};
}

char grb_type(VarType t) {
    switch (t) {
    case VarType::Binary: return GRB_BINARY;
    case VarType::Integer: return GRB_INTEGER;
    case VarType::Continuous: return GRB_CONTINUOUS;
    }
    return GRB_CONTINUOUS;
}

char grb_sense(Sense s) {
    switch (s) {
    case Sense::LessEqual: return GRB_LESS_EQUAL;
    case Sense::GreaterEqual: return GRB_GREATER_EQUAL;
    case Sense::Equal: return GRB_EQUAL;
    }
    return GRB_EQUAL;
}

double clamp_bound(double v) {
    if (v >= 1e30) return GRB_INFINITY;
    if (v <= -1e30) return -GRB_INFINITY;
    return v;
}

} // namespace

Result<Gurobi> Gurobi::open(std::string const &log_file, int threads) {
    auto impl = std::make_unique<GurobiImpl>();
    if (GRBemptyenv(&impl->env) != 0 || impl->env == nullptr)
        return err("Gurobi: cannot create an environment");
    // Before starting the environment, so not even the license banner reaches stdout
    TRY_VOID(check(*impl, GRBsetintparam(impl->env, "LogToConsole", 0), "LogToConsole"));
    if (!log_file.empty())
        TRY_VOID(check(*impl, GRBsetstrparam(impl->env, "LogFile", log_file.c_str()), "LogFile"));
    if (threads > 0)
        TRY_VOID(check(*impl, GRBsetintparam(impl->env, "Threads", threads), "Threads"));
    if (GRBstartenv(impl->env) != 0) return impl->error("cannot start (license?)");
    TRY_VOID(check(*impl,
                   GRBnewmodel(impl->env, &impl->model, "mlrcs", 0, nullptr, nullptr, nullptr,
                               nullptr, nullptr),
                   "cannot create a model"));
    return Gurobi(std::move(impl));
}

Result<std::int32_t> Gurobi::add_vars(std::int32_t count, double lb, double ub, double obj,
                                      VarType type) {
    auto &g = *impl_;
    std::int32_t const first = g.vars;
    if (count <= 0) return first;
    std::vector<double> lbs(count, clamp_bound(lb)), ubs(count, clamp_bound(ub)), objs(count, obj);
    std::vector<char> types(count, grb_type(type));
    TRY_VOID(check(g,
                   GRBaddvars(g.model, count, 0, nullptr, nullptr, nullptr, objs.data(),
                              lbs.data(), ubs.data(), types.data(), nullptr),
                   "add variables"));
    g.vars += count;
    return first;
}

Result<Unit> Gurobi::add_constr(std::vector<std::int32_t> const &ind,
                                std::vector<double> const &val, Sense sense, double rhs) {
    auto &g = *impl_;
    if (ind.size() != val.size()) return err("Gurobi::add_constr: size mismatch");
    // int and std::int32_t are the same type on every platform we build for
    static_assert(sizeof(int) == sizeof(std::int32_t));
    return check(g,
                 GRBaddconstr(g.model, static_cast<int>(ind.size()),
                              const_cast<int *>(reinterpret_cast<int const *>(ind.data())),
                              const_cast<double *>(val.data()), grb_sense(sense), rhs, nullptr),
                 "add constraint");
}

Result<Unit> Gurobi::add_constrs(RowBatch const &rows) {
    auto &g = *impl_;
    auto const count = rows.beg.size();
    if (count == 0) return Unit{};
    if (rows.sense.size() != count || rows.rhs.size() != count || rows.ind.size() != rows.val.size())
        return err("Gurobi::add_constrs: size mismatch");
    if (rows.ind.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return err("Gurobi::add_constrs: too many nonzeros in one batch");
    std::vector<char> senses(count);
    for (std::size_t r = 0; r < count; ++r) senses[r] = grb_sense(rows.sense[r]);
    static_assert(sizeof(int) == sizeof(std::int32_t));
    return check(g,
                 GRBaddconstrs(g.model, static_cast<int>(count), static_cast<int>(rows.ind.size()),
                               const_cast<int *>(reinterpret_cast<int const *>(rows.beg.data())),
                               const_cast<int *>(reinterpret_cast<int const *>(rows.ind.data())),
                               const_cast<double *>(rows.val.data()), senses.data(),
                               const_cast<double *>(rows.rhs.data()), nullptr),
                 "add constraints");
}

std::int32_t Gurobi::num_vars() const { return impl_->vars; }

Result<Unit> Gurobi::reset_model() {
    auto &g = *impl_;
    if (g.model != nullptr) GRBfreemodel(g.model);
    g.model = nullptr;
    g.vars = 0;
    return check(g,
                 GRBnewmodel(g.env, &g.model, "mlrcs", 0, nullptr, nullptr, nullptr, nullptr,
                             nullptr),
                 "cannot create a model");
}

Result<Unit> Gurobi::set_int_param(char const *name, int value) {
    return check(*impl_, GRBsetintparam(impl_->model_env(), name, value), name);
}

Result<Unit> Gurobi::set_double_param(char const *name, double value) {
    return check(*impl_, GRBsetdblparam(impl_->model_env(), name, value), name);
}

Result<Unit> Gurobi::set_start(std::vector<double> const &start) {
    auto &g = *impl_;
    if (static_cast<std::int32_t>(start.size()) != g.vars)
        return err("Gurobi::set_start: wrong number of values");
    if (g.vars == 0) return Unit{};
    return check(g,
                 GRBsetdblattrarray(g.model, GRB_DBL_ATTR_START, 0, g.vars,
                                    const_cast<double *>(start.data())),
                 "set MIP start");
}

Result<Unit> Gurobi::write(std::string const &path) {
    TRY_VOID(check(*impl_, GRBupdatemodel(impl_->model), "update model"));
    return check(*impl_, GRBwrite(impl_->model, path.c_str()), "write model");
}

Result<SolveResult> Gurobi::solve(double seconds) {
    auto &g = *impl_;
    auto const start = std::chrono::steady_clock::now();
    if (seconds < 0.1) seconds = 0.1;
    TRY_VOID(set_double_param("TimeLimit", seconds));

    // Watchdog: GRBterminate at the deadline (safe to call from another thread)
    std::mutex mutex;
    std::condition_variable done_cv;
    bool done = false;
    GRBmodel *const model = g.model;
    std::thread watchdog([&mutex, &done_cv, &done, model, start, seconds] {
        std::unique_lock<std::mutex> lock(mutex);
        auto const deadline = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                          std::chrono::duration<double>(seconds));
        if (!done_cv.wait_until(lock, deadline, [&done] { return done; })) GRBterminate(model);
    });
    int const rc = GRBoptimize(g.model);
    {
        std::lock_guard<std::mutex> lock(mutex);
        done = true;
    }
    done_cv.notify_all();
    watchdog.join();
    TRY_VOID(check(g, rc, "optimize"));

    SolveResult r;
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    int status = 0;
    TRY_VOID(check(g, GRBgetintattr(g.model, GRB_INT_ATTR_STATUS, &status), "status"));
    switch (status) {
    case GRB_OPTIMAL: r.status = SolveStatus::Optimal; break;
    case GRB_TIME_LIMIT:
    case GRB_INTERRUPTED: r.status = SolveStatus::TimeLimit; break;
    case GRB_INFEASIBLE:
    case GRB_INF_OR_UNBD: r.status = SolveStatus::Infeasible; break;
    default: r.status = SolveStatus::Other; break;
    }
    int solutions = 0;
    TRY_VOID(check(g, GRBgetintattr(g.model, GRB_INT_ATTR_SOLCOUNT, &solutions), "SolCount"));
    if (GRBgetdblattr(g.model, GRB_DBL_ATTR_OBJBOUND, &r.bound) != 0) r.bound = -GRB_INFINITY;
    r.has_bound = std::isfinite(r.bound) && std::fabs(r.bound) < GRB_INFINITY;
    if (solutions > 0 && g.vars > 0) {
        r.has_solution = true;
        TRY_VOID(check(g, GRBgetdblattr(g.model, GRB_DBL_ATTR_OBJVAL, &r.objective), "ObjVal"));
        r.x.resize(g.vars);
        TRY_VOID(check(g, GRBgetdblattrarray(g.model, GRB_DBL_ATTR_X, 0, g.vars, r.x.data()), "X"));
    }
    return r;
}

#else // no Gurobi at build time

struct GurobiImpl {
    std::int32_t vars = 0;
};

Result<Gurobi> Gurobi::open(std::string const &, int) {
    return err("this mlrcs was built without Gurobi; set GUROBI_HOME and rebuild to use -e");
}
Result<std::int32_t> Gurobi::add_vars(std::int32_t, double, double, double, VarType) {
    return err("no Gurobi");
}
Result<Unit> Gurobi::add_constr(std::vector<std::int32_t> const &, std::vector<double> const &,
                                Sense, double) {
    return err("no Gurobi");
}
std::int32_t Gurobi::num_vars() const { return impl_->vars; }
Result<Unit> Gurobi::add_constrs(RowBatch const &) { return err("no Gurobi"); }
Result<Unit> Gurobi::reset_model() { return err("no Gurobi"); }
Result<Unit> Gurobi::set_int_param(char const *, int) { return err("no Gurobi"); }
Result<Unit> Gurobi::set_double_param(char const *, double) { return err("no Gurobi"); }
Result<Unit> Gurobi::set_start(std::vector<double> const &) { return err("no Gurobi"); }
Result<Unit> Gurobi::write(std::string const &) { return err("no Gurobi"); }
Result<SolveResult> Gurobi::solve(double) { return err("no Gurobi"); }

#endif

Gurobi::Gurobi(std::unique_ptr<GurobiImpl> impl) : impl_(std::move(impl)) {}
Gurobi::Gurobi(Gurobi &&) noexcept = default;
Gurobi &Gurobi::operator=(Gurobi &&) noexcept = default;
Gurobi::~Gurobi() = default;
