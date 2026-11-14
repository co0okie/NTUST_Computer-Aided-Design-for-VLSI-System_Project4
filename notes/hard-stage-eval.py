#!/usr/bin/env python3
"""Capture a current source snapshot and run the formal frozen-reference hard50 suite."""
import csv,hashlib,json,os,shutil,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parent.parent
TAG='hard-order-stage1'
TMP=(ROOT/'shared/tmp/codex').resolve()
SNAP=TMP/TAG
NOTES=ROOT/'notes'
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
assert not SNAP.exists()
assert not any(r['note'].split()[0]==TAG for r in csv.DictReader((NOTES/'codex-results.tsv').open(),delimiter='\t'))
SNAP.mkdir(parents=True)
shutil.copy2(ROOT/'CMakeLists.txt',SNAP/'CMakeLists.txt')
shutil.copytree(ROOT/'src',SNAP/'src')
files={str(p.relative_to(SNAP)):digest(p) for p in sorted(SNAP.rglob('*')) if p.is_file()}
sha=hashlib.sha256(json.dumps(files,sort_keys=True).encode()).hexdigest()
rev=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
manifest={'revision':rev,'working_tree':subprocess.check_output(['git','status','--short'],cwd=ROOT,text=True),'snapshot':str(SNAP),'files':files,'sha256':sha,'reference_sha256':digest(NOTES/'hard-best-reference.tsv')}
(NOTES/(TAG+'-source-manifest.json')).write_text(json.dumps(manifest,indent=2)+'\n')
for p in sorted(SNAP.rglob('*'),reverse=True):p.chmod(0o555 if p.is_dir() else 0o444)
SNAP.chmod(0o555)
side=TMP/(TAG+'-internal.tsv'); assert not side.exists()
cases=[]
for row in csv.DictReader((ROOT/'benchmark/hard/manifest.tsv').open(),delimiter='\t'):
 name,limits=row['case'].rsplit(':',1);cases.append(str(ROOT/'benchmark/hard'/f'{name}.blif')+':'+limits)
cmd=['taskset','-c','4-7','python3',str(ROOT/'tools/eval.py'),'--src',str(SNAP),'--cpu','4-7','--mode','h','--tsv',str(NOTES/'codex-results.tsv'),'--note',f'{TAG} source={rev}+working-tree sha256={sha}',*cases]
env={**os.environ,'TMPDIR':str(TMP),'MLRCS_ORDER_LOG':str(side)}
(NOTES/(TAG+'-command.json')).write_text(json.dumps({'command':cmd,'TMPDIR':str(TMP),'MLRCS_ORDER_LOG':str(side)},indent=2)+'\n')
with (NOTES/(TAG+'.log')).open('w') as out:r=subprocess.run(cmd,cwd=ROOT,env=env,stdout=out,stderr=subprocess.STDOUT)
if side.exists():
 with (NOTES/(TAG+'-internal.tsv')).open('w') as out:
  out.write('model\tand\tor\tnot\tseed_latency\tfinal_latency\tattempts\tfeasible\taccepted\tenabled\treason\n');out.write(side.read_text())
raise SystemExit(r.returncode)
