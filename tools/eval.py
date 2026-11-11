#!/usr/bin/env python3
"""Evaluate mlrcs the way a grader would: clean build, then run each case with a time limit.

usage: eval.py [--src DIR] [--mode h,e] [--tsv FILE] [--note TEXT] [--cpu LIST] [--threads N]
               [--timeout S] [--list] [CASE ...]

A CASE is BENCH:AND,OR,NOT, e.g. aoi_C1355:2,2,1 (BENCH is a file name in benchmark/ without
.blif, or a path to a .blif file). Without cases, the default suite is used (see --list).

Steps:
  1. Copy CMakeLists.txt and src/ from --src into a fresh directory under shared/tmp/, run
     `cmake -B build && cmake --build build` and time it (the build must finish in 60 s).
  2. For each case and mode, run `build/mlrcs -h|-e ... --stats` with a wall-clock timeout
     (default 60 s), pinned with taskset to --cpu (default: by branch, see below).
  3. Check every output with tools/check.py and write one TSV row per run (appended to --tsv
     right after the run, so an interrupted evaluation keeps its rows).

CPU pinning (the laptop has 6 P-cores = CPUs 0-11, and E-cores 12-19): branch `claude` uses
CPUs 0-3, `codex` 4-7, anything else 8-11. Gurobi gets --threads (default 4) threads.
"""

import argparse
import datetime
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BENCH = ROOT / "benchmark"
CHECK = Path(__file__).resolve().parent / "check.py"

# Default suite. big4 is byte-identical to big3, so it is left out.
CIRCUITS = [
    "aoi_sample01", "aoi_sample02", "aoi_cm138a", "aoi_x2", "aoi_z4ml", "aoi_i3", "aoi_i4",
    "aoi_i2", "aoi_9symml", "aoi_cht", "aoi_C1355", "aoi_alu4", "aoi_t481", "aoi_i8",
    "aoi_C6288", "aoi_des", "aoi_big1", "aoi_big2", "aoi_big3",
]
CONSTRAINTS = [(1, 1, 1), (2, 2, 1), (3, 2, 2), (8, 8, 4)]
DEFAULT_SUITE = [f"{c}:{a},{o},{n}" for c in CIRCUITS for a, o, n in CONSTRAINTS]

FIELDS = ["date", "commit", "case", "mode", "latency", "lower_bound", "heuristic", "status",
          "time", "check", "note"]


