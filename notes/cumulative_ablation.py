#!/usr/bin/env python3
"""Prepare scalar/control and cumulative-prefix source snapshots, without editing src/.

python3 notes/cumulative_ablation.py --source b376c9b
TMPDIR="$PWD/shared/tmp/codex" python3 tools/eval.py --src shared/tmp/codex/cumulative-src --cpu 4-7 --threads 4 --mode e aoi_C1355:2,2,1 aoi_C1355:8,8,4 codex_gap:1,1,1 --tsv notes/codex-results.tsv --note cumulative-precedence-ablation
Use scalar-src and --note scalar-precedence-control for the matched control.
No third-party dependencies. Snapshot experiment diagnostics append to the path
in MLRCS_ABLATION_META if set; production source is unchanged.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent.parent
BASE = ROOT / 'shared/tmp/codex'
OLD = '''    // Precedence: start(i) >= start(j) + 1 for every operation edge j -> i
    for (auto const i : n.topo) {
        for (auto const j : n.fanin.row(i)) {
            if (stop_building()) return result;
            if (!n.is_op(j) || late[j] < early[i]) continue; // windows already ordered
            lp.begin("p" + std::to_string(j) + "_" + std::to_string(i));
            for (auto t = early[i]; t <= late[i]; ++t) lp.term(t, x(i, t));
            for (auto t = early[j]; t <= late[j]; ++t) lp.term(-t, x(j, t));
            lp.end(">=", 1);
        }
    }
'''
NEW = '''    // Cumulative precedence: P(start(i)<=t) <= P(start(j)<=t-1).
    // Exactly-one assignments make these equivalent to strict precedence for
    // integer solutions. Thresholds outside [early[i],late[j]] are tautologies:
    // the child prefix is zero or the parent prefix is one. ASAP/ALAP along
    // an edge ensure early[j]<early[i] and late[j]<late[i], so all included
    // prefixes below are partial, without omitted boundary constants.
    for (auto const i : n.topo) {
        for (auto const j : n.fanin.row(i)) {
            if (stop_building()) return result;
            if (!n.is_op(j) || late[j] < early[i]) continue;
            for (auto t = early[i]; t <= late[j]; ++t) {
                if (stop_building()) return result;
                lp.begin("p" + std::to_string(j) + "_" + std::to_string(i) + "_" + std::to_string(t));
                for (auto s = early[i]; s <= t; ++s) lp.term(1, x(i, s));
                for (auto s = early[j]; s < t; ++s) lp.term(-1, x(j, s));
                lp.end("<=", 0);
            }
        }
    }
'''
MARK = '    // Release construction buffers before the solver\'s large allocation phase.\n'
DIAG = '''    // Experiment-only size diagnostics; no model or solver parameter changes.
    if (auto const *meta = std::getenv("MLRCS_ABLATION_META")) {
        std::ofstream info(meta, std::ios::app);
        info << "VARIANT" << '\\t' << n.model << '\\t' << ub << '\\t'
             << result.num_vars << '\\t' << lp.text.size() << '\\t'
             << std::count(lp.text.begin(), lp.text.end(), ':') << '\\n';
    }
'''

def prepare(source_ref='b376c9b', output=BASE):
    """Pin the historical source instead of inheriting future production changes."""
    source = subprocess.check_output(
        ['git', '-C', str(ROOT), 'rev-parse', '--verify', source_ref + '^{commit}'],
        text=True).strip()
    names = subprocess.check_output(
        ['git', '-C', str(ROOT), 'ls-tree', '-r', '--name-only', source,
         '--', 'CMakeLists.txt', 'src'], text=True).splitlines()
    files = {name: subprocess.check_output(
        ['git', '-C', str(ROOT), 'show', source + ':' + name]) for name in names}
    original = files['src/ilp.cpp'].decode()
    if original.count(OLD) != 1 or original.count(MARK) != 1:
        raise SystemExit('Source changed: inspect precedence block before updating this experiment.')
    manifest = {'source_commit': source, 'source_sha256': {
        name: hashlib.sha256(data).hexdigest() for name, data in files.items()},
        'variant_sha256': {}}
    for variant, directory in [('scalar', 'scalar-src'), ('cumulative', 'cumulative-src')]:
        target = output / directory
        if target.exists():
            shutil.rmtree(target)
        target.mkdir(parents=True)
        for name, data in files.items():
            destination = target / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
        text = original if variant == 'scalar' else original.replace(OLD, NEW)
        text = text.replace('#include <cmath>', '#include <cmath>\n#include <cstdlib>')
        text = text.replace(MARK, DIAG.replace('VARIANT', variant) + MARK)
        (target / 'src/ilp.cpp').write_text(text)
        manifest['variant_sha256'][variant] = {
            name: hashlib.sha256((target / name).read_bytes()).hexdigest()
            for name in files}
        print(target)
    (output / 'cumulative-source-manifest.json').write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + '\n')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', default='b376c9b', help='source commit (default: original experiment)')
    parser.add_argument('--output', type=Path, default=BASE, help='snapshot parent directory')
    args = parser.parse_args()
    prepare(args.source, args.output.resolve())
