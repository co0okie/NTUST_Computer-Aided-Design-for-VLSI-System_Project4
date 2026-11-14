#!/usr/bin/env python3
"""Search pipeline used to build the hard suite (see README.md). Not needed to regenerate files.

  search.py cand  WORKDIR MLRCS [ROUND]        generate candidate circuits + limit sets, run `-h --htime 0`
                                        -> WORKDIR/cand.tsv (resumable, one row per circuit x limits)
  search.py exact WORKDIR MLRCS LIST T  run `-e --time T` on the cases listed in LIST (one
                                        `name:A,O,N` per line), check with tools/check.py
                                        -> WORKDIR/exact.tsv (appends; skips cases already present)
Candidate parameters are drawn from a fixed RNG, so the pipeline is reproducible.
"""
import os
import re
import subprocess
import sys
import time
import random

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_hard  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
CHECK = os.path.join(ROOT, "tools", "check.py")


def candidate_specs(rnd=1):
    if rnd == 2:
        return candidate_specs2()
    if rnd == 3:
        return candidate_specs3()
    r = random.Random(20260611)
    specs = []
    sid = 100

    def add(fam, **kw):
        nonlocal sid
        sid += 1
        kw = dict(kw)
        kw["seed"] = sid
        specs.append(" ".join([fam] + ["%s=%s" % kv for kv in [("seed", sid)] + [(k, v) for k, v in kw.items() if k != "seed"]]))

    ri = lambda a, b: r.randint(a, b)
    for _ in range(160):
        L, W = ri(6, 40), ri(3, 16)
        while L * W > 420:
            W -= 1
        add("layered", layers=L, width=W, profile=r.choice(["uniform", "diamond", "hourglass"]),
            phased=r.choice([1, 1, 1, 0]), fanmax=r.choice([2, 3, 3]), back=r.choice([1, 2, 3, 4]))
    for _ in range(40):
        add("xor", kind="parity", n=ri(8, 24), trees=ri(2, 8), m=ri(4, 12))
    for n in (5, 6, 8, 10, 12, 16, 20, 24, 28):
        add("xor", kind="adder", n=n)
    for n in (4, 6, 8, 12, 16, 24, 30):
        add("xor", kind="adder", n=n, trees=1)
    for n in (3, 4, 5, 6):
        add("xor", kind="mult", n=n)
    for n in (8, 12, 16, 24, 32, 40):
        add("xor", kind="cmp", n=n)
    for n in (3, 4, 5, 6, 8, 10):
        add("xor", kind="alu", n=n)
    for _ in range(110):
        add("chains", chains=ri(2, 10), length=ri(10, 40), side=ri(0, 60), cross=ri(10, 50), period=ri(2, 6))
    for _ in range(110):
        add("funnel", feeders=ri(8, 60), stages=ri(1, 4), tail=ri(10, 60), overlap=r.choice([3, 4]))
    # large ones
    for _ in range(10):
        add("layered", layers=ri(40, 90), width=ri(20, 60), profile=r.choice(["uniform", "diamond", "hourglass"]),
            phased=1, fanmax=3, back=ri(1, 4))
    for n in (8, 10, 12, 14, 16):
        add("xor", kind="mult", n=n)
    for n in (150, 300, 450):
        add("xor", kind="adder", n=n)
    add("xor", kind="cmp", n=300)
    for n in (24, 40, 60):
        add("xor", kind="alu", n=n)
    for _ in range(6):
        add("chains", chains=ri(10, 30), length=ri(60, 120), side=ri(100, 600), cross=ri(10, 50), period=ri(2, 6))
    for _ in range(6):
        add("funnel", feeders=ri(200, 700), stages=ri(2, 4), tail=ri(60, 200), overlap=r.choice([3, 4]))
    return specs


def candidate_specs2():
    """round 2: mid-size circuits (300-1500 ops), seeds >= 1000."""
    r = random.Random(20260612)
    specs = []
    sid = 1000
    ri = lambda a, b: r.randint(a, b)
    for _ in range(90):
        sid += 1
        L, W = ri(15, 70), ri(8, 30)
        while L * W > 1300:
            W -= 1
        specs.append("layered seed=%d layers=%d width=%d profile=%s phased=%d fanmax=%d back=%d" % (
            sid, L, W, r.choice(["uniform", "diamond", "hourglass"]), r.choice([1, 1, 0]), r.choice([2, 3]), ri(1, 4)))
    for _ in range(60):
        sid += 1
        specs.append("chains seed=%d chains=%d length=%d side=%d cross=%d period=%d" % (
            sid, ri(4, 14), ri(30, 90), ri(20, 250), ri(10, 50), ri(2, 7)))
    for _ in range(60):
        sid += 1
        specs.append("funnel seed=%d feeders=%d stages=%d tail=%d overlap=%d" % (
            sid, ri(60, 250), ri(2, 4), ri(40, 120), r.choice([3, 4])))
    return specs


def candidate_specs3():
    """round 3: a few very large circuits (3000-6000 ops), seeds >= 2000."""
    return [
        "layered seed=2001 layers=60 width=80 profile=uniform phased=1 fanmax=3 back=3",
        "layered seed=2002 layers=70 width=90 profile=diamond phased=1 fanmax=3 back=2",
        "layered seed=2003 layers=100 width=60 profile=hourglass phased=1 fanmax=3 back=3",
        "chains seed=2004 chains=40 length=100 side=800 cross=30 period=4",
        "chains seed=2005 chains=25 length=160 side=600 cross=40 period=5",
        "funnel seed=2006 feeders=1000 stages=3 tail=250 overlap=3",
        "xor seed=2007 kind=mult n=22",
    ]


