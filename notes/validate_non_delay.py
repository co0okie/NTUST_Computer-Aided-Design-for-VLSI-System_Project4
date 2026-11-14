#!/usr/bin/env python3
"""Theory validation only: exact subset DP with/without partial idle; no mlrcs evaluation.
Run on CPU4-7: taskset -c 4-7 python3 notes/validate_non_delay.py
All 200 generated schedules and BLIFs are retained on disk; the fixed-priority toy is in notes.
"""
import csv
from functools import lru_cache
import hashlib
from itertools import combinations, product
import json
from pathlib import Path
import random
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / 'shared/tmp/codex/non-delay-validation'


def solve(kind, pred, limits, non_delay):
    n = len(kind)
    full = (1 << n) - 1
    choice = {}
    transitions = 0
    @lru_cache(None)
    def dp(done):
        nonlocal transitions
        if done == full:
            return 0
        ready = [[u for u in range(n) if kind[u] == k and not (done >> u & 1)
                  and pred[u] & done == pred[u]] for k in range(3)]
        options = []
        for k in range(3):
            maximum = min(limits[k], len(ready[k]))
            sizes = [maximum] if non_delay else range(maximum + 1)
            options.append([sum(1 << u for u in subset) for size in sizes
                            for subset in combinations(ready[k], size)])
        optimum = n + 1
        for selected in product(*options):
            action = selected[0] | selected[1] | selected[2]
            if not action: # whole-system idle cannot help makespan; omit self-loop
                continue
            transitions += 1
            value = 1 + dp(done | action)
            if value < optimum:
                optimum, choice[done] = value, action
        assert optimum <= n, 'Generated graph is not acyclic'
        return optimum
    optimum = dp(0)
    steps = [0] * n
    done = 0
    for t in range(1, optimum + 1):
        action = choice[done]
        for u in range(n):
            if action >> u & 1:
                steps[u] = t
        done |= action
    assert done == full
    return optimum, steps, dp.cache_info().currsize, transitions


def write_blif(path, kind, pred):
    lines = ['.model ' + path.stem, '.inputs a b', '.outputs ' + ' '.join('n' + str(u) for u in range(len(kind)))]
    for u, k in enumerate(kind):
        fanin = ['n' + str(v) for v in range(u) if pred[u] >> v & 1]
        if k == 2:
            assert len(fanin) <= 1
            if not fanin:
                fanin = ['a']
        else:
            if len(fanin) < 2:
                fanin += ['a', 'b'][:2 - len(fanin)]
        lines.append('.names ' + ' '.join(fanin) + ' n' + str(u))
        if k == 0:
            lines.append('1' * len(fanin) + ' 1')
        elif k == 1:
            for i in range(len(fanin)):
                lines.append('-' * i + '1' + '-' * (len(fanin) - i - 1) + ' 1')
        else:
            lines.append('0 1')
    path.write_text('\n'.join(lines + ['.end', '']))


def write_schedule(path, kind, steps):
    latency = max(steps)
    lines = ['Heuristic Scheduling Result']
    for t in range(1, latency + 1):
        sets = ['{' + ' '.join('n' + str(u) for u in range(len(kind)) if kind[u] == k and steps[u] == t) + '}' for k in range(3)]
        lines.append(str(t) + ': ' + ' '.join(sets))
    path.write_text('\n'.join(lines + ['LATENCY: ' + str(latency), 'END', '']))


def check(blif, output, limits):
    run = subprocess.run([sys.executable, str(ROOT / 'tools/check.py'), str(blif),
                          *map(str, limits), str(output), '--mode', 'h'], capture_output=True, text=True)
    output.with_suffix(output.suffix + '.check').write_text(run.stdout + run.stderr)
    assert run.returncode == 0, (blif, output, run.stdout, run.stderr)


