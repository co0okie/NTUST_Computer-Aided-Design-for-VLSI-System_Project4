# Heuristic source attribution

The following files are adopted from the parallel research branch `claude`, pinned commit `f275c81`:

- `src/seed_heuristic.cpp` ← `src/heuristic.cpp` (include renamed).
- `src/seed_heuristic.hpp` ← `src/heuristic.hpp`.
- `src/bounds.cpp` and `src/bounds.hpp` ← same paths.
- `src/opgraph.cpp` and `src/opgraph.hpp` ← same paths.

The original Netlist height-priority list schedule is retained as an additional incumbent in `src/heuristic.cpp`. The resource-lane order search in `src/resource_order.cpp` is a new codex implementation. The original scalar ILP, Netlist, parser and independent schedule validator remain in use.

Local changes to the adopted source: `bounds.cpp` declines closure construction when
the estimated edge/word work or maximum descendant-pair work exceeds 100 million;
`seed_heuristic.cpp` checks the shared deadline between unlock passes. Empty closure
uses the adopted path-tail/list scheduling fallback. These guards bound pathological
deep/dense input preprocessing without changing the hard-suite graphs' closure use.

Research controls in the new wrapper: `MLRCS_ORDER_SEARCH=0` disables only the new
resource-order pass; `MLRCS_ORDER_DIAGNOSTICS=1` prints its counters to stderr.
`MLRCS_ORDER_LOG=/path/to/file.tsv` appends one headerless row per heuristic call,
including calls skipped at a bound or deadline and calls with the pass disabled.
Columns: `model`, `and`, `or`, `not`, `seed_latency`, `final_latency`, `attempts`,
`feasible`, `accepted`, `enabled` (0/1), `reason` (search/bound/budget/empty/disabled).
The seed and final values belong to the same run, allowing a causal comparison
without relying on equal wall-clock restart trajectories in separate processes.
