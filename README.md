# mlrcs — latency-minimizing resource-constrained scheduling (ML-RCS)

Build (Linux, GCC with C++17, CMake >= 3.18). The exact method (`-e`) links Gurobi's C API, so
the build needs the environment variable `GUROBI_HOME` pointing at a Gurobi installation:

```
export GUROBI_HOME=/opt/gurobi
cmake -B build
cmake --build build
```

CMake uses `$GUROBI_HOME/include/gurobi_c.h` and the newest `$GUROBI_HOME/lib/libgurobiNNN.so`.
The library path is embedded in the build tree (RPATH), so `LD_LIBRARY_PATH` is not needed at run
time. Without `GUROBI_HOME` the program still builds and `-h` works, but `-e` prints an error
and exits with status 1. Running `-e` also needs a valid Gurobi license.

The program is `build/mlrcs`:

```
./build/mlrcs -h BLIF_FILE AND_CONSTRAINT OR_CONSTRAINT NOT_CONSTRAINT
./build/mlrcs -e BLIF_FILE AND_CONSTRAINT OR_CONSTRAINT NOT_CONSTRAINT
```

- `-h` runs the heuristic (list scheduling).
- `-e` runs the exact method: an ILP solved with Gurobi, starting from the heuristic schedule.

Example, run from the build directory as the course grader does:

```
cd build
./mlrcs -h sample02.blif 2 1 1
```

The schedule is written to `<BLIF base name>_h.out` (for `-h`) or `<BLIF base name>_e.out` (for
`-e`) in the current directory, e.g. `sample02_h.out`, and is also printed to stdout. For `-e`,
the output file first holds the heuristic schedule and is replaced by the ILP result, so it always
holds a valid schedule even if the run is cut short. Errors go to stderr, with exit status 1.

Optional arguments:

| Option | Default | Meaning |
|---|---|---|
| `--time SECONDS` | 55 | Wall-clock budget for the whole run; on timeout `-e` keeps the best schedule found |
| `--threads N` | Gurobi's choice | Gurobi threads |
| `--keep DIR` | (none) | Write the ILP model (`model.lp`) and the Gurobi log (`gurobi.log`) to `DIR` |
| `--stats` | off | Print one line of statistics (latency, lower bound, ILP status, time) to stderr |

Algorithm notes:

- The heuristic retains the original height-priority schedule, adds resource-aware seed
  scheduling and fixed-seed priority restarts, then searches resource-lane orders near critical
  paths. It retains the best validated schedule. Source attribution: notes/heuristic-sources.md.
- A resource-window lower bound uses per-kind unit-job EDF scheduling. A memory/work-capped
  ancestor/descendant analysis strengthens the windows; larger inputs use path-only windows.
- The exact model uses time-indexed binary variables, precedence and per-cycle resource
  constraints, with the heuristic as upper bound and MIP start. If its latency meets the lower
  bound, no model is needed. Models above 1,000,000 binaries or 16 million constraint nonzeros are skipped.
- Solver failures, timeouts, or invalid candidate schedules retain the validated heuristic.
  A successful run can therefore be feasible without proving optimality; inspect `--stats`.
- The output file holds a fast legal baseline before the longer heuristic search starts.

Small reproducible counterexample (four operations):

```
./build/mlrcs -h benchmark/codex_gap.blif 1 1 1
./build/mlrcs -e benchmark/codex_gap.blif 1 1 1
```

The original height-priority heuristic had latency 4 and Gurobi proved 3. The stronger
heuristic may now also reach 3; this historical case must be rechecked before submission.
Regenerate the input with
`python3 notes/generate_codex_gap.py`. Research data, validation scripts and report figures are
in `notes/`; only CMake and `src/` are needed to build the program.

The directory `testcase/` holds the shared candidate difference circuit; recheck it with this
branch before submission (see
`testcase/README.md`). `tools/eval.py` and `tools/check.py` evaluate runs and verify outputs.
