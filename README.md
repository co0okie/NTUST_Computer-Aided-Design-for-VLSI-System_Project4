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
