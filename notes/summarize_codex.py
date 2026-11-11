#!/usr/bin/env python3
"""Rebuild report tables and vector figure from committed eval rows (stdlib only)."""
import csv
import math
from pathlib import Path
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parent.parent
NOTES = ROOT / 'notes'
rows = list(csv.DictReader((NOTES / 'codex-results.tsv').open(), delimiter='\t'))
methods = [('baseline', 'Baseline'), ('edf-window-bound', 'EDF windows'),
           ('closure-count-single-edf', 'Closure + EDF')]
summary = []
for note, label in methods:
    cases = {r['case']: r for r in rows if r['note'] == note and r['mode'] == 'h'
             and r['case'].startswith('aoi_') and r['check'] == 'OK'}
    if not cases:
        continue
    summary.append(dict(method=label, cases=len(cases),
                        proven_by_bound=sum(r['latency'] == r['lower_bound'] for r in cases.values()),
                        runtime_seconds=f"{sum(float(r['time']) for r in cases.values()):.2f}",
                        geomean_h_over_lb=f"{math.exp(sum(math.log(int(r['latency']) / int(r['lower_bound'])) for r in cases.values()) / len(cases)):.6f}"))
with (NOTES / 'codex-summary.tsv').open('w') as f:
    w = csv.DictWriter(f, fieldnames=list(summary[0]), delimiter='\t', lineterminator='\n')
    w.writeheader()
    w.writerows(summary)
svg = ['<svg xmlns="http://www.w3.org/2000/svg" width="760" height="330" viewBox="0 0 760 330">',
       '<rect width="760" height="330" fill="white"/>',
       '<g font-family="sans-serif" fill="#172334">',
       '<text x="30" y="35" font-size="21">Lower-bound ablation: unchanged heuristic, 76 cases</text>',
       '<text x="30" y="65" font-size="14">Cases with a checker-validated heuristic matching the lower bound</text>']
for i, row in enumerate(summary):
    y = 95 + i * 61
    v = row['proven_by_bound']
    svg += [f'<text x="30" y="{y+23}" font-size="15">{escape(row["method"])}</text>',
            f'<rect x="185" y="{y}" width="456" height="32" fill="#ecf0f5"/>',
            f'<rect x="185" y="{y}" width="{6*v}" height="32" fill="#256a98"/>',
            f'<text x="660" y="{y+23}" font-size="17">{v} / 76</text>']
svg += ['<text x="30" y="300" font-size="12">Source: codex-results.tsv. This chart compares bounds, not heuristic improvements.</text>', '</g></svg>']
(NOTES / 'codex-bound-ablation.svg').write_text('\n'.join(svg) + '\n')
for r in summary:
    print(r)
