# Hard ML-RCS suite

Circuits (AND/OR/NOT only) where critical path and resource limits pull against each other, so the
simple bounds and plain list scheduling are not enough. `manifest.tsv` lists every case
(`<name>:A,O,N`) with generator arguments, size, depth, kind counts, `h0_latency` (`-h --htime 0`),
`h_latency` (default `-h`), `best_latency`, `lower_bound`, `status` (`optimal` = best == bound,
otherwise `open`) and the mlrcs commit the numbers were measured with. All latencies come from
outputs that passed `tools/check.py`.

## Families (`gen_hard.py`)
- `layered`: random layered DAG, all three kinds mixed in each layer (per-layer kind mix when
  `phased=1`), fan-in 2-3, edges mostly from the previous `back` layers; width profile
  uniform / diamond / hourglass.
- `xor`: XOR (= AND(OR(a,b), NOT(AND(a,b)))) based parity trees, adders, multipliers, comparators, ALU slices.
- `chains`: several long chains whose kind pattern is phase shifted, cross-linked, plus wide shallow side logic.
- `funnel`: many feeders of one kind feeding overlapping groups of another kind, several stages, deep tail.

## Regenerate
    python3 benchmark/hard/gen_hard.py --all                 # rewrites every .blif listed in manifest.tsv
    python3 benchmark/hard/gen_hard.py --all --out DIR       # same, into DIR (then `cmp` with benchmark/hard)
    python3 benchmark/hard/gen_hard.py layered seed=17 layers=20 width=8 > x.blif   # a single circuit

`search.py` (candidate generation, limit selection, offline `-e` runs) and `build_manifest.py`
(suite selection, manifest) are the pipeline that produced the suite; they are not needed to use it.

## Evaluate
    python3 tools/eval.py --mode h $(cut -f1 benchmark/hard/manifest.tsv | tail -n +2 | sed 's|^|benchmark/hard/|; s|:|.blif:|' | tr '\n' ' ')

(`case` in the manifest is `<name>:A,O,N`; the command above turns it into `benchmark/hard/<name>.blif:A,O,N`.)
Replace `--mode h` by `--mode e` for the ILP. The best known latencies are also in `shared/best.tsv`.
