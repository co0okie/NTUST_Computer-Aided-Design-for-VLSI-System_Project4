# Direct cumulative-prefix precedence ablation

## Result and decision

Six matched formal runs completed before the session restart. Every run passed
`tools/check.py` through `tools/eval.py`. The direct cumulative-prefix formulation
showed no latency benefit on this three-case subset, so production keeps the
scalar expected-start precedence model.

| Case | Scalar latency / status | Cumulative latency / status | Scalar / cumulative wall seconds |
|---|---|---|---|
| aoi_C1355:2,2,1 | 185 / timelimit | 185 / too_large | 52.40 / 0.72 |
| aoi_C1355:8,8,4 | 48 / timelimit | 49 / timelimit | 51.15 / 52.85 |
| codex_gap:1,1,1 | 3 / optimal | 3 / optimal | 0.02 / 0.02 |

Heuristic latencies were 185, 50, and 4 respectively. Lower bounds were unchanged:
178, 46, and 3 after the optimal gap solve. Each run stayed below the external
60-second limit. Clean builds took 13.7 seconds for scalar and 13.2 seconds for
cumulative. Runtime is recorded as a hard-limit check and report measurement;
the decision is based on latency and model feasibility within that limit.

For C1355:8,8,4 both models have 14,057 binary variables. Scalar has 1,804 labelled
constraints and 999,315 LP bytes; cumulative has 20,543 labelled constraints and
5,229,466 LP bytes (about 11.39 times the rows and 5.23 times the text).
The cumulative C1355:2,2,1 model returned `too_large` before the size diagnostic
point. Its final text size was not recorded. The same windows give 100,862
binaries in the scalar model, below the one-million-binary guard; because only
precedence changed, the cumulative LP text guard of 128 MiB caused the skip.
This cause is inferred from the unchanged variable construction and guard code,
not from a saved complete cumulative LP file.

`cumulative-model-size.tsv` contains the five completed-model size diagnostics
with explicit case names and headers. Labelled constraints are counted by LP
label separators, rather than by querying a presolved Gurobi model. No LP
relaxation objective, presolve reduction, or root-LP strength was measured in
this experiment. The observed latency comparison is one matched run per variant
and case, not a statistical claim about solver performance.

## Controlled change and boundary proof

For each operation edge u -> v, scalar precedence is
`sum_s s*x[v,s] - sum_s s*x[u,s] >= 1`. The experimental replacement uses
`sum_{s<=t} x[v,s] <= sum_{s<=t-1} x[u,s]` at every nontrivial threshold t.
All other model terms, latency objective, ASAP/ALAP windows, resource constraints,
heuristic MIP start, solver options, fallback, and construction guards are the
same. Both snapshots add the same optional size-diagnostic hook.

With one-hot binary assignments, taking t equal to the chosen start of v forces
u to start at least one cycle earlier. Conversely, strict integer precedence
satisfies every threshold inequality. Thus the integer feasible schedules are
identical. For fractional assignments the cumulative inequalities imply the
expected-start inequality, so the mathematical relaxation is at least as strong;
this theoretical relation does not establish practical solve improvements.

Only thresholds `early[v] <= t <= late[u]` need rows. For smaller t the child
prefix is zero; for larger t the parent prefix is one. ASAP/ALAP consistency on
an edge gives `early[u] < early[v]` and `late[u] < late[v]`, so every generated
prefix is partial and needs no omitted constant-one boundary term. Edges whose
windows are already ordered need no row. The implementation expands each prefix
as x terms, which can require quadratic work in window width per edge.

## Source provenance and reproduction

The historical source is **b376c9bfb4d239322b802c307cf005f57c984b48** (codex's merge
of main@7344de9). The six TSV rows report `7344de9`: their source snapshots lived
under `shared/`, a symlink outside the codex worktree, and eval.py's Git lookup
found the enclosing main checkout. The original TSV is preserved unchanged.
At closeout, both retained snapshots were compared byte-for-byte against the
scripted transformation of b376c9b: all source files and CMakeLists matched.
`cumulative-source-manifest.json` records the exact original and generated SHA256
hashes. The preparation script now defaults to this pinned source and accepts
`--source` and `--output`; later production changes cannot silently alter the
historical reproduction.

From the codex worktree root, prepare the snapshots:

```sh
python3 notes/cumulative_ablation.py --source b376c9b
```

The original formal evaluations can be reproduced with the following commands
(they are documented here; they were **not rerun** at closeout):

```sh
TMPDIR="$PWD/shared/tmp/codex" MLRCS_ABLATION_META="$PWD/shared/tmp/codex/cumulative-model-size.tsv" python3 tools/eval.py --src shared/tmp/codex/cumulative-src --cpu 4-7 --threads 4 --mode e aoi_C1355:2,2,1 aoi_C1355:8,8,4 codex_gap:1,1,1 --tsv notes/codex-results.tsv --note cumulative-precedence-ablation
TMPDIR="$PWD/shared/tmp/codex" MLRCS_ABLATION_META="$PWD/shared/tmp/codex/cumulative-model-size.tsv" python3 tools/eval.py --src shared/tmp/codex/scalar-src --cpu 4-7 --threads 4 --mode e aoi_C1355:2,2,1 aoi_C1355:8,8,4 codex_gap:1,1,1 --tsv notes/codex-results.tsv --note scalar-precedence-control
```

Formal result rows are in `codex-results.tsv`, with the two note values above.
The diagnostic file is append-only; use a fresh path to avoid mixing repeated
runs. Gurobi is required only for executing the experiment, not for preparing
the snapshots. No Python packages are required.

## Validation retained at closeout

The original eval logs and six TSV records all have `check=OK`. Eval.py deletes
its per-case stdout and stderr after the batch, so the five other individual
historical outputs are unavailable for fresh rechecking. This does not replace
the original checker results with a new validation claim.

The retained standalone `shared/tmp/codex/cumulative-gap.out` was freshly checked:

```sh
python3 tools/check.py benchmark/codex_gap.blif 1 1 1 shared/tmp/codex/cumulative-gap.out --mode e
# OK latency=3
```

Its schedule is n1 at step 1; n0 and n3 at step 2; n2 at step 3. The source
preparation was also rerun into a separate reproduction-check directory, without
building or solving; both generated snapshots matched the historical snapshots
byte-for-byte. No benchmark suite or timed solve was repeated at closeout.
