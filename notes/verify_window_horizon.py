#!/usr/bin/env python3
"""Read-only verification of recorded hashes, schedules, status and run scope."""
import hashlib,json,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parent.parent
BASE=ROOT/'shared/tmp/codex/window-horizon'
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def main():
    manifest=json.loads((ROOT/'notes/window-horizon-source-manifest.json').read_text())
    for name,digest in manifest['sha256'].items():assert sha(BASE/'src'/name)==digest,name
    assert sha(ROOT/'notes/window_horizon_ilp.cpp')==manifest['sha256']['src/ilp.cpp']
    rows=json.loads((ROOT/'notes/window-horizon-runs.json').read_text())
    assert len(rows)==8 and len({(r['case'],r['mode']) for r in rows})==8
    validation=json.loads((ROOT/'notes/window-horizon-validation.json').read_text())
    assert len(validation)==48
    for r in rows+validation:
        directory=ROOT/r['directory']
        for name,digest in r['hashes'].items():assert sha(directory/name)==digest,(directory,name)
        command=json.loads((directory/'command.json').read_text())['command'];i=command.index('-e');path=command[i+1];cap=command[i+2:i+5]
        check=subprocess.run(['python3',str(ROOT/'tools/check.py'),path,*cap,str(directory/'stdout'),'--mode','e'],capture_output=True,text=True)
        assert check.returncode==0,(directory,check.stdout,check.stderr)
        assert r['latency']<=r['original_UB']
        if r['mode'].endswith('feas'):
            assert not (directory/'model/start.mst').exists()
            if r['stats']['status']=='optimal':
                assert r['raw_solver']==['infeasible killed=0 has_solution=0'] and int(r['stats']['lower_bound'])==r['original_UB']
    print('source hashes and all 56 retained checker outputs verified; feasibility starts absent and proof statuses sound')
if __name__=='__main__':main()
