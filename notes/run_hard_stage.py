#!/usr/bin/env python3
"""Immutable hard-suite evaluation; retains outputs, same-run telemetry, and provenance.

Run: taskset -c 4-7 python3 notes/run_hard_stage.py --tag hard-order-stage2
Recheck/analyze retained records: python3 notes/run_hard_stage.py --tag hard-order-stage2 --summarize
Results remain in notes/TAG-results.tsv; deliberately not appended to codex-results.tsv.
"""
import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
NOTES = ROOT / 'notes'
TMP = (ROOT / 'shared/tmp/codex').resolve()
HEADER = ['model', 'and', 'or', 'not', 'seed_latency', 'final_latency',
          'attempts', 'feasible', 'accepted', 'enabled', 'reason']


def rows(path):
    with path.open() as stream:
        return list(csv.DictReader(stream, delimiter='\t'))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def git(source, *args):
    result = subprocess.run(['git', '-C', str(source), *args], capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else None


def paths(tag):
    return TMP / tag, NOTES / (tag + '-results.tsv')


def summarize(tag):
    run, results = paths(tag)
    manifest = json.loads((NOTES / (tag + '-source-manifest.json')).read_text())
    source = run / 'source'
    assert all(sha(source / name) == value for name, value in manifest['files'].items())
    assert sha(run / 'frozen-best.tsv') == manifest['frozen_reference_sha256']
    assert all(sha(ROOT / 'benchmark/hard' / (c.rsplit(':', 1)[0] + '.blif')) == value
               for c, value in manifest['case_files'].items())
    formal = rows(results)
    expected = set(manifest['cases'])
    actual = [r['case'] for r in formal]
    assert len(set(actual)) == len(actual), 'Duplicate formal case rows'
    assert set(actual) <= expected
    frozen = {r['case']: int(r['best_latency']) for r in rows(run / 'frozen-best.tsv')}
    live = {r['case']: int(r['best_latency']) for r in rows(ROOT / 'shared/best.tsv')}
    # Preserve precisely which denominator was current at analysis time.
    shutil.copy2(ROOT / 'shared/best.tsv', run / 'live-best-at-summary.tsv')
    checked = {}
    for meta_path in sorted((run / 'outputs').glob('*/run.json')):
        meta = json.loads(meta_path.read_text())
        folder = meta_path.parent
        assert sha(folder / 'stdout.txt') == meta['stdout_sha256']
        assert sha(folder / 'stderr.txt') == meta['stderr_sha256']
        case = Path(meta['blif']).stem + ':' + ','.join(map(str, meta['limits']))
        if meta['result']['check'] == 'OK':
            check = subprocess.run(['taskset', '-c', '4-7', sys.executable, str(ROOT / 'tools/check.py'),
                                    meta['blif'], *map(str, meta['limits']), str(folder / 'stdout.txt'),
                                    '--mode', 'h'], capture_output=True, text=True)
            assert check.returncode == 0, (case, check.stderr, check.stdout)
            assert check.stdout.strip() == 'OK latency=' + meta['result']['latency'], check.stdout
            checked[case] = meta['result']['latency']
    valid = [r for r in formal if r['check'] == 'OK']
    assert all(checked.get(r['case']) == r['latency'] for r in valid)
    raw = run / 'internal-raw.tsv'
    telemetry = []
    if raw.exists():
        (NOTES / (tag + '-internal.tsv')).write_text('\t'.join(HEADER) + '\n' + raw.read_text())
        telemetry = rows(NOTES / (tag + '-internal.tsv'))
    telemetry_by_case = {r['model'] + ':' + ','.join(r[k] for k in ('and', 'or', 'not')): r for r in telemetry}
    assert len(telemetry_by_case) == len(telemetry), 'Duplicate telemetry cases'
    assert set(telemetry_by_case) <= expected, 'Unexpected telemetry case/model name'
    for r in valid:
        assert r['case'] in telemetry_by_case, 'Missing same-run telemetry'
        assert telemetry_by_case[r['case']]['final_latency'] == r['latency']
    def geo(denominator, seed=False):
        if not valid:
            return None
        return math.exp(sum(math.log(int(telemetry_by_case[r['case']]['seed_latency'] if seed else r['latency']) /
                                         denominator[r['case']]) for r in valid) / len(valid))
    log = (NOTES / (tag + '.log')).read_text()
    build = re.search(r'# commit .*, build ([0-9.]+) s, cpu 4-7', log)
    summary = {
        'source_sha256': manifest['sha256'], 'cases_expected': len(expected), 'rows': len(formal),
        'complete': set(actual) == expected, 'checker_ok': len(valid), 'retained_outputs_rechecked': len(checked),
        'frozen_geomean': geo(frozen), 'same_run_seed_frozen_geomean': geo(frozen, seed=True),
        'live_geomean': geo(live), 'live_reference_sha256': sha(run / 'live-best-at-summary.tsv'),
        'equal_frozen_best': sum(int(r['latency']) == frozen[r['case']] for r in valid),
        'same_run_improvements': sum(int(r['final_latency']) < int(r['seed_latency']) for r in telemetry),
        'same_run_regressions': sum(int(r['final_latency']) > int(r['seed_latency']) for r in telemetry),
        'total_seconds': round(sum(float(r['time']) for r in formal), 2),
        'max_seconds': max((float(r['time']) for r in formal), default=0),
        'build_seconds': float(build[1]) if build else None,
        'failures': [r for r in formal if r['check'] != 'OK'],
        'strict_live_best_improvements': [r for r in valid if int(r['latency']) < live[r['case']]],
        'outputs': str(run / 'outputs'),
        'recheck_checker_sha256': sha(ROOT / 'tools/check.py'),
    }
    write_json(NOTES / (tag + '-summary.json'), summary)
    print(json.dumps(summary, indent=2))
    return summary


def evaluate(args):
    run, results = paths(args.tag)
    assert not run.exists() and not results.exists(), 'Use a fresh tag; never overwrite an evaluation'
    selected = [r['case'] for r in rows(ROOT / 'benchmark/hard/manifest.tsv')]
    if args.cases:
        assert set(args.cases) <= set(selected), 'Unknown hard-suite case'
        selected = args.cases
    assert len(selected) == len(set(selected))
    run.mkdir(parents=True)
    source = Path(args.src).resolve()
    snapshot = run / 'source'
    snapshot.mkdir()
    shutil.copy2(source / 'CMakeLists.txt', snapshot / 'CMakeLists.txt')
    shutil.copytree(source / 'src', snapshot / 'src')
    files = {str(p.relative_to(snapshot)): sha(p) for p in sorted(snapshot.rglob('*')) if p.is_file()}
    aggregate = hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()
    shutil.copy2(NOTES / 'hard-best-reference.tsv', run / 'frozen-best.tsv')
    shutil.copy2(ROOT / 'shared/best.tsv', run / 'live-best-at-start.tsv')
    manifest = {'source': str(source), 'source_git_revision': git(source, 'rev-parse', 'HEAD'),
                'source_git_status': git(source, 'status', '--short'),
                'snapshot': str(snapshot), 'files': files, 'sha256': aggregate, 'cases': selected,
                'frozen_reference_sha256': sha(run / 'frozen-best.tsv'),
                'live_reference_at_start_sha256': sha(run / 'live-best-at-start.tsv'),
                'eval_sha256': sha(ROOT / 'tools/eval.py'), 'check_sha256': sha(ROOT / 'tools/check.py'),
                'case_files': {c: sha(ROOT / 'benchmark/hard' / (c.rsplit(':', 1)[0] + '.blif')) for c in selected}}
    write_json(NOTES / (args.tag + '-source-manifest.json'), manifest)
    for path in sorted(snapshot.rglob('*'), reverse=True):
        path.chmod(0o555 if path.is_dir() else 0o444)
    snapshot.chmod(0o555)
    case_args = [str(ROOT / 'benchmark/hard' / (c.rsplit(':', 1)[0] + '.blif')) + ':' + c.rsplit(':', 1)[1] for c in selected]
    command = ['taskset', '-c', '4-7', sys.executable, str(ROOT / 'tools/eval.py'), '--src', str(snapshot),
               '--cpu', '4-7', '--mode', 'h', '--timeout', '60', '--tsv', str(results), '--keep-outputs',
               str(run / 'outputs'), '--note', args.tag + ' source_sha256=' + aggregate, *case_args]
    extra = dict(item.split('=', 1) for item in args.env)
    assert 'MLRCS_ORDER_LOG' not in extra, 'The harness owns the telemetry destination'
    environment = {**os.environ, **extra, 'TMPDIR': str(TMP), 'MLRCS_ORDER_LOG': str(run / 'internal-raw.tsv')}
    write_json(NOTES / (args.tag + '-command.json'), {'command': command,
               'environment': {k: v for k, v in environment.items() if k.startswith('MLRCS_') or k == 'TMPDIR'}})
    with (NOTES / (args.tag + '.log')).open('w') as log:
        result = subprocess.run(command, cwd=ROOT, env=environment, stdout=log, stderr=subprocess.STDOUT)
    summary = summarize(args.tag)
    return result.returncode or (0 if summary['complete'] and not summary['failures'] else 1)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--src', default=str(ROOT))
    parser.add_argument('--summarize', action='store_true')
    parser.add_argument('--env', action='append', default=[], metavar='KEY=VALUE')
    parser.add_argument('cases', nargs='*')
    args = parser.parse_args()
    assert re.fullmatch(r'[A-Za-z0-9_.-]+', args.tag) and args.tag not in ('.', '..'), 'Invalid tag'
    if args.summarize:
        summarize(args.tag)
    else:
        raise SystemExit(evaluate(args))
