#!/usr/bin/env python3
"""Short functional/checker smoke tests; run on the assigned CPU affinity.

Usage: taskset -c 4-7 python3 tests/check_resource_order.py build-order/mlrcs
No third-party packages. Scratch files live on disk under shared/tmp/codex.
"""
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(sys.argv[1]).resolve()
SCRATCH = ROOT / "shared/tmp/codex"
SCRATCH.mkdir(parents=True, exist_ok=True)
ENV = dict(os.environ, PATH="/opt/gurobi/bin:" + os.environ.get("PATH", ""),
           LD_LIBRARY_PATH="/opt/gurobi/lib:" + os.environ.get("LD_LIBRARY_PATH", ""))


def blif(operations):
    lines = [".model test", ".inputs a b", ".outputs n" + str(len(operations) - 1)]
    for i, (kind, fanins) in enumerate(operations):
        lines.append(".names " + " ".join(fanins) + f" n{i}")
        lines.extend({"and": ["11 1"], "or": ["1- 1", "-1 1"], "not": ["0 1"]}[kind])
    return "\n".join(lines + [".end", ""])


def check(directory, name, operations, limits, budget, modes=("h",)):
    path = directory / (name + ".blif")
    path.write_text(blif(operations))
    for mode in modes:
        output = directory / (name + mode + ".out")
        with output.open("w") as stream:
            run = subprocess.run([str(BINARY), "-" + mode, str(path), *map(str, limits),
                                  "--time", str(budget), "--threads", "1", "--stats"],
                                 stdout=stream, stderr=subprocess.PIPE, text=True,
                                 env=ENV, timeout=6)
        assert run.returncode == 0, (name, mode, run.stderr)
        subprocess.run([sys.executable, str(ROOT / "tools/check.py"), str(path),
                        *map(str, limits), str(output), "--mode", mode, "--quiet"],
                       check=True, timeout=6)


with tempfile.TemporaryDirectory(prefix="order-check-", dir=SCRATCH) as tmp:
    directory = Path(tmp)
    log = directory / "order.tsv"
    ENV["MLRCS_ORDER_LOG"] = str(log)
    ENV.setdefault("MLRCS_ORDER_METHOD", "beam")
    rng = random.Random(247)
    for sample in range(16):
        operations = []
        names = ["a", "b"]
        for i in range(18):
            kind = rng.choice(["and", "or", "not"])
            fanins = rng.sample(names, 1 if kind == "not" else 2)
            operations.append((kind, fanins))
            names.append(f"n{i}")
        check(directory, f"random{sample}", operations, (2, 2, 1), 0.4)
    # Very short budget must return the mandatory initial valid schedule.
    check(directory, "expired", operations, (1, 1, 1), 0.000001)
    check(directory, "capacity", operations, (10**9, 10**9, 10**9), 0.4)
    chain = [("not", ["a" if i == 0 else f"n{i-1}"]) for i in range(15000)]
    check(directory, "deep", chain, (0, 0, 1), 0.4)
    wide = [("not", ["a"]) for _ in range(15000)]
    check(directory, "wide", wide, (0, 0, 10**9), 0.4)
    # Normal and exact modes share the same seed/search entry point.
    check(directory, "both_modes", [("not", ["a"]), ("not", ["n0"])],
          (0, 0, 1), 0.4, ("h", "e"))
    ENV["MLRCS_ORDER_SEARCH"] = "0"
    check(directory, "disabled", operations, (1, 1, 1), 0.4)
    del ENV["MLRCS_ORDER_SEARCH"]
    bad = subprocess.run([str(BINARY), "-h", str(directory / "deep.blif"), "0", "0", "0"],
                         capture_output=True, text=True, timeout=6)
    assert bad.returncode != 0
    records = [line.split("\t") for line in log.read_text().splitlines()]
    assert len(records) == 23
    for row in records:
        assert len(row) == 11 and int(row[5]) <= int(row[4])
        assert int(row[6]) >= int(row[7]) >= int(row[8])
    assert records[-1][9:] == ["0", "disabled"]
    assert any(row[-1] == "budget" for row in records)
    assert any(row[-1] == "bound" for row in records)
print("resource-order CLI/checker OK: 23 schedules, sidecar records, invalid-capacity rejection")
