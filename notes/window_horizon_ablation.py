#!/usr/bin/env python3
"""Pinned diagnostic harness. No common tools modified. See --help and companion md."""
import argparse,csv,datetime,hashlib,io,json,os,re,shutil,subprocess,tarfile,time
from pathlib import Path
ROOT=Path(__file__).resolve().parent.parent
BASE=ROOT/'shared/tmp/codex/window-horizon'
BINARY=BASE/'build/mlrcs'
MODES=('path-min','resource-min','path-feas','resource-feas')
ENV=os.environ.copy();ENV.update(PATH='/opt/gurobi/bin:'+ENV.get('PATH',''),LD_LIBRARY_PATH='/opt/gurobi/lib',TMPDIR=str(ROOT/'shared/tmp/codex'))
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def prepare():
    target=BASE/'src';target.mkdir(parents=True,exist_ok=True)
    data=subprocess.check_output(['git','archive','b376c9b','CMakeLists.txt','src'],cwd=ROOT)
    with tarfile.open(fileobj=io.BytesIO(data)) as a:a.extractall(target,filter='data')
    shutil.copyfile(ROOT/'notes/window_horizon_ilp.cpp',target/'src/ilp.cpp')
    for file in ('bounds.cpp','bounds.hpp','opgraph.cpp','opgraph.hpp'):
        (target/'src'/file).write_bytes(subprocess.check_output(['git','show',f'55567d1:src/{file}'],cwd=ROOT))
    manifest={'baseline_commit':subprocess.check_output(['git','rev-parse','b376c9b'],cwd=ROOT,text=True).strip(),'bound_source_commit':subprocess.check_output(['git','rev-parse','55567d1'],cwd=ROOT,text=True).strip(),'sha256':{str(p.relative_to(target)):sha(p) for p in sorted(target.rglob('*')) if p.is_file()}}
    (ROOT/'notes/window-horizon-source-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    start=time.monotonic()
    with (BASE/'build.log').open('w') as log:
        for cmd in (['cmake','-S',str(target),'-B',str(BASE/'build')],['cmake','--build',str(BASE/'build'),'-j','4']):
            subprocess.run(['taskset','-c','4-7',*cmd],stdout=log,stderr=subprocess.STDOUT,check=True,timeout=60)
    (BASE/'build.json').write_text(json.dumps({'seconds':time.monotonic()-start,'cpu':'4-7','manifest':sha(ROOT/'notes/window-horizon-source-manifest.json')}))
    print('built',BINARY,flush=True)
def run(path,cap,mode,directory,seconds=55,force=False):
    directory.mkdir(parents=True,exist_ok=False)
    env=ENV.copy();env['MLRCS_DIAG_MODE']=mode;env['MLRCS_ABLATION_META']=str(directory/'size.tsv')
    if force:env['MLRCS_DIAG_FORCE']='1'
    cmd=['taskset','-c','4-7',str(BINARY),'-e',str(path),*map(str,cap),'--time',str(seconds),'--threads','4','--stats','--keep',str(directory/'model')]
    (directory/'command.json').write_text(json.dumps({'command':cmd,'environment':{k:env[k] for k in ('MLRCS_DIAG_MODE','MLRCS_ABLATION_META','PATH','LD_LIBRARY_PATH','TMPDIR')}},indent=2)+'\n')
    start=time.monotonic();code='timeout'
    with (directory/'stdout').open('w') as out,(directory/'stderr').open('w') as err:
        try:code=subprocess.run(cmd,stdout=out,stderr=err,env=env,timeout=60 if seconds==55 else seconds+5).returncode
        except subprocess.TimeoutExpired:pass
    wall=time.monotonic()-start
    check=subprocess.run(['python3',str(ROOT/'tools/check.py'),str(path),*map(str,cap),str(directory/'stdout'),'--mode','e'],capture_output=True,text=True)
    (directory/'check.txt').write_text(check.stdout+check.stderr)
    err=(directory/'stderr').read_text();out=(directory/'stdout').read_text()
    m=re.search(r'mlrcs-stats (.*)',err);stats=dict(x.split('=',1) for x in m[1].split()) if m else {}
    latency=re.search(r'LATENCY: (\d+)',out)
    logpath=directory/'model/gurobi.log';log=logpath.read_text() if logpath.exists() else ''
    diagnostic=re.search(r'window-horizon mode=\S+ ub=(\d+) H=(\d+) start=(\d+)',err)
    size=(directory/'size.tsv').read_text().strip().split('\t') if (directory/'size.tsv').exists() else []
    selected=[l for l in log.splitlines() if re.search(r'Presolve|Presolved:|Root relaxation|Explored |Barrier solved|Crossover|Optimal solution|infeasible|Time limit reached|Interrupt request|Solve interrupted|Solution count|Best objective|User MIP start|Loaded user',l)]
    summary='\n'.join(selected)+'\n';(directory/'log-summary.txt').write_text(summary)
    rec={'case':path.stem+':'+','.join(map(str,cap)),'mode':mode,'latency':int(latency[1]) if latency else None,'wall_seconds':wall,'returncode':code,'check':'OK' if check.returncode==0 and code==0 else 'FAIL','stats':stats,'original_UB':int(diagnostic[1]) if diagnostic else None,'H':int(diagnostic[2]) if diagnostic else None,'mip_start':bool(int(diagnostic[3])) if diagnostic else None,'binaries':int(size[4]) if size else 0,'LPbytes':int(size[5]) if size else 0,'rows':int(size[6]) if size else 0,'raw_solver':re.findall(r'window-horizon solver=(.*)',err),'log_summary':selected,'directory':str(directory.relative_to(ROOT)),'hashes':{str(p.relative_to(directory)):sha(p) for p in sorted(directory.rglob('*')) if p.is_file()}}
    (directory/'record.json').write_text(json.dumps(rec,indent=2)+'\n')
    return rec
def formal():
    rows=[]
    for case,cap in (('aoi_C1355',(2,2,1)),('codex_gap',(1,1,1))):
        for mode in MODES:
            rec=run(ROOT/'benchmark'/f'{case}.blif',cap,mode,BASE/'formal'/f'{case}-{mode}')
            rows.append(rec)
            (ROOT/'notes/window-horizon-runs.json').write_text(json.dumps(rows,indent=2)+'\n')
            stats=rec['stats']
            with (ROOT/'notes/codex-results.tsv').open('a') as f:
                csv.writer(f,delimiter='\t',lineterminator='\n').writerow([datetime.datetime.now().strftime('%Y-%m-%d %H:%M'),'b376c9b+window-horizon',rec['case'],'e',rec['latency'],stats.get('lower_bound',''),rec['original_UB'],stats.get('status',''),f"{rec['wall_seconds']:.2f}",rec['check'],'window-horizon-diag:'+mode])
            print(rec['case'],mode,rec['latency'],stats,rec['wall_seconds'],flush=True)
            assert rec['check']=='OK',rec
    columns=['case','mode','original_UB','H','mip_start','latency','status','lower_bound','wall_seconds','binaries','rows','LPbytes','raw_solver','presolve','root_relaxation','explored_nodes','reason','check']
    with (ROOT/'notes/window-horizon-model-size.tsv').open('w') as f:
        w=csv.DictWriter(f,fieldnames=columns,delimiter='\t',lineterminator='\n');w.writeheader()
        for r in rows:
            d={k:r.get(k,'') for k in columns};d.update(status=r['stats'].get('status',''),lower_bound=r['stats'].get('lower_bound',''))
            for key,pat in [('presolve',r'^Presolve.*'),('root_relaxation',r'^Root relaxation:.*'),('explored_nodes',r'^Explored .*'),('reason',r'^(?:Optimal solution found|Model is infeasible|Time limit reached|Interrupt request received|Solve interrupted).*')]:
                d[key]=' | '.join(l for l in r['log_summary'] if re.search(pat,l)) or 'unobserved'
            w.writerow(d)
if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('action',choices=['prepare','formal']);args=parser.parse_args()
    prepare() if args.action=='prepare' else formal()
