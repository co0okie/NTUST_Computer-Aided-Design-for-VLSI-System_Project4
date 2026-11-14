#!/usr/bin/env python3
"""Validate and summarize the completed, checker-verified stage1 records (no reruns)."""
import csv
import hashlib
import json
import math
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
NOTES = ROOT / 'notes'
TAG = 'hard-order-stage1'

def rows(path):
    with path.open() as handle:
        return list(csv.DictReader(handle, delimiter='\t'))

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def geo(values):
    return math.exp(sum(math.log(a / b) for a, b in values) / len(values))

all_rows = rows(NOTES / 'codex-results.tsv')
formal = [r for r in all_rows if r['note'].startswith(TAG + ' ')]
reference = {r['case']: r for r in rows(NOTES / 'hard-best-reference.tsv')}
claude = {r['case']: r for r in all_rows if r['note'].startswith('hard-baseline-claude ')}
internal = rows(NOTES / (TAG + '-internal.tsv'))
telemetry = {r['model'] + ':' + ','.join(r[k] for k in ('and', 'or', 'not')): r for r in internal}
manifest = json.loads((NOTES / (TAG + '-source-manifest.json')).read_text())
assert len(formal) == len(reference) == len(telemetry) == len(internal) == 50
assert {r['case'] for r in formal} == set(reference) == set(telemetry) == set(claude)
assert all(r['mode'] == 'h' and r['check'] == 'OK' for r in formal)
assert all(r['latency'] == telemetry[r['case']]['final_latency'] for r in formal)
assert all(int(r['final_latency']) <= int(r['seed_latency']) for r in internal)
assert digest(NOTES / 'hard-best-reference.tsv') == manifest['reference_sha256']
assert all(digest(ROOT / name) == sha for name, sha in manifest['files'].items())
# The snapshot was made before 4f1f4c9; source content, rather than inherited git metadata,
# identifies the actual evaluated implementation.
source_sha = hashlib.sha256(json.dumps(manifest['files'], sort_keys=True).encode()).hexdigest()
assert source_sha == manifest['sha256']
paired = []
for r in formal:
    case = r['case']; t = telemetry[case]
    paired.append({'case': case, 'seed': t['seed_latency'], 'final': r['latency'],
                   'claude_baseline': claude[case]['latency'],
                   'frozen_best': reference[case]['best_latency'],
                   'time': r['time'], 'attempts': t['attempts'], 'accepted': t['accepted']})
with (NOTES / (TAG + '-paired.tsv')).open('w') as handle:
    writer = csv.DictWriter(handle, fieldnames=list(paired[0]), delimiter='\t', lineterminator='\n')
    writer.writeheader(); writer.writerows(paired)
summary = {
    'source_commit': '4f1f4c9', 'source_sha256': source_sha,
    'reference_sha256': manifest['reference_sha256'], 'cases': len(formal),
    'checker_ok_at_original_run': 50,
    'original_outputs_retained': False,
    'original_outputs_rechecked': False,
    'data_integrity_checks': 'PASS: case sets, source hashes, frozen reference, final latency telemetry',
    'geometric_mean': geo([(int(r['latency']), int(reference[r['case']]['best_latency'])) for r in formal]),
    'seed_geometric_mean': geo([(int(telemetry[r['case']]['seed_latency']), int(reference[r['case']]['best_latency'])) for r in formal]),
    'equal_frozen_best': sum(int(r['latency']) == int(reference[r['case']]['best_latency']) for r in formal),
    'same_run_seed_improvements': sum(int(r['final_latency']) < int(r['seed_latency']) for r in internal),
    'same_run_seed_regressions': sum(int(r['final_latency']) > int(r['seed_latency']) for r in internal),
    'versus_claude_baseline': dict(Counter('win' if int(r['latency']) < int(claude[r['case']]['latency']) else 'loss' if int(r['latency']) > int(claude[r['case']]['latency']) else 'tie' for r in formal)),
    'total_seconds': round(sum(float(r['time']) for r in formal), 2),
    'max_seconds': max(float(r['time']) for r in formal),
    'build_seconds': 14.5, 'cpu': '4-7', 'reason_counts': dict(Counter(r['reason'] for r in internal)),
    'attempts': sum(int(r['attempts']) for r in internal),
    'accepted': sum(int(r['accepted']) for r in internal),
    'publication_comparison': '1.0044654 is numerically below published 1.00459, but exactly ties same-machine claude baseline on all 50 cases; no causal gain from resource-order search.'
}
(NOTES / (TAG + '-summary.json')).write_text(json.dumps(summary, indent=2) + '\n')
print(json.dumps(summary, indent=2))
