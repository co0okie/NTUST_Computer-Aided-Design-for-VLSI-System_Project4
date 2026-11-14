#!/usr/bin/env python3
"""Deterministic stress-input generator for mlrcs.

usage: gen_stress.py NAME [params...] > file.blif      |   gen_stress.py --list
Every generated circuit is a pure function of (NAME, params); random families
take an explicit seed (default 1) and use random.Random(seed).
"""
import random
import sys

W = sys.stdout.write


def emit(inputs, outputs, gates, trailer=".end\n"):
    """gates: iterable of (kind, out, [ins]); kind in AND/OR/NOT."""
    W(".model stress\n.inputs " + " ".join(inputs) + "\n.outputs " + " ".join(outputs) + "\n")
    buf = []
    for kind, out, ins in gates:
        k = len(ins)
        hdr = ".names " + " ".join(ins) + " " + out + "\n"
        if kind == "AND":
            buf.append(hdr + "1" * k + " 1\n")
        elif kind == "NOT":
            buf.append(hdr + "0 1\n")
        else:  # OR: one cube per input
            rows = []
            for i in range(k):
                rows.append("-" * i + "1" + "-" * (k - i - 1) + " 1\n")
            buf.append(hdr + "".join(rows))
        if len(buf) >= 2000:
            W("".join(buf))
            buf = []
    W("".join(buf))
    W(trailer)


def sinks(gates, pis=()):
    used = set()
    for _, _, ins in gates:
        used.update(ins)
    return [o for _, o, _ in gates if o not in used]


def chain_not(n):
    n = int(n)
    gates = [("NOT", f"g{i}", ["a" if i == 0 else f"g{i-1}"]) for i in range(n)]
    emit(["a"], [f"g{n-1}"], gates)


def chain_mix(n):
    n = int(n)
    kinds = ["AND", "OR", "NOT"]
    gates = []
    for i in range(n):
        p = "a" if i == 0 else f"g{i-1}"
        k = kinds[i % 3]
        gates.append((k, f"g{i}", [p] if k == "NOT" else [p, "b"]))
    emit(["a", "b"], [f"g{n-1}"], gates)


def flat(n):
    n = int(n)
    gates = [("AND", f"g{i}", [f"a{i%1000}", f"b{(i*7+1)%1000}"]) for i in range(n)]
    emit([f"a{i}" for i in range(1000)] + [f"b{i}" for i in range(1000)], sinks(gates), gates)


def tree(n):
    n = int(n)  # n internal ops (complete binary tree, n+1 leaves), alternating AND/OR by level
    leaves = n + 1
    gates = []
    level = [f"i{j}" for j in range(leaves)]
    cnt = 0
    lv = 0
    while len(level) > 1:
        nxt = []
        for j in range(0, len(level) - 1, 2):
            o = f"g{cnt}"
            cnt += 1
            gates.append(("AND" if lv % 2 == 0 else "OR", o, [level[j], level[j + 1]]))
            nxt.append(o)
        if len(level) % 2:
            nxt.append(level[-1])
        level = nxt
        lv += 1
    emit([f"i{j}" for j in range(leaves)], [level[0]], gates)


def star(n):
    n = int(n)  # one root gate, n NOT consumers
    gates = [("AND", "root", ["a", "b"])]
    gates += [("NOT", f"c{i}", ["root"]) for i in range(n)]
    emit(["a", "b"], [f"c{i}" for i in range(n)], gates)


def wide(kind, k):
    k = int(k)
    gates = [(kind, "y", [f"x{i}" for i in range(k)])]
    emit([f"x{i}" for i in range(k)], ["y"], gates)


def layered(w, d, seed=1):
    w, d = int(w), int(d)
    r = random.Random(int(seed))
    prev = [f"i{j}" for j in range(w)]
    pis = list(prev)
    gates = []
    for l in range(d):
        cur = []
        for j in range(w):
            o = f"l{l}_{j}"
            k = r.choice(["AND", "OR", "NOT"])
            ins = [r.choice(prev)] if k == "NOT" else [r.choice(prev), r.choice(prev)]
            gates.append((k, o, ins))
            cur.append(o)
        prev = cur
    emit(pis, prev, gates)


