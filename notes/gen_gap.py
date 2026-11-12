#!/usr/bin/env python3
"""Search for a small random AND/OR/NOT DAG where `mlrcs -h` is worse than `mlrcs -e`.

Generate one instance:  python3 notes/gen_gap.py gen SEED NOPS NIN > out.blif
Search:                 python3 notes/gen_gap.py search [--count N] [--min-ops A] [--max-ops B] [--seed0 S]
REPRODUCE: (filled in after search; see bottom of notes/claude.md or the final line printed by search)
Only plain python3 stdlib; all randomness is seeded.
"""
import random, subprocess, sys, os, re, argparse

def gen(seed, nops, nin):
    r = random.Random(seed)
    sigs = [f"i{k}" for k in range(nin)]
    lines, used = [], set()
    for k in range(nops):
        kind = r.choice(["AND", "OR", "NOT"])
        win = max(3, int(len(sigs) * r.choice([0.3, 0.5, 1.0])))
        def pick():
            return r.choice(sigs[-win:]) if r.random() < 0.7 else r.choice(sigs)
        name = f"n{k}"
        if kind == "NOT":
            a = pick()
            lines.append(f".names {a} {name}\n0 1"); ins = [a]
        else:
            fan = r.choice([2, 2, 3])
            ins = []
            for _ in range(fan * 5):
                s = pick()
                if s not in ins: ins.append(s)
                if len(ins) == fan: break
            if len(ins) < 2:
                ins = r.sample(sigs, 2)
            if kind == "AND":
                rows = "1" * len(ins) + " 1"
            else:
                rows = "\n".join("".join("1" if j == i else "-" for j in range(len(ins))) + " 1" for i in range(len(ins)))
            lines.append(f".names {' '.join(ins)} {name}\n{rows}")
        used.update(ins)
        sigs.append(name)
    outs = [s for s in sigs[nin:] if s not in used]
    return (".model gap\n.inputs " + " ".join(sigs[:nin]) + "\n.outputs " + " ".join(outs) + "\n"
            + "\n".join(lines) + "\n.end\n")

def stats(binp, mode, blif, res, env, tl=20):
    p = subprocess.run(["taskset", "-c", "0-3", binp, mode, blif, *map(str, res), "--stats", "--threads", "4", "--time", str(tl)],
                       capture_output=True, text=True, env=env)
    m = re.search(r"mlrcs-stats (.*)", p.stderr)
    return dict(kv.split("=", 1) for kv in m.group(1).split()) if m else {}

def search(a):
    env = dict(os.environ)
    env["PATH"] = "/opt/gurobi/bin:" + env["PATH"]; env["LD_LIBRARY_PATH"] = "/opt/gurobi/lib"
    tmp = "shared/tmp/claude/gap_try.blif"
    resl = [(1,1,1),(2,1,1),(1,2,1),(2,2,1),(2,2,2),(3,2,2)]
    nz = 0; found = 0
    r = random.Random(a.seed0)
    for it in range(a.count):
        seed = r.randrange(10**9)
        nops = r.randint(a.min_ops, a.max_ops); nin = r.randint(3, 8)
        res = r.choice(resl)
        open(tmp, "w").write(gen(seed, nops, nin))
        h = stats(a.bin, "-h", tmp, res, env)
        if not h or h.get("latency") == h.get("lower_bound"): continue
        nz += 1
        e = stats(a.bin, "-e", tmp, res, env)
        if e and int(e["latency"]) < int(h["latency"]) and e.get("status") == "optimal":
            found += 1
            print(f"FOUND seed={seed} nops={nops} nin={nin} res={res} h={h['latency']} e={e['latency']}", flush=True)
            if a.stop: break
    print(f"searched {it+1}, latency!=lb {nz}, found {found}")

if __name__ == "__main__":
    if sys.argv[1] == "gen":
        sys.stdout.write(gen(int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])))
    else:
        ap = argparse.ArgumentParser(); ap.add_argument("cmd")
        ap.add_argument("--count", type=int, default=3000); ap.add_argument("--bin", default="build/mlrcs")
        ap.add_argument("--min-ops", type=int, default=15); ap.add_argument("--max-ops", type=int, default=40)
        ap.add_argument("--seed0", type=int, default=1); ap.add_argument("--stop", type=int, default=1)
        search(ap.parse_args())
