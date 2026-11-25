# 11/19 output-file and Gurobi C API migration

Base: codex `1cd27a5`; merge main `236f4b9`. The authoritative requirements are
`docs/Project4 Requirement/Project4 Requirement.md`. This stage changes the build,
solver interface and output contract; it does not introduce a new scheduler or formulation.

- CMake and the thin `Gurobi` wrapper come from main `236f4b9`: discover the C header
  and versioned shared library only through `GUROBI_HOME`, link the C API with build RPATH,
  and compile an explicit `-e` error path when Gurobi is absent. `-e` starts the environment
  and checks licensing before scheduling. The wrapper updates pending variables before
  setting the MIP start; solve has both TimeLimit and a GRBterminate watchdog.
- `main.cpp` uses main's atomic temporary-file/rename writer. Both modes first validate and
  write a quick height-priority baseline to the cwd's `<stem>_h.out` / `<stem>_e.out`, then
  start HardDeadline. The unchanged longer heuristic publishes validated strict improvements
  after its seed, restart and resource-order phases. Stdout remains a convenience copy.
- The production ILP remains the old time-indexed minimize-L model: path ASAP/ALAP(UB),
  one-hot assignments, expected-start precedence, per-kind resource rows, sink latency rows,
  heuristic UB and MIP start, and existing default solver parameters. The model is inserted
  directly through the C API; no gurobi_cl, scratch subprocess or LP parsing remains in src.
  `--keep` can still export a diagnostic LP and solver log.
- The existing one-million-binary guard remains. The old 128 MiB LP-text guard is replaced
  by a conservative pre-count of 16 million constraint nonzeros, plus model-building deadline
  checks. These are **not equivalent guards**: the former measures serialization bytes and
  the latter bounds C API/solver storage. Resource-row terms are counted conservatively even
  when a row will be redundant. Construction buffers are released before optimize.
- Only a validated integral one-hot solution no worse than the heuristic is accepted.
  Budget exits, model/solver errors and invalid candidates retain the heuristic with
  `timelimit`; only a completed optimal solve (or the existing valid lower bound) proves
  optimality. An optimal solve also updates the reported lower bound to its latency.
- CLI rejects nonfinite/oversized time budgets (maximum 3600 seconds, preventing chrono
  duration overflow) and out-of-range thread counts. Capacities accept signed 32-bit maxima.
- The merged evaluation tool checks the graded output file and records NO_OUTPUT when absent.
  Existing `--keep-outputs` additionally preserves that actual file and its SHA-256 alongside
  stdout, stderr and run metadata, including error/timeout/missing-output branches.

Verification and before/after runtime/RSS figures are recorded separately by the representative
suite harness. `tests/check_output_contract.py` exercises short budgets, no gurobi_cl on PATH,
no-Gurobi builds, invalid CLI values, and killing a long heuristic after its early atomic output
appears; every expected-success output is independently checked. Historical note-only LP
experiments remain archival material and are not part of production or this migration.

Local contract smoke (CPU 4–7) passed after both Release builds, with and without
`GUROBI_HOME=/opt/gurobi`. Reproduce after configuring those two build directories:

```
taskset -c 4-7 python3 tests/check_output_contract.py \
  shared/tmp/codex/resume2-build/mlrcs shared/tmp/codex/resume2-no-gurobi/mlrcs \
  shared/tmp/codex/resume2-contract-new
```

All nine checks passed. The early-kill output is a fully checker-valid height baseline;
no-Gurobi `-h` remains usable and `-e` fails with a clear nonzero error. Short-budget C API
`-e` initializes licensing and writes a valid fallback with no `gurobi_cl` on PATH; actual
optimization is covered by the representative suite, not claimed by this smoke. Original
logs and independent checker evidence are in `shared/tmp/codex/resume2-contract/summary.json`.
The source has been frozen for the before/after suite. `testcase/claude_gap4.blif` already
has `-h` = `-e` = 25 in the pre-migration codex baseline, so this branch cannot submit it
as a difference case without a later replacement; this stage performs no new case search.
