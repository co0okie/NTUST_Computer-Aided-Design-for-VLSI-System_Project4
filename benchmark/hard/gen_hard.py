#!/usr/bin/env python3
"""Deterministic generator of hard ML-RCS test circuits (AND/OR/NOT BLIF).

Usage
  gen_hard.py FAMILY key=value ...  [-o FILE]   write one circuit (stdout by default)
  gen_hard.py --all [--manifest manifest.tsv] [--out DIR]
                                                regenerate every circuit named in the manifest
                                                (column `args` = "FAMILY key=value ...",
                                                 file name = part of `case` before ':')

Families (all parameters are integers except `profile`/`kind`; `seed` is always required)
  layered  seed layers width [profile=uniform|diamond|hourglass] [phased=1] [fanmax=3] [back=3]
           random layered DAG, all kinds mixed in each layer; edge sources mostly in the previous
           `back` layers; with phased=1 every layer has its own random kind mix.
  xor      seed kind=parity|adder|mult|cmp|alu n [trees=k] [m=subset size]
           XOR (= AND(OR(a,b), NOT(AND(a,b)))) based structures: parity trees over random subsets,
           ripple-carry adder, array multiplier, comparator, mux-based ALU slice.
  chains   seed chains length [side=S] [cross=pct] [period=p]
           chains whose kind pattern is phase shifted, cross-linked, plus wide shallow side logic.
  funnel   seed feeders stages tail [overlap=3]
           many ops of one kind feeding overlapping groups of another kind, `stages` times, then a
           deep tail that consumes the funnel outputs at different depths.
Only the python standard library is used; output depends only on the arguments.
"""
import sys
import random

AND, OR, NOT = "AND", "OR", "NOT"


class Rng:
    def __init__(self, seed):
        self.r = random.Random(seed)

    def f(self):
        return self.r.random()

    def i(self, n):  # uniform in [0, n)
        return min(n - 1, int(self.r.random() * n))

    def pick(self, xs):
        return xs[self.i(len(xs))]

    def chance(self, p):
        return self.r.random() < p

    def sample(self, xs, k):
        xs = list(xs)
        out = []
        for _ in range(min(k, len(xs))):
            out.append(xs.pop(self.i(len(xs))))
        return out


class Net:
    def __init__(self, name):
        self.name = name
        self.pis = []
        self.gates = []  # (out, kind, [ins])
        self.used = set()

    def pi(self):
        s = "i%d" % len(self.pis)
        self.pis.append(s)
        return s

    def g(self, kind, ins):
        ins = list(dict.fromkeys(ins))  # dedupe, keep order
        if kind == NOT:
            ins = ins[:1]
        elif len(ins) == 1:  # AND/OR needs fan-in >= 2: degenerate case becomes a NOT
            kind = NOT
        s = "n%d" % len(self.gates)
        self.gates.append((s, kind, ins))
        self.used.update(ins)
        return s

    def xor(self, a, b):
        o = self.g(OR, [a, b])
        n = self.g(NOT, [self.g(AND, [a, b])])
        return self.g(AND, [o, n])

    def mux(self, s, ns, a, b):  # s ? a : b  (ns = NOT s, shared)
        return self.g(OR, [self.g(AND, [s, a]), self.g(AND, [ns, b])])

    def blif(self):
        used_pis = [p for p in self.pis if p in self.used]
        outs = [o for o, _, _ in self.gates if o not in self.used]
        L = [".model %s" % self.name]
        L.append(".inputs " + " ".join(used_pis))
        L.append(".outputs " + " ".join(outs))
        for o, k, ins in self.gates:
            L.append(".names %s %s" % (" ".join(ins), o))
            n = len(ins)
            if k == AND:
                L.append("1" * n + " 1")
            elif k == NOT:
                L.append("0 1")
            else:
                for j in range(n):
                    L.append("-" * j + "1" + "-" * (n - j - 1) + " 1")
        L.append(".end")
        return "\n".join(L) + "\n"


def pick_kind(rng, w):
    x = rng.f() * sum(w)
    for k, wk in zip((AND, OR, NOT), w):
        x -= wk
        if x < 0:
            return k
    return NOT