def greedy(kind, pred, limits):
    done = 0
    steps = [0] * len(kind)
    t = 0
    while not all(steps):
        t += 1
        selected = []
        for k in range(3):
            selected.extend([u for u in range(len(kind)) if kind[u] == k and not steps[u]
                             and pred[u] & done == pred[u]][:limits[k]])
        assert selected
        for u in selected:
            steps[u] = t
            done |= 1 << u
    return steps


def main():
    started = time.monotonic()
    assert not DEST.exists(), 'Use a new artifact directory for a rerun (do not overwrite evidence)'
    DEST.mkdir(parents=True)
    random_source = random.Random(661704)
    data = []
    for trial in range(100):
        n = random_source.randint(3, 10)
        kind = [random_source.randrange(3) for _ in range(n)]
        pred = []
        probability = [.15, .25, .40][trial % 3]
        for u in range(n):
            incoming = [v for v in range(u) if random_source.random() < probability]
            if kind[u] == 2 and incoming:
                incoming = [random_source.choice(incoming)]
            pred.append(sum(1 << v for v in incoming))
        limits = [random_source.randint(1, 2) for _ in range(3)]
        blif = DEST / ('random_%03d.blif' % trial)
        write_blif(blif, kind, pred)
        answers = []
        row = {'case': blif.stem, 'ops': n, 'resources': ','.join(map(str, limits))}
        for non_delay, label in [(False, 'idle'), (True, 'non_delay')]:
            start = time.monotonic()
            optimum, steps, states, transitions = solve(kind, pred, limits, non_delay)
            seconds = time.monotonic() - start
            output = DEST / (blif.stem + '-' + label + '.out')
            write_schedule(output, kind, steps)
            check(blif, output, limits)
            row.update({label + '_opt': optimum, label + '_states': states,
                        label + '_transitions': transitions, label + '_seconds': f'{seconds:.6f}',
                        label + '_check': 'OK'})
            answers.append(optimum)
        row['equal'] = answers[0] == answers[1]
        data.append(row)
    with (ROOT / 'notes/non-delay-validation.tsv').open('w') as out:
        writer = csv.DictWriter(out, fieldnames=list(data[0]), delimiter='\t', lineterminator='\n')
        writer.writeheader(); writer.writerows(data)
    # Fixed-priority counterexample: selecting independent AND n0 before AND n1
    # delays n1's OR successor; both candidate schedules remain non-delay.
    kind, pred, limits = [0, 0, 1], [0, 0, 2], [1, 1, 1]
    blif = ROOT / 'notes/non-delay-fixed-priority-gap.blif'
    write_blif(blif, kind, pred)
    optimum, steps, _, _ = solve(kind, pred, limits, True)
    fixed = greedy(kind, pred, limits)
    for label, selected in [('optimal', steps), ('fixed', fixed)]:
        output = ROOT / ('notes/non-delay-fixed-priority-gap-' + label + '.out')
        write_schedule(output, kind, selected); check(blif, output, limits)
    assert optimum == 2 and max(fixed) == 3
    summary = {
        'kind': 'theory validation; not eval.py, not mlrcs -h, not hard-suite metrics',
        'random_seed': 661704, 'cases': 100, 'ops_range': [min(r['ops'] for r in data), max(r['ops'] for r in data)],
        'resources': 'each AND/OR/NOT capacity independently 1 or 2',
        'equal_optima': sum(r['equal'] for r in data), 'counterexamples': [r for r in data if not r['equal']],
        'checker_ok': 200, 'fixed_priority_toy_checker_ok': 2,
        'fixed_priority_toy': {'non_delay_optimum': optimum, 'fixed_id_greedy': max(fixed)},
        'elapsed_seconds': round(time.monotonic() - started, 3),
        'idle_dp_states': sum(r['idle_states'] for r in data),
        'non_delay_dp_states': sum(r['non_delay_states'] for r in data),
        'script_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'artifacts': str(DEST),
    }
    (ROOT / 'notes/non-delay-validation-summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    assert summary['equal_optima'] == 100


if __name__ == '__main__':
    main()
