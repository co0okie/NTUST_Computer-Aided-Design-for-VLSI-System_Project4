#!/usr/bin/env python3
"""Independent checker for mlrcs output.

usage: check.py BLIF AND OR NOT OUTPUT_FILE [--mode h|e] [--quiet]

Re-parses the BLIF file (independently of the C++ parser) and checks that OUTPUT_FILE:
  - has the right title line, "t: {...} {...} {...}" lines for t = 1..LATENCY, "LATENCY: n", "END"
  - schedules every operation exactly once, in the brace of its own kind (AND, OR, NOT)
  - respects precedence (an operation runs strictly after all its operation fanins)
  - uses at most the given number of units of each kind per step
Prints "OK latency=<n>" and exits 0, or prints the first problem and exits 1.
"""

import argparse
import re
import sys

TITLES = {"h": "Heuristic Scheduling Result", "e": "ILP-based Scheduling Result"}
KINDS = ("AND", "OR", "NOT")


def read_blif(path):
    """Returns (inputs, {node: (kind, fanins)}) for an AND/OR/NOT-only BLIF file."""
    text = open(path).read()
    lines = []
    pending = ""
    for raw in text.splitlines():
        raw = raw.split("#", 1)[0].rstrip()
        if raw.endswith("\\"):
            pending += raw[:-1] + " "
            continue
        lines.append((pending + raw).split())
        pending = ""
    inputs, nodes, cur = [], {}, None
    for tok in lines:
        if not tok:
            continue
        if tok[0] == ".inputs":
            inputs += tok[1:]
        elif tok[0] == ".names":
            cur = tok[-1]
            nodes[cur] = [tok[1:-1], []]
        elif tok[0] == ".end":
            break
        elif tok[0].startswith("."):
            cur = None
        elif cur is not None:
            nodes[cur][1].append(tok)
    result = {}
    for name, (fanins, cubes) in nodes.items():
        k = len(fanins)
        if k == 1 and cubes in ([["0", "1"]], [["1", "0"]]):
            kind = "NOT"
        elif k > 1 and cubes == [["1" * k, "1"]]:
            kind = "AND"
        elif k > 1 and len(cubes) == k and all(
            c[1] == "1" and c[0].count("1") == 1 and c[0].count("-") == k - 1 for c in cubes
        ) and len({c[0].index("1") for c in cubes}) == k:
            kind = "OR"
        else:
            raise SystemExit(f"check.py: {path}: node {name} is not AND/OR/NOT")
        result[name] = (kind, fanins)
    return inputs, result


def check(blif, limits, output, mode):
    inputs, nodes = read_blif(blif)
    lines = open(output).read().splitlines()
    if not lines:
        return "empty output"
    if mode and lines[0] != TITLES[mode]:
        return f"title is {lines[0]!r}, expected {TITLES[mode]!r}"
    if lines[0] not in TITLES.values():
        return f"unknown title {lines[0]!r}"
    if len(lines) < 3 or lines[-1] != "END":
        return "output does not end with END"
    m = re.fullmatch(r"LATENCY: (\d+)", lines[-2])
    if not m:
        return f"bad LATENCY line {lines[-2]!r}"
    latency = int(m.group(1))
    body = lines[1:-2]
    if len(body) != latency:
        return f"{len(body)} step lines but LATENCY is {latency}"

    step = {}
    pattern = re.compile(r"(\d+): \{([^{}]*)\} \{([^{}]*)\} \{([^{}]*)\}")
    for t, line in enumerate(body, 1):
        m = pattern.fullmatch(line)
        if not m or int(m.group(1)) != t:
            return f"bad step line {line!r} (expected step {t})"
        for k, kind in enumerate(KINDS):
            group = m.group(k + 2)
            if group != group.strip() or "  " in group:
                return f"step {t}: bad spacing in {{{group}}}"
            names = group.split()
            if len(names) > limits[k]:
                return f"step {t}: {len(names)} {kind} operations, limit {limits[k]}"
            for name in names:
                if name not in nodes:
                    return f"step {t}: {name} is not an operation"
                if nodes[name][0] != kind:
                    return f"step {t}: {name} is {nodes[name][0]}, listed as {kind}"
                if name in step:
                    return f"{name} scheduled twice"
                step[name] = t
    missing = [n for n in nodes if n not in step]
    if missing:
        return f"{len(missing)} operations not scheduled, e.g. {missing[0]}"
    for name, (_, fanins) in nodes.items():
        for f in fanins:
            if f in nodes and step[f] >= step[name]:
                return f"precedence: {f} (step {step[f]}) -> {name} (step {step[name]})"
            if f not in nodes and f not in inputs:
                return f"{f} is undefined"
    if nodes and max(step.values()) != latency:
        return f"last operation at step {max(step.values())}, LATENCY {latency}"
    return latency


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("blif")
    ap.add_argument("limits", nargs=3, type=int, metavar="AND OR NOT")
    ap.add_argument("output")
    ap.add_argument("--mode", choices=("h", "e"))
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()
    result = check(a.blif, a.limits, a.output, a.mode)
    if isinstance(result, str):
        print(f"FAIL {result}")
        sys.exit(1)
    if not a.quiet:
        print(f"OK latency={result}")


if __name__ == "__main__":
    main()