# ---------------------------------------------------------------- layered
def layered(P, net, rng):
    layers, width = P["layers"], P["width"]
    profile = P.get("profile", "uniform")
    phased = P.get("phased", 1)
    fanmax = P.get("fanmax", 3)
    back = P.get("back", 3)
    widths = []
    for l in range(layers):
        t = l / max(1, layers - 1)
        if profile == "diamond":
            w = width * (1 - abs(2 * t - 1) * 0.8)
        elif profile == "hourglass":
            w = width * (0.2 + 0.8 * abs(2 * t - 1))
        else:
            w = width
        widths.append(max(2, int(round(w))))
    pis = [net.pi() for _ in range(max(widths) + 2)]
    prev = []  # list of layers (lists of signals)
    for l in range(layers):
        w = [0.45, 0.45, 0.2]
        if phased:
            w = [0.1 + rng.f() ** 1.5, 0.1 + rng.f() ** 1.5, 0.05 + 0.5 * rng.f() ** 1.5]
        cur = []
        for _ in range(widths[l]):
            k = pick_kind(rng, w)
            if l == 0:
                pool0 = pis
            else:
                pool0 = prev[-1]
            first = rng.pick(pool0)
            if k == NOT:
                cur.append(net.g(NOT, [first]))
                continue
            fan = 2 if (fanmax == 2 or rng.chance(0.6)) else 3
            pool = []
            for b in range(2, back + 1):
                if l - b >= 0:
                    pool += prev[-b]
            if l < 1 or not pool:
                pool = pis
            ins = [first]
            for _ in range(fan - 1):
                src = pool if (pool and rng.chance(0.5)) else pool0
                ins.append(rng.pick(src))
            if len(set(ins)) < 2:
                ins.append(rng.pick(pis))
            if len(set(ins)) < 2:
                ins.append(pis[0] if pis[0] != first else pis[1])
            cur.append(net.g(k, ins))
        prev.append(cur)


# ---------------------------------------------------------------- xor family
def full_adder(net, a, b, c):
    x = net.xor(a, b)
    s = net.xor(x, c)
    cy = net.g(OR, [net.g(AND, [a, b]), net.g(AND, [x, c])])
    return s, cy


def half_adder(net, a, b):
    return net.xor(a, b), net.g(AND, [a, b])