def git(src, *args):
    r = subprocess.run(["git", "-C", str(src), *args], capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else ""


def default_cpus(src):
    branch = git(src, "rev-parse", "--abbrev-ref", "HEAD")
    return {"claude": "0-3", "codex": "4-7"}.get(branch, "8-11")


def parse_case(text):
    m = re.fullmatch(r"(.+):(\d+),(\d+),(\d+)", text)
    if not m:
        sys.exit(f"eval.py: bad case {text!r}, expected BENCH:AND,OR,NOT")
    bench = m.group(1)
    path = Path(bench) if bench.endswith(".blif") else BENCH / f"{bench}.blif"
    if not path.exists():
        sys.exit(f"eval.py: no such benchmark {path}")
    return path, [int(m.group(i)) for i in (2, 3, 4)]


def build(src, work):
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    shutil.copy(src / "CMakeLists.txt", work)
    shutil.copytree(src / "src", work / "src")
    start = time.monotonic()
    for cmd in (["cmake", "-B", "build"], ["cmake", "--build", "build"]):
        r = subprocess.run(cmd, cwd=work, capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f"eval.py: build failed: {' '.join(cmd)}\n{r.stdout[-3000:]}{r.stderr[-3000:]}")
    seconds = time.monotonic() - start
    binary = work / "build" / "mlrcs"
    if not binary.exists():
        sys.exit("eval.py: build/mlrcs was not produced")
    return binary, seconds


def run_case(binary, blif, limits, mode, args, work):
    out_file = work / "stdout.txt"
    err_file = work / "stderr.txt"
    cmd = [str(binary), f"-{mode}", str(blif), *map(str, limits), "--stats"]
    if mode == "e" and args.threads:
        cmd += ["--threads", str(args.threads)]
    if args.cpu != "none":
        cmd = ["taskset", "-c", args.cpu, *cmd]
    start = time.monotonic()
    with open(out_file, "w") as out, open(err_file, "w") as err:
        try:
            r = subprocess.run(cmd, stdout=out, stderr=err, timeout=args.timeout)
            code = r.returncode
        except subprocess.TimeoutExpired:
            code = None
    seconds = time.monotonic() - start

    row = {"latency": "", "lower_bound": "", "heuristic": "", "status": "", "time": f"{seconds:.2f}"}
    stderr = err_file.read_text(errors="replace")
    stats = re.search(r"^mlrcs-stats (.*)$", stderr, re.M)
    if stats:
        kv = dict(p.split("=", 1) for p in stats.group(1).split())
        for key in ("lower_bound", "heuristic", "status"):
            row[key] = kv.get(key, "")
    if code is None:
        row["check"] = "TIMEOUT"
        return row
    if code != 0:
        row["check"] = "ERROR " + " ".join(stderr.strip().splitlines()[-1:])[:200]
        return row
    c = subprocess.run([sys.executable, str(CHECK), str(blif), *map(str, limits), str(out_file),
                        "--mode", mode], capture_output=True, text=True)
    line = c.stdout.strip() or c.stderr.strip()
    if c.returncode == 0:
        row["latency"] = line.split("=", 1)[1]
        row["check"] = "OK"
    else:
        row["check"] = line[:200]
    return row


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("cases", nargs="*")
    ap.add_argument("--src", default=str(ROOT), help="tree with CMakeLists.txt and src/")
    ap.add_argument("--mode", default="h,e", help="comma-separated: h, e or both (default h,e)")
    ap.add_argument("--tsv", help="append results to this TSV file")
    ap.add_argument("--note", default="", help="free text for the note column")
    ap.add_argument("--cpu", help="taskset CPU list, or 'none' (default: by branch)")
    ap.add_argument("--threads", type=int, default=4, help="Gurobi threads (0: mlrcs default)")
    ap.add_argument("--timeout", type=float, default=60, help="seconds per run (default 60)")
    ap.add_argument("--list", action="store_true", help="print the default suite and exit")
    args = ap.parse_args()

    if args.list:
        print("\n".join(DEFAULT_SUITE))
        return
    # Non-interactive shells may not have sourced ~/.bashrc: add the local Gurobi install
    if shutil.which("gurobi_cl") is None and Path("/opt/gurobi/bin/gurobi_cl").exists():
        os.environ["PATH"] = "/opt/gurobi/bin:" + os.environ.get("PATH", "")
        os.environ["LD_LIBRARY_PATH"] = "/opt/gurobi/lib:" + os.environ.get("LD_LIBRARY_PATH", "")
    src = Path(args.src).resolve()
    args.cpu = args.cpu or default_cpus(src)
    modes = [m for m in args.mode.split(",") if m]
    if any(m not in ("h", "e") for m in modes):
        sys.exit("eval.py: --mode takes h, e or h,e")
    cases = [parse_case(c) for c in (args.cases or DEFAULT_SUITE)]

    commit = git(src, "rev-parse", "--short", "HEAD") or "-"
    if git(src, "status", "--porcelain", "--", "src", "CMakeLists.txt"):
        commit += "+dirty"
    work = ROOT / "shared" / "tmp" / f"eval-{os.getpid()}"
    binary, build_seconds = build(src, work)
    print(f"# commit {commit}, build {build_seconds:.1f} s, cpu {args.cpu}", file=sys.stderr)
    if build_seconds > 60:
        print("# WARNING: build took more than 60 s", file=sys.stderr)

    tsv = None
    if args.tsv:
        new = not Path(args.tsv).exists() or Path(args.tsv).stat().st_size == 0
        tsv = open(args.tsv, "a")
        if new:
            tsv.write("\t".join(FIELDS) + "\n")
    print("\t".join(FIELDS[2:-1]))
    failures = 0
    try:
        for blif, limits in cases:
            for mode in modes:
                row = run_case(binary, blif, limits, mode, args, work)
                row.update(date=datetime.datetime.now().strftime("%Y-%m-%d %H:%M"),
                           commit=commit, case=f"{blif.stem}:{','.join(map(str, limits))}",
                           mode=mode, note=args.note)
                failures += row["check"] != "OK"
                print("\t".join(str(row[f]) for f in FIELDS[2:-1]), flush=True)
                if tsv:
                    tsv.write("\t".join(str(row[f]) for f in FIELDS) + "\n")
                    tsv.flush()
    finally:
        if tsv:
            tsv.close()
        shutil.rmtree(work, ignore_errors=True)
    if failures:
        print(f"# {failures} run(s) failed", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