def stats_of(blif):
    """ops, depth, per-kind counts straight from the generated gates (independent of mlrcs)."""
    names = {}
    cnt = {"AND": 0, "OR": 0, "NOT": 0}
    depth = {}
    cur = None
    gates = []
    lines = blif.split("\n")
    i = 0
    while i < len(lines):
        l = lines[i]
        if l.startswith(".names"):
            t = l.split()[1:]
            ins, out = t[:-1], t[-1]
            nxt = lines[i + 1]
            cube = nxt.split()[0]
            if len(ins) == 1:
                k = "NOT"
            elif "-" in cube or len(ins) == 1:
                k = "OR"
            else:
                k = "AND"
            gates.append((out, k, ins))
        i += 1
    for out, k, ins in gates:
        cnt[k] += 1
        depth[out] = 1 + max([depth.get(x, 0) for x in ins])
    return len(gates), max(depth.values()), cnt


def limit_sets(spec, ops, D, cnt, n=3):
    r = random.Random("lim/" + spec)
    big = ops > 1000
    cap = 64 if big else 8
    out = []
    kinds = ["AND", "OR", "NOT"]
    for _ in range(40):
        if len(out) >= n:
            break
        tight = r.sample(kinds, 2)
        c = []
        for k in kinds:
            lo, hi = (0.6, 1.2) if k in tight else (0.3, 1.6)
            ratio = r.uniform(lo, hi)
            c.append(max(1, min(cap, round(cnt[k] / (ratio * D)))) if cnt[k] else 1)
        if tuple(c) not in out and any(cnt[k] / c[i] >= 0.6 * D for i, k in enumerate(kinds) if cnt[k]):
            out.append(tuple(c))
    return out


def run_h0(m, path, lim):
    p = subprocess.run(["taskset", "-c", "0-1", m, "-h", path] + [str(x) for x in lim] + ["--stats", "--htime", "0"],
                       capture_output=True, text=True, timeout=120)
    s = parse_stats(p.stderr)
    return int(s["latency"]), int(s["lower_bound"]), int(s["first"])


def cmd_cand(work, m, rnd=1):
    os.makedirs(work + "/cand", exist_ok=True)
    out = work + "/cand.tsv"
    done = set()
    if os.path.exists(out):
        done = {l.split("\t")[0] for l in open(out)}
    f = open(out, "a")
    for spec in candidate_specs(rnd):
        if spec in done:
            continue
        blif = gen_hard.generate(spec.split(), "x")
        ops, D, cnt = stats_of(blif)
        if ops < 25:
            f.write("%s\tskip\n" % spec)
            f.flush()
            continue
        name = "%s_s%s_n%d" % (spec.split()[0], spec.split()[1].split("=")[1], ops)
        path = "%s/cand/%s.blif" % (work, name)
        open(path, "w").write(blif.replace(".model x", ".model " + name, 1))
        for lim in limit_sets(spec, ops, D, cnt):
            h0, lb, first = run_h0(m, path, lim)
            f.write("%s\t%s\t%d\t%d\t%d\t%d\t%d\t%s\t%d\t%d\t%d\n" % (
                spec, name, ops, D, cnt["AND"], cnt["OR"], cnt["NOT"], ",".join(map(str, lim)), h0, lb, first))
            f.flush()
        if spec not in done:
            pass
    f.close()


def parse_stats(err):
    line = [l for l in err.splitlines() if l.startswith("mlrcs-stats")][-1]
    return dict(t.split("=", 1) for t in line.split()[1:] if "=" in t)


def cmd_exact(work, m, lst, T):
    out = work + "/exact.tsv"
    done = set()
    if os.path.exists(out):
        done = {l.split("\t")[0] for l in open(out)}
    f = open(out, "a")
    env = dict(os.environ)
    env["PATH"] = "/opt/gurobi/bin:" + env["PATH"]
    env["LD_LIBRARY_PATH"] = "/opt/gurobi/lib"
    budget = float(os.environ.get("BUDGET", "1e9"))
    start = time.time()
    for case in [l.strip() for l in open(lst) if l.strip()]:
        if case in done:
            continue
        if time.time() - start + float(T) + 15 > budget:
            break
        name, lim = case.split(":")
        a, o, n = lim.split(",")
        path = "%s/cand/%s.blif" % (work, name)
        outf = "%s/cand/%s.%s.out" % (work, name, lim.replace(",", "_"))
        t0 = time.time()
        with open(outf, "w") as of:
            p = subprocess.run(["taskset", "-c", "0-1", m, "-e", path, a, o, n, "--stats", "--threads", "2",
                                "--time", str(T)], stdout=of, stderr=subprocess.PIPE, text=True, env=env,
                               timeout=T + 120)
        wall = time.time() - t0
        c = subprocess.run([sys.executable, CHECK, path, a, o, n, outf, "--mode", "e"], capture_output=True, text=True)
        st = parse_stats(p.stderr)
        f.write("%s\t%s\t%s\t%s\t%s\t%s\t%.1f\t%s\n" % (
            case, st["latency"], st["heuristic"], st["lower_bound"], st["status"], "OK" if c.returncode == 0 else "FAIL",
            wall, T))
        f.flush()
        os.remove(outf)
    f.close()


if __name__ == "__main__":
    if sys.argv[1] == "cand":
        cmd_cand(sys.argv[2], sys.argv[3], int(sys.argv[4]) if len(sys.argv) > 4 else 1)
    elif sys.argv[1] == "exact":
        cmd_exact(sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5]))
