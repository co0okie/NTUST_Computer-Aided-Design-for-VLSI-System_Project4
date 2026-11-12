#!/usr/bin/env python3
"""Prepare compact-cumulative experimental source snapshot; no production edits.

python3 notes/compact_ablation.py
TMPDIR="$PWD/shared/tmp/codex" MLRCS_ABLATION_META="$PWD/shared/tmp/codex/compact-model-size.tsv" python3 tools/eval.py --src shared/tmp/codex/compact-src --cpu 4-7 --threads 4 --mode e aoi_C1355:2,2,1 aoi_C1355:8,8,4 codex_gap:1,1,1 --tsv notes/codex-results.tsv --note compact-cumulative-ablation

Source is pinned to b376c9b (the scalar control's merge-stage code), overridden
only by notes/compact_ilp.cpp. The manifest records hashes because eval.py's
snapshot directory is inside the shared symlink and its git label is misleading.
"""
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
ROOT = Path(__file__).resolve().parent.parent
SOURCE = 'b376c9b'
def prepare():
    target = ROOT / 'shared/tmp/codex/compact-src'
    if target.exists():
        shutil.rmtree(target)
    target.mkdir(parents=True)
    data = subprocess.check_output(['git', '-C', str(ROOT), 'archive', SOURCE, 'CMakeLists.txt', 'src'])
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:
        archive.extractall(target, filter='data')
    shutil.copyfile(ROOT / 'notes/compact_ilp.cpp', target / 'src/ilp.cpp')
    files = [target / 'CMakeLists.txt', *sorted((target / 'src').rglob('*'))]
    manifest = {'base_commit': subprocess.check_output(['git','-C',str(ROOT),'rev-parse',SOURCE],text=True).strip(),
                'replacement': 'notes/compact_ilp.cpp',
                'sha256': {str(p.relative_to(target)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files if p.is_file()}}
    (target / 'source-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(target)
    return target
if __name__ == '__main__':
    prepare()