def xorfam(P, net, rng):
    kind = P.get("kind", "parity")
    n = P["n"]
    if kind == "parity":
        pis = [net.pi() for _ in range(n)]
        trees = P.get("trees", 4)
        m = P.get("m", max(3, n // 2))
        for _ in range(trees):
            sig = rng.sample(pis, m)
            while len(sig) > 1:  # random-shape reduction (pairs from the front, extra mixing)
                if rng.chance(0.5):
                    j = rng.i(len(sig))
                    sig[0], sig[j] = sig[j], sig[0]
                a = sig.pop(0)
                b = sig.pop(0)
                sig.append(net.xor(a, b))
    elif kind == "adder":
        a = [net.pi() for _ in range(n)]
        b = [net.pi() for _ in range(n)]
        s, c = half_adder(net, a[0], b[0])
        for i in range(1, n):
            s, c = full_adder(net, a[i], b[i], c)
        if P.get("trees", 0):  # extra operand: a + b + d (carry chain depth grows)
            d = [net.pi() for _ in range(n)]
            c = None
            for i in range(n):
                if c is None:
                    s, c = half_adder(net, a[i], d[i])
                else:
                    s, c = full_adder(net, a[i], d[i], c)
    elif kind == "mult":
        a = [net.pi() for _ in range(n)]
        b = [net.pi() for _ in range(n)]
        pp = [[net.g(AND, [a[i], b[j]]) for i in range(n)] for j in range(n)]
        acc = pp[0][:]
        for j in range(1, n):
            carry = None
            new = []
            for i in range(n):
                x = acc[i + 1] if i + 1 < len(acc) else None
                y = pp[j][i]
                if x is None:
                    if carry is None:
                        new.append(y)
                    else:
                        s, carry = half_adder(net, y, carry)
                        new.append(s)
                elif carry is None:
                    s, carry = half_adder(net, x, y)
                    new.append(s)
                else:
                    s, carry = full_adder(net, x, y, carry)
                    new.append(s)
            if carry is not None:
                new.append(carry)
            acc = new
    elif kind == "cmp":
        a = [net.pi() for _ in range(n)]
        b = [net.pi() for _ in range(n)]
        lt = None
        eqall = None
        for i in range(n):
            x = net.xor(a[i], b[i])
            eq = net.g(NOT, [x])
            less = net.g(AND, [net.g(NOT, [a[i]]), b[i]])
            if lt is None:
                lt, eqall = less, eq
            else:
                lt = net.g(OR, [less, net.g(AND, [eq, lt])])
                eqall = net.g(AND, [eqall, eq])
    elif kind == "alu":
        a = [net.pi() for _ in range(n)]
        b = [net.pi() for _ in range(n)]
        s0, s1 = net.pi(), net.pi()
        n0, n1 = net.g(NOT, [s0]), net.g(NOT, [s1])
        c = None
        for i in range(n):
            if c is None:
                sm, c = half_adder(net, a[i], b[i])
            else:
                sm, c = full_adder(net, a[i], b[i], c)
            an = net.g(AND, [a[i], b[i]])
            orr = net.g(OR, [a[i], b[i]])
            xr = net.xor(a[i], b[i])
            m1 = net.mux(s0, n0, an, orr)
            m2 = net.mux(s0, n0, xr, sm)
            net.mux(s1, n1, m1, m2)
    else:
        raise ValueError("unknown xor kind " + kind)


# ---------------------------------------------------------------- chains
def chains(P, net, rng):
    nch, length = P["chains"], P["length"]
    side = P.get("side", 0)
    cross = P.get("cross", 30) / 100.0
    period = P.get("period", 4)
    pis = [net.pi() for _ in range(max(6, 2 * nch))]
    # shallow, wide side logic: random gates over PIs and earlier side gates, depth <= 3
    sidesig = []
    lay = list(pis)
    for d in range(3):
        nxt = []
        for _ in range(side // 3 if d < 2 else side - 2 * (side // 3)):
            k = pick_kind(rng, [0.4, 0.4, 0.2])
            if k == NOT:
                nxt.append(net.g(NOT, [rng.pick(lay)]))
            else:
                nxt.append(net.g(k, rng.sample(lay, 2 + (1 if rng.chance(0.3) else 0))))
        sidesig += nxt
        lay = list(dict.fromkeys(nxt + lay[: len(lay) // 2] + pis[:3]))
    patterns = []
    for c in range(nch):
        patterns.append([pick_kind(rng, [0.4, 0.4, 0.2]) for _ in range(period)])
    phase = [rng.i(period) for _ in range(nch)]
    last = [None] * nch
    for j in range(length):
        cur = []
        for c in range(nch):
            k = patterns[c][(j + phase[c]) % period]
            head = last[c] if last[c] is not None else rng.pick(pis)
            if k == NOT:
                cur.append(net.g(NOT, [head]))
                continue
            others = [x for x in last if x is not None and x != head] if j > 0 else []
            if others and rng.chance(cross):
                second = rng.pick(others)
            elif sidesig and rng.chance(0.4):
                second = rng.pick(sidesig)
            else:
                second = rng.pick(pis)
            if second == head:
                second = pis[0] if pis[0] != head else pis[1]
            ins = [head, second]
            if rng.chance(0.15):
                ins.append(rng.pick(pis))
            cur.append(net.g(k, ins))
        last = cur
    # make sure every chain end stays an output (automatic: unused gates are outputs)


# ---------------------------------------------------------------- funnel
def funnel(P, net, rng):
    m, stages, tail = P["feeders"], P["stages"], P["tail"]
    ov = P.get("overlap", 3)
    pis = [net.pi() for _ in range(max(6, m))]
    X = rng.pick([AND, OR])
    Y = OR if X == AND else AND
    level = []
    for _ in range(m):
        level.append(net.g(X, rng.sample(pis, 2)))
    outs = []
    cur = level
    kinds = [X, Y]
    for s in range(stages):
        k = kinds[(s + 1) % 2]
        cnt = max(2, len(cur) // 2)
        nxt = []
        for i in range(cnt):
            center = int(i * len(cur) / cnt)
            grp = [cur[(center + d) % len(cur)] for d in range(ov)]
            fan = 2 + rng.i(ov - 1) if ov > 2 else 2
            nxt.append(net.g(k, rng.sample(grp, fan)))
        outs.append(nxt)
        cur = nxt
    # deep tail: alternating kinds, taking funnel signals from random stages
    t = rng.pick(cur)
    for j in range(tail):
        k = rng.pick([AND, OR, NOT]) if rng.chance(0.3) else (AND if j % 2 else OR)
        if k == NOT:
            t = net.g(NOT, [t])
        else:
            src = rng.pick(outs[rng.i(len(outs))])
            if src == t:
                src = rng.pick(pis)
            t = net.g(k, [t, src])


FAMILIES = {"layered": layered, "xor": xorfam, "chains": chains, "funnel": funnel}


def parse_spec(tokens):
    fam = tokens[0]
    P = {}
    for t in tokens[1:]:
        k, v = t.split("=", 1)
        P[k] = int(v) if v.lstrip("-").isdigit() else v
    if "seed" not in P:
        raise ValueError("seed=... required")
    return fam, P


def generate(tokens, name="hard"):
    fam, P = parse_spec(tokens)
    if fam not in FAMILIES:
        raise ValueError("unknown family " + fam)
    net = Net(name)
    FAMILIES[fam](P, net, Rng("%s/%d" % (fam, P["seed"])))
    return net.blif()


def main(argv):
    if "--all" in argv:
        import os
        here = os.path.dirname(os.path.abspath(__file__))
        manifest = argv[argv.index("--manifest") + 1] if "--manifest" in argv else os.path.join(here, "manifest.tsv")
        out = argv[argv.index("--out") + 1] if "--out" in argv else here
        os.makedirs(out, exist_ok=True)
        rows = [l.rstrip("\n").split("\t") for l in open(manifest)]
        hdr = rows[0]
        ci, ai = hdr.index("case"), hdr.index("args")
        done = set()
        for r in rows[1:]:
            name = r[ci].split(":")[0]
            if name in done:
                continue
            done.add(name)
            with open(os.path.join(out, name + ".blif"), "w") as f:
                f.write(generate(r[ai].split(), name))
        print("regenerated %d files into %s" % (len(done), out))
        return 0
    o = None
    if "-o" in argv:
        i = argv.index("-o")
        o = argv[i + 1]
        argv = argv[:i] + argv[i + 2:]
    text = generate(argv, o.rsplit("/", 1)[-1].rsplit(".", 1)[0] if o else "hard")
    if o:
        open(o, "w").write(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
