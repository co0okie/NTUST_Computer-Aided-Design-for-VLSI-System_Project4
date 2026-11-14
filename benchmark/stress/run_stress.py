#!/usr/bin/env python3
"""Stress runner for mlrcs (see gen_stress.py for the inputs).

usage: run_stress.py --bin MLRCS --work DIR --out RESULTS.tsv [--filter SUBSTR]
                     [--maxops N] [--big] [--cpus 2-3] [--vmem-kb 4000000]
One TSV row per (case, mode). Inputs are generated into DIR and deleted when the next input
starts. Every run: taskset + ulimit -v + timeout 70 + /usr/bin/time -v. Output is verified with
tools/check.py. Needs: Gurobi on PATH (for -e rows).
"""
import argparse
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
GEN = os.path.join(HERE, "gen_stress.py")
CHECK = os.path.join(HERE, "..", "..", "tools", "check.py")


def case(name, gen, lim="2,2,2", modes="h", expect="ok", args=None, hint=0):
    return dict(name=name, gen=gen.split(), lim=lim.split(","), modes=modes, expect=expect, args=args, hint=hint)


def build_cases(big):
    C = []
    sizes = [10000, 100000] + ([1000000] if big else [])
    # 1. shapes
    for fam in ["chain_not", "chain_mix", "flat", "tree", "star"]:
        for n in sizes:
            for lim in ["1,1,1", "50,50,50"]:
                modes = "h"
                C.append(case(f"{fam}_{n}", f"{fam} {n}", lim, modes, hint=n))
    # -e subset
    C.append(case("chain_not_10000", "chain_not 10000", "1,1,1", "e", hint=10000))
    C.append(case("chain_mix_10000", "chain_mix 10000", "1,1,1", "e", hint=10000))
    C.append(case("flat_10000", "flat 10000", "50,50,50", "e", hint=10000))
    C.append(case("tree_10000", "tree 10000", "1,1,1", "e", hint=10000))
    C.append(case("star_100000", "star 100000", "1,1,1", "e", hint=100000))
    for k in [5000]:
        for f in ["wide_and", "wide_or"]:
            for lim in ["1,1,1", "2,2,2"]:
                C.append(case(f"{f}_{k}", f"{f} {k}", lim, "he", hint=1))
    C.append(case("wide_and_20000", "wide_and 20000", "1,1,1", "h", hint=1))
    C.append(case("wide_or_20000", "wide_or 20000", "1,1,1", "h", hint=1))
    for lim in ["1,1,1", "100,100,100", "1000,1000,1000", "1000000,1000000,1000000"]:
        modes = "he" if lim in ("1,1,1", "100,100,100") else "h"
        C.append(case("layered_5000x20", "layered 5000 20", lim, modes, hint=100000))
    for lim in ["1,1,1", "5,5,5", "1000000,1000000,1000000"]:
        C.append(case("layered_5x20000", "layered 5 20000", lim, "he" if lim != "1000000,1000000,1000000" else "h", hint=100000))
    C.append(case("layered_100x100", "layered 100 100", "3,3,3", "he", hint=10000))
    if big:
        C.append(case("layered_50000x20", "layered 50000 20", "100,100,100", "h", hint=1000000))
        C.append(case("layered_5x200000", "layered 5 200000", "2,2,2", "he", hint=1000000))
        C.append(case("layered_1000x1000", "layered 1000 1000", "1,1,1", "he", hint=1000000))
        C.append(case("layered_1000x1000", "layered 1000 1000", "3,3,3", "he", hint=1000000))
        C.append(case("dense_100000x20", "dense 100000 20", "1,1,1", "h", hint=1000000))
        C.append(case("dense_100000x20", "dense 100000 20", "30,30,30", "h", hint=1000000))
        C.append(case("chain_not_1000000", "chain_not 1000000", "1,1,1", "e", hint=1000000))
    for n, k in [(3000, 50), (20000, 50), (3000, 200)]:
        for lim in ["1,1,1", "20,20,20"]:
            C.append(case(f"dense_{n}x{k}", f"dense {n} {k}", lim, "he" if (n, lim) == (3000, "1,1,1") and k == 50 else "h", hint=n))
    for kind in ["AND", "OR", "NOT"]:
        C.append(case(f"onekind_{kind}_100x100", f"onekind {kind} 100 100", "1,1,1", "he" if kind == "AND" else "h", hint=10000))
        C.append(case(f"onekind_{kind}_1000x100", f"onekind {kind} 1000 100", "10,10,10", "h", hint=100000))
    # absent kind with limits given anyway (AND-only circuit)
    C.append(case("absent_OR_NOT", "onekind AND 100 100", "2,1000000,2147483647", "he", hint=10000))
    C.append(case("absent_zero", "onekind AND 100 100", "2,0,0", "h", hint=10000))
    C.append(case("absent_zero_ok_and_zero", "onekind AND 100 100", "0,2,2", "h", expect="err", hint=10000))
    # 2. limits
    for lim in ["1,1,1", "1000000,1000000,1000000", "2147483647,2147483647,2147483647", "1,1000,1000", "1000,1,1", "1000,1,1000", "1,1,1000"]:
        C.append(case("limits_layered_100x100", "layered 100 100", lim, "he" if lim in ("1000000,1000000,1000000", "1,1000,1000") else "h", hint=10000))
    for lim in ["2147483647,2147483647,2147483647", "1,1,1"]:
        C.append(case("limits_tiny", "tiny", lim, "he"))
    for lim in ["0,2,2", "2,0,2", "2,2,0", "0,0,0", "-1,2,2", "2,2,-5", "2147483648,2,2", "99999999999999999999,2,2", "1.5,2,2", "abc,2,2", "2,,2", "+2,2,2", "02,2,2", "0x2,2,2"]:
        C.append(case("badlim_tiny", "tiny", lim, "h", expect="err"))
    base = ["{F}", "2", "2", "2"]
    for tag, args in [
        ("no_args", []), ("missing_limit", ["{F}", "2", "2"]), ("missing_all_limits", ["{F}"]),
        ("extra_arg", ["{F}", "2", "2", "2", "9"]), ("nofile", ["/nonexistent/x.blif", "2", "2", "2"]),
        ("dir_as_file", ["/", "2", "2", "2"]), ("opt_time_nan", base + ["--time", "abc"]),
        ("opt_time_neg", base + ["--time", "-1"]), ("opt_time_0", base + ["--time", "0"]),
        ("opt_time_inf", base + ["--time", "inf"]), ("opt_time_nanval", base + ["--time", "nan"]),
        ("opt_time_missing", base + ["--time"]), ("opt_threads_neg", base + ["--threads", "-3"]),
        ("opt_threads_abc", base + ["--threads", "abc"]), ("opt_htime_neg", base + ["--htime", "-5"]),
        ("opt_unknown", base + ["--bogus"]), ("opt_keep_missing_dir", base + ["--keep", "/nonexistent/dir/zz"]),
        ("empty_string_file", ["", "2", "2", "2"]),
    ]:
        C.append(case(f"args_{tag}", "tiny", "2,2,2", "he" if tag in ("opt_time_0", "opt_threads_neg", "opt_keep_missing_dir") else "h", expect="err", args=args))
    C.append(case("args_time0_valid", "layered 100 100", "3,3,3", "e", args=base + ["--time", "0"], hint=10000))
    C.append(case("args_time1_valid", "layered 100 100", "3,3,3", "e", args=base + ["--time", "1"], hint=10000))
    for tag in ["bad_mode"]:
        C.append(case("args_bad_mode", "tiny", "2,2,2", "x", expect="err", args=["-x"] + base))
    # 3. oddities
    odd_ok = ["continuation", "comments", "crlf", "tabs", "nonl", "noend", "nomodel", "pi_out", "nogates", "dupfanin_and",
              "dupfanin_or", "longname", "oddchars", "unicode", "reverse", "out_fanin", "dangling", "dup_input", "dup_output",
              "multi_model", "huge_line"]
    odd_maybe = ["const1", "const0", "const_out", "empty_names", "buffer", "buffer_out", "dupfanin_not", "xor", "offset",
                 "nand_cube", "bad_row", "latch", "pi_redef"]
    odd_err = ["loop", "selfloop", "undefined", "undef_out", "redefine", "empty", "garbage", "only_ws", "dangling_backslash"]
    e_set = {"crlf", "dupfanin_and", "dangling", "reverse", "pi_out", "nogates", "out_fanin", "longname", "buffer", "const1"}
    for o in odd_ok:
        C.append(case(f"odd_{o}", f"odd_{o}", "2,2,2", "he" if o in e_set else "h"))
    for o in odd_maybe:
        C.append(case(f"odd_{o}", f"odd_{o}", "2,2,2", "he" if o in e_set else "h", expect="maybe"))
    for o in odd_err:
        C.append(case(f"odd_{o}", f"odd_{o}", "2,2,2", "h", expect="err"))
    return C


