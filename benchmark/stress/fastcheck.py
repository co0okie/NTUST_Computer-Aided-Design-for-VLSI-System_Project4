#!/usr/bin/env python3
"""Linear-time schedule verifier for very large outputs (tools/check.py is superlinear:
~1 s at 2e4 ops, ~30 s at 1e5). Reuses check.read_blif for parsing.
usage: fastcheck.py BLIF AND OR NOT OUTPUT --mode h|e"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import check  # noqa: E402


def run(blif, limits, out, mode):
    inputs, nodes = check.read_blif(blif)
    inputs = set(inputs)
    with open(out) as f:
        lines = f.read().split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    if lines[0] != check.TITLES[mode]:
        return "bad title"
    if lines[-1] != "END" or not lines[-2].startswith("LATENCY: "):
        return "bad trailer"
    lat = int(lines[-2][9:])
    body = lines[1:-2]
    if len(body) != lat:
        return "step count != LATENCY"
    step = {}
    for t, line in enumerate(body, 1):
        head, _, rest = line.partition(": ")
        if head != str(t):
            return f"bad step line {t}"
        parts = rest.split("} {")
        if len(parts) != 3 or not parts[0].startswith("{") or not parts[2].endswith("}"):
            return f"bad groups at {t}"
        parts[0] = parts[0][1:]
        parts[2] = parts[2][:-1]
        for k, g in enumerate(parts):
            names = g.split()
            if len(names) > limits[k]:
                return f"step {t}: over limit"
            for n in names:
                if n not in nodes or nodes[n][0] != check.KINDS[k] or n in step:
                    return f"step {t}: bad op {n}"
                step[n] = t
    if len(step) != len(nodes):
        return "missing ops"
    for n, (_, fi) in nodes.items():
        for f in fi:
            if f in nodes:
                if step[f] >= step[n]:
                    return f"precedence {f}->{n}"
            elif f not in inputs:
                return f"undefined {f}"
    if nodes and max(step.values()) != lat:
        return "latency != last step"
    return lat


if __name__ == "__main__":
    a = sys.argv[1:]
    r = run(a[0], [int(x) for x in a[1:4]], a[4], a[a.index("--mode") + 1])
    if isinstance(r, str):
        print("FAIL " + r)
        sys.exit(1)
    print(f"OK latency={r}")
