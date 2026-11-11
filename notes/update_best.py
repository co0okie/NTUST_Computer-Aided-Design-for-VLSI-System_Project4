#!/usr/bin/env python3
"""Merge checked rows of a results TSV (tools/eval.py --tsv) into shared/best.tsv.

usage: update_best.py RESULTS.tsv [--source TEXT]
Only rows with check == OK are used. A row improves best_latency if its latency is lower, and
lower_bound if its lower_bound is higher. The file is locked while it is rewritten.
"""
import csv, fcntl, sys
from pathlib import Path

best_path = Path(__file__).resolve().parent.parent / "shared" / "best.tsv"
results = sys.argv[1]
source = sys.argv[sys.argv.index("--source") + 1] if "--source" in sys.argv else "claude"
rows = [r for r in csv.DictReader(open(results), delimiter="\t") if r["check"] == "OK"]
with open(best_path, "r+") as f:
    fcntl.flock(f, fcntl.LOCK_EX)
    lines = f.read().splitlines()
    table = [l.split("\t") for l in lines[1:]]
    index = {t[0]: t for t in table}
    changed = 0
    for r in rows:
        t = index.get(r["case"])
        if t is None:
            continue
        while len(t) < 5:
            t.append("")
        src = f"{source}@{r['commit']} -{r['mode']}"
        hit = []
        if r["latency"] and int(r["latency"]) < int(t[1]):
            t[1] = r["latency"]; hit.append("latency")
        if r["lower_bound"] and int(r["lower_bound"]) > int(t[2]):
            t[2] = r["lower_bound"]; hit.append("bound")
        if hit:
            t[3] = (t[3] + "; " if len(hit) < 2 and t[3] else "") + src + " (" + "+".join(hit) + ")"
            t[4] = "proven optimal" if t[1] == t[2] else ""
            changed += 1
            print(r["case"], t[1], t[2], hit)
    f.seek(0); f.truncate()
    f.write(lines[0] + "\n" + "".join("\t".join(t) + "\n" for t in table))
print("changed", changed, "proven", sum(t[1] == t[2] for t in table), "of", len(table))
