#!/usr/bin/env python3
"""Independent subset-DP, all four modes, ten h>OPT tiny DAGs and infeasible H test.
For validation only FORCE bypasses lower-bound shortcut so every model is exercised.
"""
import csv,json,random
from pathlib import Path
from check_compact import optimum,baseline,blif
from window_horizon_ablation import ROOT,BASE,MODES,run

def main():
    directory=BASE/'validation';directory.mkdir(parents=True,exist_ok=True)
    rng=random.Random(20261112);cases=[]
    for attempt in range(10000):
        n=rng.randrange(6,10);kind=[rng.randrange(3) for _ in range(n)];pred=[]
        for i,k in enumerate(kind):
            parents=[j for j in range(i) if rng.randrange(4)==0]
            if k==2:parents=parents[:1]
            pred.append(sum(1<<j for j in parents))
        cap=(1,1,1);opt=optimum(pred,kind,cap);h=baseline(pred,kind,cap)
        if h<=opt:continue
        label=f'tiny-gap{len(cases):02}';path=directory/(label+'.blif');blif(path,pred,kind)
        cases.append((label,path,cap,opt,h))
        if len(cases)==10:break
    assert len(cases)==10
    path=directory/'infeasible.blif';blif(path,[0]*4,[2]*4)
    cases.append(('infeasible',path,(1,1,1),4,4))
    rows=[];records=[]
    for label,path,cap,opt,h in cases:
        for mode in MODES:
            record_path=directory/(label+'-'+mode)/'record.json'
            r=json.loads(record_path.read_text()) if record_path.exists() else run(path,cap,mode,directory/(label+'-'+mode),seconds=10,force=True)
            assert r['check']=='OK' and r['original_UB']==h,(label,mode,r)
            assert opt<=r['latency']<=h,(label,mode,r)
            if mode.endswith('min'):assert r['latency']==opt and r['stats']['status'] in ('optimal','bound'),r
            if r['stats']['status'] in ('optimal','bound'):assert r['latency']==opt,r
            if label=='infeasible' and mode.endswith('feas'):
                assert r['raw_solver']==['infeasible killed=0 has_solution=0'],r
                assert r['stats']['status']=='optimal' and int(r['stats']['lower_bound'])==h,r
            rows.append([label,mode,h,opt,r['latency'],r['stats']['status'],r['raw_solver'],r['check']]);records.append(r)
    # Gap preflight is retained separately; formal gap still runs exactly once/mode.
    for mode in MODES:
        r=run(ROOT/'benchmark/codex_gap.blif',(1,1,1),mode,directory/('gap-preflight-'+mode),seconds=10)
        assert r['check']=='OK' and r['latency']==3,r
        rows.append(['gap-preflight',mode,4,3,r['latency'],r['stats']['status'],r['raw_solver'],r['check']]);records.append(r)
    with (ROOT/'notes/window-horizon-validation.tsv').open('w') as f:
        w=csv.writer(f,delimiter='\t',lineterminator='\n');w.writerow(['case','mode','heuristic','DP_optimum','latency','status','raw_solver','check']);w.writerows(rows)
    (ROOT/'notes/window-horizon-validation.json').write_text(json.dumps(records,indent=2)+'\n')
    print(len(rows),'checker/DP validations passed; all 40 tiny-gap models exercised, four infeasibility controls, four gap preflights')
if __name__=='__main__':main()
