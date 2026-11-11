#!/usr/bin/env python3
"""Regenerate the four-operation height-tie counterexample; no packages required.

Run from any directory: python3 notes/generate_codex_gap.py
With resources AND=1, OR=1, NOT=1 the baseline height-priority heuristic
schedules n0 / n1 / n2 / n3 (latency 4), while an optimum is
n1 / (n0,n3) / n2 (latency 3). Both NOT roots have height 2, so the
file-order tie break postpones n1 even though it unlocks an OR by itself.
The two NOT operations must finish before n2, giving a lower bound of 3.
"""
from pathlib import Path

BLIF = """# Four-operation height-tie counterexample; resources 1 1 1.
# Regenerate with python3 notes/generate_codex_gap.py.
.model codex_gap
.inputs a b
.outputs n2 n3
.names a n0
0 1
.names b n1
0 1
.names n0 n1 n2
1- 1
-1 1
.names n1 a n3
1- 1
-1 1
.end
"""

if __name__ == "__main__":
    destination = Path(__file__).resolve().parent.parent / "benchmark" / "codex_gap.blif"
    destination.write_text(BLIF)
    print(destination)
