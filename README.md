# mlrcs — latency-minimizing resource-constrained scheduling (ML-RCS)

Build (Linux, GCC with C++17, CMake >= 3.16):

```
cmake -B build
cmake --build build
```

The program is `build/mlrcs`:

```
./build/mlrcs -h BLIF_FILE AND_CONSTRAINT OR_CONSTRAINT NOT_CONSTRAINT
./build/mlrcs -e BLIF_FILE AND_CONSTRAINT OR_CONSTRAINT NOT_CONSTRAINT
```

- `-h` runs the heuristic (list scheduling).
- `-e` runs the exact method: an ILP solved with Gurobi. `gurobi_cl` must be on `PATH` (or in
  `$GUROBI_HOME/bin`) and runnable, i.e. `LD_LIBRARY_PATH` must include Gurobi's `lib`
  directory; otherwise `mlrcs -e` stops with an error message.

The schedule is printed to stdout in the assignment's format. Errors go to stderr, with exit
status 1.

Optional arguments:

| Option | Default | Meaning |
|---|---|---|
| `--time SECONDS` | 55 | Wall-clock budget for the whole run; on timeout `-e` prints the best schedule found |
| `--threads N` | Gurobi's choice | Gurobi threads |
| `--keep DIR` | (none) | Keep the ILP model, MIP start, solution and Gurobi log in `DIR` |
| `--stats` | off | Print one line of statistics (latency, lower bound, ILP status, time) to stderr |

Algorithm notes:

- The heuristic uses list scheduling by longest remaining path, then BLIF definition order.
- A resource-window lower bound uses per-kind unit-job EDF scheduling. A memory/work-capped
  ancestor/descendant analysis strengthens the windows; larger inputs use path-only windows.
- The exact model uses time-indexed binary variables, precedence and per-cycle resource
  constraints, with the heuristic as upper bound and MIP start. If its latency meets the lower
  bound, no model is needed. Models above 1,000,000 binaries or 128 MiB of LP text are skipped.
- Solver failures, timeouts, or invalid candidate schedules retain the validated heuristic.
  A successful run can therefore be feasible without proving optimality; inspect `--stats`.
- For disk-backed scratch space, set `TMPDIR` or use `--keep DIR`.

Small reproducible counterexample (four operations):

```
./build/mlrcs -h benchmark/codex_gap.blif 1 1 1
./build/mlrcs -e benchmark/codex_gap.blif 1 1 1
```

The heuristic has latency 4; Gurobi proves latency 3. Regenerate the input with
`python3 notes/generate_codex_gap.py`. Research data, validation scripts and report figures are
in `notes/`; only CMake and `src/` are needed to build the program.
