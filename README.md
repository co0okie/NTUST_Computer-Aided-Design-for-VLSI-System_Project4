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

The directory `testcase/` holds a circuit where `-h` and `-e` give different latencies (see
`testcase/README.md`). `tools/eval.py` and `tools/check.py` evaluate runs and verify outputs.