def parse_time_file(path):
    t = open(path).read()
    rss = re.search(r"Maximum resident set size \(kbytes\): (\d+)", t)
    exit_ = re.search(r"Exit status: (\d+)", t)
    sig = re.search(r"Command terminated by signal (\d+)", t)
    return (int(rss.group(1)) / 1024 if rss else -1, int(exit_.group(1)) if exit_ else None, int(sig.group(1)) if sig else None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--filter", default="")
    ap.add_argument("--minops", type=int, default=0)
    ap.add_argument("--maxops", type=int, default=0)
    ap.add_argument("--big", action="store_true")
    ap.add_argument("--cpus", default="2-3")
    ap.add_argument("--vmem-kb", type=int, default=4000000)
    ap.add_argument("--modes", default="he")
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)
    env = dict(os.environ)
    env["PATH"] = "/opt/gurobi/bin:" + env["PATH"]
    env["LD_LIBRARY_PATH"] = "/opt/gurobi/lib:" + env.get("LD_LIBRARY_PATH", "")
    blif = os.path.join(a.work, "in.blif")
    outp = os.path.join(a.work, "out.txt")
    errp = os.path.join(a.work, "err.txt")
    timep = os.path.join(a.work, "time.txt")
    cols = "name ops limits mode exit wall_s rss_mb latency lower_bound check class note".split()
    new = not os.path.exists(a.out)
    tsv = open(a.out, "a")
    if new:
        tsv.write("\t".join(cols) + "\n")
    last_gen = None
    for c in build_cases(a.big):
        if a.filter and a.filter not in c["name"] and a.filter not in " ".join(c["gen"]):
            continue
        if a.minops and c["hint"] < a.minops:
            continue
        if a.maxops and c["hint"] > a.maxops:
            continue
        key = " ".join(c["gen"])
        if key != last_gen:
            with open(blif, "wb") as f:
                subprocess.run([sys.executable, GEN] + c["gen"], stdout=f, check=True)
            last_gen = key
        ops = open(blif, "rb").read().count(b".names") if os.path.getsize(blif) < 400_000_000 else -1
        for mode in c["modes"]:
            if mode not in a.modes + "x":
                continue
            args = c["args"] if c["args"] is not None else ["{F}"] + c["lim"]
            args = [blif if x == "{F}" else x for x in args]
            if mode == "x":
                argv = [a.bin] + args
            else:
                argv = [a.bin, "-" + mode] + (args if c["args"] is None or c["args"][:1] != ["-x"] else args)
            if c["args"] is not None and c["args"][:1] == ["-x"]:
                argv = [a.bin] + args
            if "--stats" not in argv:
                argv.append("--stats")
            shell = f"ulimit -v {a.vmem_kb}; exec taskset -c {a.cpus} /usr/bin/time -v -o {timep} timeout 70 \"$@\""
            for p in (outp, errp, timep):
                if os.path.exists(p):
                    os.remove(p)
            t0 = time.time()
            with open(outp, "wb") as fo, open(errp, "wb") as fe:
                subprocess.run(["bash", "-c", shell, "bash"] + argv, stdout=fo, stderr=fe, env=env)
            wall = time.time() - t0
            rss, rc, sig = parse_time_file(timep)
            err = open(errp, "rb").read().decode("utf-8", "replace")
            stats = re.search(r"mlrcs-stats (.*)", err)
            kv = dict(x.split("=", 1) for x in stats.group(1).split()) if stats else {}
            first_err = next((l for l in err.splitlines() if l.strip() and not l.startswith("mlrcs-stats")), "")[:140]
            chk, chk_ok = "-", True
            lat = kv.get("latency", "-")
            if rc == 0 and mode != "x" and (c["args"] is None or "{F}" in c["args"]):
                lim = c["lim"]
                try:
                    chk_script = CHECK if ops < 50000 else os.path.join(HERE, "fastcheck.py")
                    r = subprocess.run(["bash", "-c", "ulimit -v 8000000; exec python3 \"$@\"", "bash", chk_script, blif] + lim + [outp, "--mode", mode],
                                       capture_output=True, text=True, timeout=900)
                    chk = (r.stdout.strip() or r.stderr.strip()).splitlines()[0][:100] if (r.stdout.strip() or r.stderr.strip()) else f"rc{r.returncode}"
                    if r.returncode == 0:
                        m = re.search(r"latency=(\d+)", chk)
                        lat = m.group(1) if m else lat
                    chk_ok = r.returncode == 0
                except subprocess.TimeoutExpired:
                    chk, chk_ok = "checker timeout", False
            # classification
            if rc == 124 or wall > 60:
                cls = "TIMEOUT"
            elif sig is not None:
                cls = "CRASH" if sig != 9 else "MEMORY?"
            elif "bad_alloc" in err or "Cannot allocate" in err:
                cls = "MEMORY"
            elif "runtime error" in err or "Sanitizer" in err or "terminate called" in err or "Assertion" in err:
                cls = "CRASH"
            elif rc is not None and rc >= 128:
                cls = "CRASH"
            elif rc == 0:
                if mode == "x":
                    cls = "ACCEPTED"
                elif chk == "-":
                    cls = "ACCEPTED"
                elif not chk_ok:
                    cls = "WRONG" if c["expect"] != "err" or True else "WRONG"
                else:
                    cls = "OK" if c["expect"] != "err" else "ACCEPTED"
            else:
                cls = "CLEAN_ERR" if (first_err and c["expect"] in ("err", "maybe")) else "ERR_UNEXPECTED"
            if rss > 2048 and cls == "OK":
                cls = "MEMORY"
            note = first_err
            if mode == "h" and wall > 30:
                note = "SLOW-h " + note
            row = [c["name"], str(ops), ",".join(c["lim"]) if c["args"] is None else " ".join(c["args"][1:]).replace(blif, "F"),
                   mode, str(rc), f"{wall:.2f}", f"{rss:.0f}", lat, kv.get("lower_bound", "-"), chk, cls, note.replace("\t", " ")]
            tsv.write("\t".join(row) + "\n")
            tsv.flush()
            print("\t".join(row[:8] + row[10:11]), flush=True)
    tsv.close()
    for p in (blif, outp, errp, timep):
        if os.path.exists(p):
            os.remove(p)


main()
