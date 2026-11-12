#!/usr/bin/env python3
"""Independent tiny-DAG subset-DP validation for the compact model.

Prepare with compact_ablation.py, build shared/tmp/codex/compact-build, then:
python3 notes/check_compact.py
All generated inputs, stdout/stderr and kept gap ILP live under shared/tmp/codex/compact-validation.
"""
import csv
import os
from pathlib import Path
import random
import re
import subprocess
ROOT=Path(__file__).resolve().parent.parent
BASE=ROOT/'shared/tmp/codex/compact-validation'
BINARY=ROOT/'shared/tmp/codex/compact-build/mlrcs'
ENV=os.environ.copy();ENV['PATH']='/opt/gurobi/bin:'+ENV.get('PATH','');ENV['LD_LIBRARY_PATH']='/opt/gurobi/lib';ENV['TMPDIR']=str(ROOT/'shared/tmp/codex')
def optimum(pred,kind,cap):
    end=1<<len(pred);dp=[999]*end;dp[0]=0
    for mask in range(end):
        ready=sum(1<<i for i,p in enumerate(pred) if not(mask>>i&1) and p&mask==p)
        sub=ready
        while sub:
            counts=[sum(bool(sub>>i&1) and kind[i]==k for i in range(len(pred))) for k in range(3)]
            if all(counts[k]<=cap[k] for k in range(3)):dp[mask|sub]=min(dp[mask|sub],dp[mask]+1)
            sub=(sub-1)&ready
    return dp[-1]
def baseline(pred,kind,cap):
    n=len(pred);height=[1]*n
    for i in range(n-1,-1,-1):
        height[i]=1+max((height[j] for j in range(i+1,n) if pred[j]>>i&1),default=0)
    mask=0;t=0
    while mask!=(1<<n)-1:
        chosen=[]
        for k in range(3):
            ready=[i for i in range(n) if kind[i]==k and not(mask>>i&1) and pred[i]&mask==pred[i]]
            chosen+=sorted(ready,key=lambda i:(-height[i],i))[:cap[k]]
        mask|=sum(1<<i for i in chosen);t+=1
    return t

def blif(path,pred,kind):
    lines=['.model tiny','.inputs a b','.outputs '+' '.join('n'+str(i) for i in range(len(pred)))]
    for i,(p,k) in enumerate(zip(pred,kind)):
        inputs=['n'+str(j) for j in range(i) if p>>j&1]
        if k==2:inputs=inputs[:1] or ['a']
        else:
            if not inputs:inputs=['a','b']
            elif len(inputs)==1:inputs+=['a']
        lines+=['.names '+' '.join(inputs+['n'+str(i)])]
        if k==2:lines+=['0 1']
        elif k==0:lines+=['1'*len(inputs)+' 1']
        else:lines+=['-'*j+'1'+'-'*(len(inputs)-j-1)+' 1' for j in range(len(inputs))]
    path.write_text('\n'.join(lines+['.end'])+'\n')
def run(path,cap,label,exact):
    out=BASE/(label+'.out');err=BASE/(label+'.err')
    cmd=['taskset','-c','4-7',str(BINARY),'-e',str(path),*map(str,cap),'--stats','--threads','1','--time','5']
    if label=='gap':cmd+=['--keep',str(BASE/'gap-model')]
    with out.open('w') as stdout,err.open('w') as stderr:
        result=subprocess.run(cmd,env=ENV,stdout=stdout,stderr=stderr,timeout=8)
    check=subprocess.run(['python3',str(ROOT/'tools/check.py'),str(path),*map(str,cap),str(out),'--mode','e'],capture_output=True,text=True)
    assert result.returncode==0 and check.returncode==0,(label,result.returncode,err.read_text(),check.stdout,check.stderr)
    latency=int(re.search(r'LATENCY: (\d+)',out.read_text())[1]);assert latency==exact,(label,latency,exact)
    stats=dict(x.split('=',1) for x in re.search(r'mlrcs-stats (.*)',err.read_text())[1].split())
    assert stats['status'] in ('bound','optimal'),(label,stats)
    return [label,','.join(map(str,cap)),exact,latency,stats['status'],'OK']
def main():
    BASE.mkdir(parents=True,exist_ok=True)
    rows=[run(ROOT/'benchmark/codex_gap.blif',(1,1,1),'gap',3)]
    rng=random.Random(20261112)
    for case in range(20):
        n=rng.randrange(5,10);kind=[rng.randrange(3) for _ in range(n)];pred=[]
        for i,k in enumerate(kind):
            parents=[j for j in range(i) if rng.randrange(4)==0]
            if k==2:parents=parents[:1]
            pred.append(sum(1<<j for j in parents))
        cap=tuple(rng.randrange(1,3) for _ in range(3));path=BASE/f'tiny{case:02}.blif';blif(path,pred,kind)
        rows.append(run(path,cap,f'tiny{case:02}',optimum(pred,kind,cap)))
    # Require additional cases whose actual heuristic exceeds the independent
    # optimum, so the lower bound cannot bypass the compact model.
    found=0
    for attempt in range(10000):
        n=rng.randrange(6,10);kind=[rng.randrange(3) for _ in range(n)];pred=[]
        for i,k in enumerate(kind):
            parents=[j for j in range(i) if rng.randrange(4)==0]
            if k==2:parents=parents[:1]
            pred.append(sum(1<<j for j in parents))
        cap=(1,1,1);exact=optimum(pred,kind,cap)
        if baseline(pred,kind,cap)<=exact:continue
        label=f'gap{found:02}';path=BASE/(label+'.blif');blif(path,pred,kind)
        row=run(path,cap,label,exact)
        assert row[4]=='optimal',(label,'expected solver exercise',row)
        rows.append(row);found+=1
        if found==10:break
    assert found==10,found
    with (ROOT/'notes/compact-validation.tsv').open('w') as f:
        writer=csv.writer(f,delimiter='\t',lineterminator='\n');writer.writerow(['case','resources','oracle_optimum','latency','status','check']);writer.writerows(rows)
    print('all',len(rows),'checker and exact-DP OK; modeled',sum(r[4]=='optimal' for r in rows),'bound',sum(r[4]=='bound' for r in rows))
if __name__=='__main__':main()