def dense(n, k, seed=1):
    n, k = int(n), int(k)
    r = random.Random(int(seed))
    pis = [f"i{j}" for j in range(64)]
    names = list(pis)
    gates = []
    for i in range(n):
        o = f"g{i}"
        kk = min(k, len(names))
        ins = r.sample(names, kk)
        gates.append((r.choice(["AND", "OR"]), o, ins))
        names.append(o)
    emit(pis, sinks(gates), gates)


def onekind(kind, w, d, seed=1):
    w, d = int(w), int(d)
    r = random.Random(int(seed))
    prev = [f"i{j}" for j in range(w)]
    pis = list(prev)
    gates = []
    for l in range(d):
        cur = []
        for j in range(w):
            o = f"l{l}_{j}"
            ins = [r.choice(prev)] if kind == "NOT" else [r.choice(prev), r.choice(prev)]
            gates.append((kind, o, ins))
            cur.append(o)
        prev = cur
    emit(pis, prev, gates)


def tiny():
    gates = [("AND", "x", ["a", "b"]), ("OR", "y", ["x", "c"]), ("NOT", "z", ["y"])]
    emit(["a", "b", "c"], ["z"], gates)


H = ".model m\n.inputs a b c\n.outputs z\n"
ODD = {
    "continuation": ".model m\n.inputs a \\\n b c\n.outputs z\n.names a b \\\n x\n11 1\n.names x c z\n1- 1\n-1 1\n.end\n",
    "comments": "# top\n.model m # name\n.inputs a b c # ins\n.outputs z\n# mid\n.names a b x # and\n11 1 # row\n.names x c z\n1- 1\n-1 1\n.end # done\n",
    "crlf": ".model m\r\n.inputs a b c\r\n.outputs z\r\n.names a b x\r\n11 1\r\n.names x c z\r\n1- 1\r\n-1 1\r\n.end\r\n",
    "tabs": ".model\tm\n.inputs\ta\t\tb   c\n.outputs  z\n.names\ta  \t b\tx\n11\t1\n.names x   c\t z\n1-  1\n-1\t1\n.end\n",
    "nonl": ".model m\n.inputs a b c\n.outputs z\n.names a b x\n11 1\n.names x c z\n1- 1\n-1 1\n.end",
    "noend": ".model m\n.inputs a b c\n.outputs z\n.names a b x\n11 1\n.names x c z\n1- 1\n-1 1\n",
    "nomodel": ".inputs a b c\n.outputs z\n.names a b x\n11 1\n.names x c z\n1- 1\n-1 1\n.end\n",
    "const1": H + ".names k\n1\n.names a k z\n11 1\n.end\n",
    "const0": H + ".names k\n.names a k z\n11 1\n.end\n",
    "const_out": H + ".names z\n1\n.end\n",
    "empty_names": H + ".names\n.names a b z\n11 1\n.end\n",
    "pi_out": ".model m\n.inputs a b\n.outputs a b\n.end\n",
    "nogates": ".model m\n.inputs a b\n.outputs\n.end\n",
    "buffer": H + ".names a b\n1 1\n.names b c z\n11 1\n.end\n",
    "buffer_out": ".model m\n.inputs a\n.outputs z\n.names a z\n1 1\n.end\n",
    "dupfanin_and": ".model m\n.inputs a b\n.outputs y\n.names a a y\n11 1\n.end\n",
    "dupfanin_or": ".model m\n.inputs a b\n.outputs y\n.names a a y\n1- 1\n-1 1\n.end\n",
    "dupfanin_not": ".model m\n.inputs a b\n.outputs y\n.names a a y\n00 1\n.end\n",
    "longname": ".model m\n.inputs " + "A" * 10000 + " b\n.outputs z\n.names " + "A" * 10000 + " b " + "Z" * 10000 + "\n11 1\n.names " + "Z" * 10000 + " b z\n1- 1\n-1 1\n.end\n",
    "oddchars": ".model m\n.inputs a[0] a.1 b$x\n.outputs z<2>\n.names a[0] a.1 x/y\n11 1\n.names x/y b$x z<2>\n1- 1\n-1 1\n.end\n",
    "unicode": ".model m\n.inputs 入力 b\n.outputs 出力\n.names 入力 b 中間\n11 1\n.names 中間 b 出力\n1- 1\n-1 1\n.end\n",
    "reverse": ".model m\n.inputs a b c\n.outputs z\n.names y z\n0 1\n.names x c y\n1- 1\n-1 1\n.names a b x\n11 1\n.end\n",
    "out_fanin": ".model m\n.inputs a b c\n.outputs x z\n.names a b x\n11 1\n.names x c z\n1- 1\n-1 1\n.end\n",
    "dangling": H + ".names a b x\n11 1\n.names x c z\n1- 1\n-1 1\n.names a c d1\n1- 1\n-1 1\n.names d1 d2\n0 1\n.end\n",
    "loop": ".model m\n.inputs a\n.outputs z\n.names a y z\n11 1\n.names z y\n0 1\n.end\n",
    "selfloop": ".model m\n.inputs a\n.outputs z\n.names a z z\n11 1\n.end\n",
    "undefined": ".model m\n.inputs a\n.outputs z\n.names a q z\n11 1\n.end\n",
    "undef_out": ".model m\n.inputs a\n.outputs z w\n.names a z\n0 1\n.end\n",
    "redefine": H + ".names a b x\n11 1\n.names a c x\n11 1\n.names x z\n0 1\n.end\n",
    "pi_redef": H + ".names b c a\n11 1\n.names a z\n0 1\n.end\n",
    "dup_input": ".model m\n.inputs a a b\n.outputs z\n.names a b z\n11 1\n.end\n",
    "dup_output": ".model m\n.inputs a b\n.outputs z z\n.names a b z\n11 1\n.end\n",
    "multi_model": ".model m\n.inputs a b\n.outputs z\n.names a b z\n11 1\n.end\n.model m2\n.inputs q\n.outputs r\n.names q r\n0 1\n.end\n",
    "latch": ".model m\n.inputs a b\n.outputs z\n.latch a z 0\n.end\n",
    "xor": ".model m\n.inputs a b\n.outputs z\n.names a b z\n10 1\n01 1\n.end\n",
    "offset": ".model m\n.inputs a b\n.outputs z\n.names a b z\n11 0\n.end\n",
    "nand_cube": ".model m\n.inputs a b\n.outputs z\n.names a b z\n0- 1\n-0 1\n.end\n",
    "bad_row": ".model m\n.inputs a b\n.outputs z\n.names a b z\n1x 1\n.end\n",
    "empty": "",
    "garbage": None,
    "only_ws": " \n\n\t\n",
    "dangling_backslash": ".model m\n.inputs a b\n.outputs z\n.names a b z\\",
    "huge_line": ".model m\n.inputs a b\n.outputs z\n.names a b z\n11 1\n#" + "x" * 5_000_000 + "\n.end\n",
}


def odd(name):
    if name == "garbage":
        r = random.Random(7)
        sys.stdout.buffer.write(bytes(r.randrange(256) for _ in range(100000)))
        return
    sys.stdout.buffer.write(ODD[name].encode("utf-8"))


FAMILIES = {
    "chain_not": (chain_not, "n"), "chain_mix": (chain_mix, "n"), "flat": (flat, "n"),
    "tree": (tree, "n_ops"), "star": (star, "n_consumers"),
    "wide_and": (lambda k: wide("AND", k), "k"), "wide_or": (lambda k: wide("OR", k), "k"),
    "layered": (layered, "W D [seed]"), "dense": (dense, "n fanin [seed]"),
    "onekind": (onekind, "KIND W D [seed]"), "tiny": (tiny, ""),
}


def main():
    a = sys.argv[1:]
    if not a or a[0] == "--list":
        for k, (_, p) in FAMILIES.items():
            print(f"{k} {p}")
        print("odd_<" + "|".join(ODD) + ">")
        return
    name, params = a[0], a[1:]
    if name.startswith("odd_"):
        odd(name[4:])
    else:
        FAMILIES[name][0](*params)


main()
