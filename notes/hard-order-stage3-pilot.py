#!/usr/bin/env python3
"""CPU4–7 stage3 pilot; stdlib only. Keeps output/checker/source provenance on disk."""
import csv, hashlib, json, os, pathlib, subprocess, time
ROOT = pathlib.Path(__file__).resolve().parents[1]
DEST = ROOT / 'shared/tmp/codex/hard-order-stage3-pilot'
DEST.mkdir(parents=True, exist_ok=True)
BINARY = ROOT / 'shared/tmp/codex/build-lns-stage2/mlrcs'
CASES = [('layered_s129_n252',5,3,2), ('funnel_s1210_n243',4,2,1),
         ('layered_s1008_n616',8,8,5), ('chains_s575_n2879',12,15,6),
         ('layered_s1076_n851',8,8,6)]
manifest = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted((ROOT/'src').glob('*')) if p.is_file()}
(DEST/'source.json').write_text(json.dumps(manifest,indent=2)+'\n')
with (ROOT/'notes/hard-order-stage3-pilot.tsv').open('w') as out:
    writer=csv.DictWriter(out,fieldnames=['case','latency','seconds','check','seed_latency','attempts','feasible','accepted','uphill','plateau','downhill','restarts','work'],delimiter='\t')
    writer.writeheader();out.flush()
    for model,a,o,n in CASES:
        stem=DEST/f'{model}-{a}-{o}-{n}'
        env=dict(os.environ, MLRCS_ORDER_LOG=str(stem)+'.internal.tsv', MLRCS_ORDER_SA_LOG=str(stem)+'.sa.tsv', MLRCS_ORDER_DIAGNOSTICS='1', MLRCS_ORDER_METHOD='anneal')
        for ext in ['.internal.tsv','.sa.tsv']:
            pathlib.Path(str(stem)+ext).unlink(missing_ok=True)
        blif=ROOT/'benchmark/hard'/f'{model}.blif'
        cmd=['taskset','-c','4-7',str(BINARY),'-h',str(blif),str(a),str(o),str(n),'--time','55','--threads','4','--stats']
        (pathlib.Path(str(stem)+'.command.json')).write_text(json.dumps(cmd)+'\n')
        start=time.monotonic()
        with pathlib.Path(str(stem)+'.out').open('w') as stdout, pathlib.Path(str(stem)+'.err').open('w') as stderr:
            run=subprocess.run(cmd,stdout=stdout,stderr=stderr,env=env,timeout=60)
        seconds=time.monotonic()-start
        assert run.returncode==0,(model,run.returncode)
        checked=subprocess.run(['taskset','-c','4-7','python3',str(ROOT/'tools/check.py'),str(blif),str(a),str(o),str(n),str(stem)+'.out','--mode','h'],capture_output=True,text=True,timeout=30)
        pathlib.Path(str(stem)+'.check').write_text(checked.stdout+checked.stderr)
        assert checked.returncode==0,(model,checked.stdout,checked.stderr)
        internal=pathlib.Path(str(stem)+'.internal.tsv').read_text().strip().split('\t')
        sa=pathlib.Path(str(stem)+'.sa.tsv').read_text().strip().split('\t')
        row=dict(case=f'{model}:{a},{o},{n}',latency=internal[5],seconds=f'{seconds:.6f}',check='OK',seed_latency=internal[4],attempts=internal[6],feasible=internal[7],accepted=internal[8],uphill=sa[3],plateau=sa[4],downhill=sa[5],restarts=sa[6],work=sa[7])
        writer.writerow(row);out.flush();print(row,flush=True)
