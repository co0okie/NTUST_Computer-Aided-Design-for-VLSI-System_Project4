#!/usr/bin/env python3
"""Check file-only output, early legal fallback, C API linkage and no-Gurobi build."""
import argparse,json,os,subprocess,time
from pathlib import Path
ROOT=Path(__file__).resolve().parent.parent
p=argparse.ArgumentParser();p.add_argument('binary');p.add_argument('without_gurobi');p.add_argument('directory');a=p.parse_args()
binary=Path(a.binary).resolve();without=Path(a.without_gurobi).resolve();directory=Path(a.directory).resolve();directory.mkdir(parents=True,exist_ok=False)
blif=ROOT/'benchmark/hard/layered_s129_n252.blif';records=[]
def check(folder,mode):
 out=folder/(blif.stem+'_'+mode+'.out')
 r=subprocess.run(['python3',str(ROOT/'tools/check.py'),str(blif),'5','3','2',str(out),'--mode',mode],capture_output=True,text=True)
 assert r.returncode==0,(r.stdout,r.stderr)
 return r.stdout.strip()
def run(label,program,mode,options=(),ok=True):
 folder=directory/label;folder.mkdir();cmd=[str(program),'-'+mode,str(blif),'5','3','2',*options]
 env={**os.environ,'PATH':'/usr/bin:/bin'} # no gurobi_cl on PATH
 with (folder/'stdout.txt').open('w') as out,(folder/'stderr.txt').open('w') as err:
  r=subprocess.run(cmd,cwd=folder,env=env,stdout=out,stderr=err,timeout=8)
 assert (r.returncode==0)==ok,(label,r.returncode,(folder/'stderr.txt').read_text())
 records.append({'case':label,'returncode':r.returncode,'check':check(folder,mode) if ok else 'expected error'})
run('short-h',binary,'h',['--time','0.05'])
run('short-e-no-cli',binary,'e',['--time','0.05'])
run('without-gurobi-h',without,'h',['--time','0.05'])
run('without-gurobi-e',without,'e',ok=False)
for value in ['nan','inf','1e300']:
 run('bad-time-'+value,binary,'h',['--time',value],ok=False)
run('bad-threads',binary,'h',['--threads','9223372036854775807'],ok=False)
# Interrupt the longer heuristic immediately after the first atomic output arrives.
folder=directory/'early-kill';folder.mkdir()
with (folder/'stdout.txt').open('w') as out,(folder/'stderr.txt').open('w') as err:
 child=subprocess.Popen([str(binary),'-h',str(blif),'5','3','2'],cwd=folder,stdout=out,stderr=err)
 try:
  deadline=time.monotonic()+5
  while not (folder/(blif.stem+'_h.out')).exists() and child.poll() is None and time.monotonic()<deadline:time.sleep(.01)
  assert (folder/(blif.stem+'_h.out')).exists(),'no early legal output'
  child.kill();child.wait()
 finally:
  if child.poll() is None:child.kill();child.wait()
records.append({'case':'early-kill','check':check(folder,'h')})
(directory/'summary.json').write_text(json.dumps(records,indent=2)+'\n')
print('PASS',len(records),'output-contract checks')
