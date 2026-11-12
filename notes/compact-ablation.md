# Compact cumulative ILP ablation

This experiment replaces only the scalar control's `src/ilp.cpp` in a pinned
source snapshot. Production `src/` is untouched. Base source is codex merge
`b376c9b`; exact base commit and SHA-256 hashes are in
`compact-source-manifest.json`. The replacement is `compact_ilp.cpp`.

Step-variable mathematics was motivated by `notes/codex.md` and
`claude@ce1d6e4` / shared Q6; the C++ implementation is independent. This
experiment retains the control's latency-minimizing `L` objective, ASAP/ALAP
windows at the heuristic upper bound, resources, and heuristic MIP start. It
does not implement Q6's fixed-horizon feasibility sequence or recursive
resource windows. Therefore its results isolate the representation change.

Let `z[i,t]=1` iff operation `i` starts no later than step `t`. Only
`early[i] <= t < late[i]` are variables; earlier values are constant zero and
later values constant one. Monotonicity permits exactly one zero-to-one
transition. Precedence is `z[v,t] <= z[u,t-1]`; resource use at `t` is
`sum(z[i,t]-z[i,t-1]) <= capacity`. Constants are moved to the right-hand side.
For sinks, `L + sum_t z[i,t] >= late[i]` uses the identity
`start[i] = late[i] - sum_t z[i,t]`. The MIP start contains every variable's
zero or one value from the heuristic. Decoding checks finite near-binary values,
monotonicity, precedence/resources through `validate`, and latency no larger
than the heuristic. Solver/model failures retain the validated heuristic.

Reproduce from the repository root (stdlib Python, C++17, CMake, CLI Gurobi):

```
python3 notes/compact_ablation.py
taskset -c 4-7 cmake -S shared/tmp/codex/compact-src -B shared/tmp/codex/compact-build
taskset -c 4-7 cmake --build shared/tmp/codex/compact-build -j2
python3 notes/check_compact.py
TMPDIR="$PWD/shared/tmp/codex" MLRCS_ABLATION_META="$PWD/shared/tmp/codex/compact-model-size.tsv" taskset -c 4-7 python3 tools/eval.py --src shared/tmp/codex/compact-src --cpu 4-7 --threads 4 --mode e aoi_C1355:2,2,1 aoi_C1355:8,8,4 codex_gap:1,1,1 --tsv notes/codex-results.tsv --note compact-cumulative-ablation
```

`check_compact.py` validates 31 tiny DAG cases against an independent subset-DP
optimum and `tools/check.py`. Twenty cases terminate by the lower bound and
**eleven actually exercise Gurobi and the compact formulation**: the handmade
gap and ten deterministic random DAGs selected because heuristic latency exceeds
the independent optimum. All 31 passed, including all eleven modeled cases.
Detailed results are `compact-validation.tsv`. Generated inputs, schedules, error
logs, and the kept gap LP/MIP-start/solution live under
`shared/tmp/codex/compact-validation/`; these ignored scratch files are not
required to regenerate the test.

Formal evaluations use the unchanged `tools/eval.py`, which independently invokes
`check.py` for every output before recording `OK`; its work directory and raw
schedule are removed by the evaluator. No duplicate formal run is made merely
to retain stdout. The model-size table preserves diagnostics emitted immediately
before solver launch, not a solver peak-memory measurement. `eval.py` labels
snapshots via their surrounding main worktree; the source manifest, not its
misleading `commit` label, identifies the code for this ablation.

Completed formal results (CPU 4–7, Gurobi 4 threads, default total budget 55 s):

| case | scalar control latency | expanded-prefix latency | compact latency | compact status | compact runtime |
|---|---:|---:|---:|---|---:|
| C1355:2,2,1 | 185 | 185 (model cap) | 185 | timelimit | 51.52 s |
| C1355:8,8,4 | 48 | 49 | 50 | timelimit | 51.16 s |
| codex_gap:1,1,1 | 3 | 3 | 3 | optimal | 0.02 s |

All three compact formal outputs independently passed `check.py`; total recorded
runtime was 102.70 s, clean build 13.2 s. All latencies are no larger than the
same baseline heuristic's values. Rows are appended to `codex-results.tsv` with
note `compact-cumulative-ablation`; prior scalar/prefix rows are not rewritten.
These are single runs under a time limit; the experiment does not establish a
statistical ordering of solver runtime or claim an LP-relaxation measurement.

Model sizes show why compactness alone is insufficient:

| case | scalar binary / rows / bytes | compact binary / rows / bytes |
|---|---:|---:|
| C1355:2,2,1 | 100862 / 2243 / 7723430 | 100219 / 256898 / 13001702 |
| C1355:8,8,4 | 14057 / 1804 / 999315 | 13414 / 32671 / 1592265 |
| codex_gap | 12 / 15 / 732 | 8 / 18 / 675 |

The compact C1355:2,2,1 model can be built within the text cap whereas the
expanded prefix model could not. However, it retains 100k binaries and expands
to 257k constraints; it did not improve latency within this run. At resources
8,8,4 it returned the heuristic 50, versus scalar 48 and expanded prefix 49.
Production remains the scalar model pending stronger evidence. Fixed-horizon
feasibility, stronger windows, solver-method comparisons and full-suite
integration were intentionally not performed in this bounded stage.
