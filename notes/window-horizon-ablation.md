# Bounded window/horizon diagnostic

Exactly eight formal runs: aoi_C1355:2,2,1 and codex_gap:1,1,1, each with four combinations. No production source or common tool changes. This single stage finds no C1355 latency improvement or new proof. Recursive resource windows substantially shrink the model and reach root barrier/crossover; plain path windows spend the budget in presolve. The tightened runs converge in barrier and then time out during crossover.

## Matched design and attribution

All variants use the identical codex heuristic/source pinned to `b376c9b`; baseline C1355 UB=185, gap UB=4. Lower bounds remain the identical codex baseline bound (178 and 3). The model extends `notes/compact_ilp.cpp`, using cumulative binary z(i,t)=1 iff operation i starts by t. Monotonicity, cumulative precedence and first differences for resource rows are unchanged.

Two factors only:

- `path`: ASAP/ALAP windows. `resource`: intersect these windows with recursive resource heads/tails adopted from **claude@55567d1**, `src/bounds.{cpp,hpp}` and `src/opgraph.{cpp,hpp}`. Original operation IDs are mapped through `graph.node`. Heads are reverse-graph tails+1; deadlines are H-tail. Each closure is capped at 64MiB, with the source's path-height fallback.
- `min`: minimize integer L at H=UB with the full valid heuristic MIP start. `feas`: solve only H=UB−1 with constant-zero objective and **no feasible MIP start**. The baseline schedule cannot fit H, so this absence is intrinsic to the comparison. No iterative horizon search or stronger heuristic was added.

All runs use Method=2, Presolve=2, DualReductions=0, Threads=4, MIPGap=0, inspired by claude@55567d1. These settings are fixed; there was no parameter search. No C1355:3,2,2 run was repeated.

For a descendant set whose kind-k operations each have tail≥q, all occur after i and the last needs q further steps. Consequently tail(i)≥ceil(count/capacity)+q. Recursive application preserves a lower bound; reversing the graph gives valid heads. These windows restrict feasible schedules without changing the scheduling problem.

## Formal results

C1355, original UB=185, lower bound=178. All outputs pass the independent checker. Every C1355 returned schedule is the validated heuristic fallback, latency185; none proves optimality. `Nodes` below copies Gurobi's explored-node summary (1 is the root, not completed branching).

| Windows / objective | H | Binary vars | Rows | LP bytes | Wall s | Presolve s | Root/crossover observation | Nodes |
|---|---:|---:|---:|---:|---:|---:|---|---:|
| path / min L |185|100,219|256,898|13,001,696|51.60|51.02|root unobserved|0|
| resource / min L |185|58,982|143,280|7,231,071|51.34|34.12|barrier converged; crossover timed out|1|
| path / feasibility |184|99,576|255,203|12,859,900|51.49|50.90|root unobserved|0|
| resource / feasibility |184|58,339|141,618|7,125,286|51.29|35.06|barrier converged; crossover timed out|1|

Resource windows reduce binaries by about41% and rows by44%. UB−1 removes only643 binaries in either window variant, corresponding to one time point per operation. The observed model/phase gain comes from windows; this comparison does not show a benefit from the one-step horizon reduction.

The resource/min barrier reaches objective178 in17 iterations at **cumulative solver time42.13s**. The resource/feas barrier reaches objective0 in17 iterations at **cumulative solver time42.99s**. Those timestamps include earlier presolve; they are not additional barrier durations. Both then enter `Root crossover log`, still have903/1422 primal pushes remaining near50s, and report `Crossover changed status from Optimal to Time Limit`. Root-relaxation summaries report time limit after16.56/15.44s, with58,369/57,085 simplex iterations. Thus the tightened runs' remaining bottleneck is unfinished crossover after long presolve, not failure of barrier convergence. A root LP objective of0 in the feasibility model does not establish integer feasibility. Plain-window root work is unobserved, not measured as zero.

