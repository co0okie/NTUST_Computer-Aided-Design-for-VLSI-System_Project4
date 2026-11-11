#!/usr/bin/env python3
"""Exercise exact scheduling failures without requiring Gurobi or third-party packages.

Run from any directory:
    python3 /path/to/repo/notes/check_exact_fallback.py --binary /path/to/mlrcs
Optionally test a licensed solver with --gurobi /opt/gurobi/bin/gurobi_cl.
All generated files and captured outputs live under shared/tmp/codex/fallback-test/.
"""

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SCRATCH = ROOT / "shared" / "tmp" / "codex" / "fallback-test"
BENCH = ROOT / "benchmark" / "codex_gap.blif"
CHECKER = ROOT / "tools" / "check.py"

FAKE_SOLVER = r'''
import os
from pathlib import Path
import signal
import sys
import time

if "--version" in sys.argv:
    raise SystemExit(0)
mode = os.environ["MLRCS_TEST_FAULT"]
if mode == "exit":
    raise SystemExit(3)
if mode == "signal":
    os.kill(os.getpid(), signal.SIGTERM)
if mode == "missing":
    raise SystemExit(0)
if mode == "hang":
    time.sleep(100)
args = dict(arg.split("=", 1) for arg in sys.argv[1:] if "=" in arg)
text = Path(args["InputFile"]).read_text()
if mode == "incomplete":
    text = "L 3\n"
if mode == "fractional":
    text = text.replace(" 1\n", " 0.75\n")
if mode == "duplicate":
    text += "x2_2 1\n"
if mode == "invalid":
    text = "L 3\nx2_1 1\nx3_1 1\nx4_2 1\nx5_2 1\n"
if mode == "infeasible":
    print("Model is infeasible")
    raise SystemExit(0)
if mode in ("improved", "optimal"):
    # n1 at 1 unlocks n3 at 2 while n0 uses the second NOT slot.
    text = "L 3\nx2_2 1\nx3_1 1\nx4_3 1\nx5_2 1\n"
Path(args["ResultFile"]).write_text(text)
print("Optimal solution found" if mode == "optimal" else "Time limit reached")
'''


def run_checked(binary, work, mode, env, latency=4, status="timelimit", seconds="55"):
    out = work / (mode + ".out")
    command = [str(binary), "-e", str(BENCH), "1", "1", "1", "--stats",
               "--time", seconds, "--threads", "1", "--keep", str(work / mode)]
    with out.open("w") as stream:
        result = subprocess.run(command, env=env, cwd=work, stdout=stream,
                                stderr=subprocess.PIPE, text=True, timeout=10)
    (work / (mode + ".err")).write_text(result.stderr)
    assert result.returncode == 0, (mode, result.stderr)
    checked = subprocess.run([sys.executable, str(CHECKER), str(BENCH), "1", "1", "1",
                              str(out), "--mode", "e"], cwd=work,
                             capture_output=True, text=True, timeout=10)
    assert checked.returncode == 0, (mode, checked.stdout, checked.stderr)
    assert f"LATENCY: {latency}\n" in out.read_text(), (mode, out.read_text())
    stats_line = next(line for line in result.stderr.splitlines()
                      if line.startswith("mlrcs-stats "))
    stats = dict(field.split("=", 1) for field in stats_line.split()[1:])
    assert stats["status"] == status, (mode, stats)
    assert int(stats["latency"]) <= int(stats["heuristic"]), (mode, stats)
    assert int(stats["lower_bound"]) <= latency, (mode, stats)
    if status == "optimal":
        assert int(stats["lower_bound"]) == latency, (mode, stats)
    print(f"{mode}: PASS (latency={latency}, status={status})", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build" / "mlrcs")
    parser.add_argument("--gurobi", type=Path, help="optional real gurobi_cl executable")
    args = parser.parse_args()
    binary = args.binary.resolve()
    assert binary.is_file() and os.access(binary, os.X_OK), f"not executable: {binary}"
    # Use the codex CPU when available, without depending on taskset or the caller's PATH.
    if hasattr(os, "sched_getaffinity") and 4 in os.sched_getaffinity(0):
        os.sched_setaffinity(0, {4})
    SCRATCH.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="run-", dir=SCRATCH))
    print(f"Artifacts: {work}", flush=True)
    fake_dir = work / "fake-bin"
    fake_dir.mkdir()
    fake = fake_dir / "gurobi_cl"
    fake.write_text(f"#!{sys.executable}\n" + FAKE_SOLVER)
    fake.chmod(0o755)
    env = os.environ.copy()
    env.pop("GUROBI_HOME", None)
    env["PATH"] = str(fake_dir)
    for mode in ("exit", "signal", "missing", "incomplete", "fractional", "duplicate",
                 "invalid", "infeasible", "valid", "hang", "short"):
        env["MLRCS_TEST_FAULT"] = mode
        seconds = "5" if mode == "hang" else "0.01" if mode == "short" else "55"
        run_checked(binary, work, mode, env, seconds=seconds)
    for mode, status in (("improved", "timelimit"), ("optimal", "optimal")):
        env["MLRCS_TEST_FAULT"] = mode
        run_checked(binary, work, mode, env, latency=3, status=status)

    # An absent solver must remain an error even though execution failures after
    # the initial availability probe safely retain the heuristic schedule.
    missing_env = env.copy()
    missing_env["PATH"] = str(work / "empty-bin")
    result = subprocess.run([str(binary), "-e", str(BENCH), "1", "1", "1"],
                            cwd=work, env=missing_env, capture_output=True,
                            text=True, timeout=10)
    assert result.returncode != 0 and "gurobi_cl not found" in result.stderr, result
    assert not result.stdout, result.stdout
    print("missing executable: PASS (nonzero exit)", flush=True)

    if args.gurobi is not None:
        solver = args.gurobi.resolve()
        assert solver.is_file() and os.access(solver, os.X_OK), f"not executable: {solver}"
        real_dir = work / "real-bin"
        real_dir.mkdir()
        (real_dir / "gurobi_cl").symlink_to(solver)
        real_env = env.copy()
        real_env["PATH"] = str(real_dir)
        library_dir = solver.parent.parent / "lib"
        if library_dir.is_dir():
            real_env["LD_LIBRARY_PATH"] = str(library_dir) + os.pathsep + real_env.get(
                "LD_LIBRARY_PATH", "")
        run_checked(binary, work, "genuine", real_env, latency=3, status="optimal")


if __name__ == "__main__":
    main()
