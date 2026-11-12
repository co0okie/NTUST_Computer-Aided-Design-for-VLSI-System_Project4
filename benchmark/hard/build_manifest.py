#!/usr/bin/env python3
"""Select the final hard suite from search.py results and write manifest.tsv (used once; see README).

  build_manifest.py choose   WORKDIR            -> WORKDIR/suite.txt (chosen cases, `name:A,O,N`)
  build_manifest.py finalize WORKDIR MLRCS COMMIT OUT_MANIFEST
        for every chosen case: `-h --htime 0` and default `-h` (both checked with tools/check.py);
        exact result from WORKDIR/exact.tsv (cases without one, e.g. very large circuits, are run
        with `-e --time 120` here). Writes the manifest.
"""
import collections
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
CHECK = os.path.join(ROOT, "tools", "check.py")
sys.path.insert(0, HERE)
import search  # noqa: E402


def load(work):
    cand = {}
    for l in open(work + "/cand.tsv"):
        r = l.rstrip("\n").split("\t")
        if len(r) > 3:
            cand[r[1] + ":" + r[7]] = dict(spec=r[0], ops=int(r[2]), depth=int(r[3]), na=int(r[4]), no=int(r[5]),
                                           nn=int(r[6]), h0=int(r[8]), lb0=int(r[9]))
    ex = {}
    if os.path.exists(work + "/exact.tsv"):
        for l in open(work + "/exact.tsv"):
            r = l.rstrip("\n").split("\t")
            ex[r[0]] = dict(e=int(r[1]), h=int(r[2]), lb=int(r[3]), status=r[4], T=int(r[7]), ok=r[5] == "OK")
    return cand, ex


def choose(work):
    cand, ex = load(work)
    fam = lambda c: cand[c]["spec"].split()[0]
    circ = lambda c: c.split(":")[0]
    chosen = []
    per = collections.Counter()

    def take(c, cap=2):
        if c in chosen or per[circ(c)] >= cap:
            return
        chosen.append(c)
        per[circ(c)] += 1

    rows = [(c, v) for c, v in ex.items() if v["ok"]]
    # 1. every case where the optimum / best known beats the default heuristic
    for c, v in sorted(rows, key=lambda x: -(x[1]["h"] - x[1]["e"]) / x[1]["e"]):
        if v["e"] < v["h"]:
            take(c, 3)
    # 2. large circuits (>= 1000 ops)
    for c, v in sorted(rows, key=lambda x: -cand[x[0]]["ops"]):
        if cand[c]["ops"] >= 1000 and sum(1 for d in chosen if cand[d]["ops"] >= 1000) < 8:
            take(c)
    for c in sorted(cand, key=lambda c: -(cand[c]["h0"] - cand[c]["lb0"]) / cand[c]["lb0"]):  # very large, only heuristic data
        if cand[c]["ops"] >= 3000 and c not in ex and cand[c]["h0"] > cand[c]["lb0"]:
            if sum(1 for d in chosen if cand[d]["ops"] >= 3000) < 3:
                take(c)
    # 3. fill with proven cases where optimum < h0, by relative gap, family quotas
    quota = {"layered": 18, "chains": 14, "funnel": 14, "xor": 4}
    for f, q in quota.items():
        have = sum(1 for d in chosen if fam(d) == f)
        pool = [(c, v) for c, v in rows if fam(c) == f and v["status"] in ("optimal", "bound") and (v["e"] < cand[c]["h0"] or f == "xor")
                and cand[c]["ops"] < 1000]
        pool.sort(key=lambda x: (-(cand[x[0]]["h0"] - x[1]["e"]) / x[1]["e"], x[0]))
        for c, v in pool:
            if have >= q:
                break
            if c not in chosen and per[circ(c)] < 3:
                take(c, 3)
                have += 1
    open(work + "/suite.txt", "w").write("\n".join(chosen) + "\n")
    cnt = collections.Counter(fam(c) for c in chosen)
    print(len(chosen), dict(cnt))


def run_h(m, path, lim, extra, T=None):
    t0 = time.time()
    p = subprocess.run(["taskset", "-c", "0-1", m, "-h", path] + list(lim) + ["--stats"] + extra,
                       capture_output=True, text=True, timeout=200)
    wall = time.time() - t0
    st = search.parse_stats(p.stderr)
    tmp = path + ".h.out"
    open(tmp, "w").write(p.stdout)
    c = subprocess.run([sys.executable, CHECK, path] + list(lim) + [tmp, "--mode", "h"], capture_output=True, text=True)
    os.remove(tmp)
    if c.returncode != 0:
        raise SystemExit("check failed for %s %s: %s" % (path, lim, c.stdout + c.stderr))
    return int(st["latency"]), int(st["lower_bound"]), wall


def finalize(work, m, commit, out):
    cand, ex = load(work)
    chosen = [l.strip() for l in open(work + "/suite.txt") if l.strip()]
    fam = lambda c: cand[c]["spec"].split()[0]
    chosen.sort(key=lambda c: (fam(c), cand[c]["ops"], c))
    hdr = ["case", "family", "args", "ops", "depth", "N_and", "N_or", "N_not", "h0_latency", "h_latency",
           "best_latency", "lower_bound", "status", "mlrcs_commit"]
    rows = []
    for c in chosen:
        name, lim = c.split(":")
        path = "%s/cand/%s.blif" % (work, name)
        lims = lim.split(",")
        v = cand[c]
        h0, _, _ = run_h(m, path, lims, ["--htime", "0"])
        h, lbh, _ = run_h(m, path, lims, [])
        if c not in ex:
            search.cmd_exact(work, m, "/dev/stdin" if False else write_one(work, c), 120)
            _, ex = load(work)
        e = ex[c]
        best, lb = min(e["e"], h), max(e["lb"], lbh)
        status = "optimal" if best == lb else "open"
        print(c, "h0", h0, "h", h, "best", best, "lb", lb, status, flush=True)
        rows.append([c, fam(c), v["spec"], v["ops"], v["depth"], v["na"], v["no"], v["nn"], h0, h, best, lb, status, commit])
    with open(out, "w") as f:
        f.write("\t".join(hdr) + "\n")
        for r in rows:
            f.write("\t".join(map(str, r)) + "\n")


def write_one(work, c):
    p = work + "/one.txt"
    open(p, "w").write(c + "\n")
    return p


if __name__ == "__main__":
    if sys.argv[1] == "choose":
        choose(sys.argv[2])
    else:
        finalize(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5])