For codex_gap all four return latency3, lower bound3, checker OK, in0.017–0.032s. In table order their binary counts are8/6/4/2 and rows18/10/7/2. Min-L and feasibility raw solver reasons are `Optimal solution found`; global proof follows from the independent matching lower bound3. Detailed phase lines, including barrier/crossover/push counts, are tracked in `window-horizon-phase-excerpts.txt`.

## Status semantics and validation

The zero objective's raw solver `optimal` means a feasible schedule at H, not globally minimum latency. A feasible solution without a matching scheduling lower bound is **feasible but unproven**, represented by `ExactStatus::TimeLimit` solely for existing interface compatibility; this label need not imply a solver timeout. The separate raw solver field/log establishes the real termination reason. No such ambiguous result occurred in these48 validation/eight formal runs. Feasibility infeasibility at H=UB−1 proves the original UB globally optimal; no solution on timeout never proves infeasibility. Empty windows are accepted only from the sound heads/tails. Min-L preserves its ordinary optimality interpretation. Every candidate remains subject to integrality, finiteness, monotonicity, resource/precedence validation, and ≤heuristic latency.

`check_window_horizon.py` independently generates ten deterministic tiny DAGs where baseline h>subset-DP optimum. All40 models actually exercise Gurobi. Four additional controls use four independent NOT operations with capacity1, h=OPT=4 and forced H=3; both feasibility modes return solver `infeasible` and correctly prove UB=4 (no empty-window shortcut). Four codex_gap preflights complete48 checker/DP checks. The force environment is validation-only and bypasses the early lower-bound shortcut; formal runs do not use it. Inputs, stdout/stderr, LP/start where valid/solution where available, logs and command metadata remain on disk.

`verify_window_horizon.py` rechecks source and artifact hashes and independently reruns check.py on all56 retained outputs; it performs no scheduling/solver run. The validation script resumes existing records; use a fresh artifact directory to independently rerun all tests.

## Reproduction and retained evidence

Run from repository root, with Gurobi13.0.3 installed at /opt/gurobi:

```sh
python3 notes/window_horizon_ablation.py prepare
python3 notes/check_window_horizon.py
python3 notes/window_horizon_ablation.py formal
python3 notes/verify_window_horizon.py
```

For a fresh reproduction, first move the existing `shared/tmp/codex/window-horizon` directory to a separate disk location. The harness refuses to overwrite formal run directories. `prepare`'s recorded first build was clean and took6.80s, pinned on CPU4–7 with four build jobs; a future prepare in an existing directory may be incremental. Formal elapsed times exclude build, as tools/eval.py also reports build separately. This is a dedicated diagnostic harness, not tools/eval.py or a fork of it: each run uses CPU4–7, --threads4, --time55 and subprocess outer timeout60. The normal wall-clock SIGINT/SIGKILL guards remain. No outer timeout or SIGKILL occurred; Gurobi's own time limit terminated the four C1355 solves around51s. Memory and raw files are kept off tmpfs under the ignored disk shared directory.

`window-horizon-source-manifest.json` pins complete snapshot hashes and exact commits. `window-horizon-runs.json` and `window-horizon-validation.json` record per-run commands, artifact hashes, model size, solver state and logs. `window-horizon-model-size.tsv` preserves explicit phase/termination observations; `window-horizon-validation.tsv` records DP/checker results. Eight compatible rows were appended to `notes/codex-results.tsv` with note `window-horizon-diag:<mode>`. Each formal directory contains stdout, stderr, checker output, LP, Gurobi log and `.sol` when a solution exists; feasibility runs intentionally have no `.mst`, and C1355 feasibility runs have no `.sol` because Gurobi found no integer solution. The tracked excerpts retain important solver evidence without adding megabytes of raw artifacts to git.

Single runs of one benchmark cannot establish general performance superiority. The baseline UB185 is deliberately fixed and differs from claude's stronger heuristic, so these feasibility horizons do not reproduce its UB−1=178 infeasibility proof. No claim about the best possible feasibility model or solver tuning follows. Production remains scalar and this bounded stage ends here.
