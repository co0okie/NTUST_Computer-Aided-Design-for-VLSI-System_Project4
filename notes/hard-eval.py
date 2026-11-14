#!/usr/bin/env python3
"""Official hard-suite evaluation with immutable source provenance and frozen best reference.
Prepare: python3 notes/hard-eval.py --prepare
Run: taskset -c 4-7 python3 notes/hard-eval.py --src shared/tmp/codex/hard-baseline-codex --note hard-baseline-codex
Run: taskset -c 4-7 python3 notes/hard-eval.py --src shared/tmp/codex/hard-baseline-claude --note hard-baseline-claude
Summarize: python3 notes/hard-eval.py --summarize
"""
import argparse, csv, hashlib, json, math, os, subprocess, tarfile, tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parent.parent
NOTES = ROOT / 'notes'
TMP = (ROOT / 'shared/tmp/codex').resolve()
REFERENCE = NOTES / 'hard-best-reference.tsv'
MANIFEST = ROOT / 'benchmark/hard/manifest.tsv'
PROVENANCE = NOTES / 'hard-baselines-source-manifest.json'
RESULTS = NOTES / 'codex-results.tsv'

def read_tsv(path):
    with path.open() as stream:
        return list(csv.DictReader(stream, delimiter='\t'))

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def cases():
    return [r['case'] for r in read_tsv(MANIFEST)]

def prepare():
    if PROVENANCE.exists():
        raise SystemExit('Provenance already exists; do not recreate snapshots.')
    TMP.mkdir(parents=True, exist_ok=True)
    wanted = cases()
    best = {r['case']:r for r in read_tsv(ROOT / 'shared/best.tsv')}
    assert len(wanted) == 50 and len(set(wanted)) == 50
    assert all(c in best for c in wanted)
    assert not REFERENCE.exists(), 'Frozen reference already exists'
    with REFERENCE.open('w') as out:
        writer = csv.DictWriter(out, fieldnames=['case','best_latency','lower_bound','source','note'], delimiter='\t')
        writer.writeheader()
        writer.writerows(best[c] for c in wanted)
    provenance = {'manifest_sha256':digest(MANIFEST), 'reference_sha256':digest(REFERENCE), 'sources':{}}
    for label, rev in [('codex','HEAD'),('claude','f275c81')]:
        revision = subprocess.check_output(['git','rev-parse',rev],cwd=ROOT,text=True).strip()
        snapshot = TMP / ('hard-baseline-' + label)
        assert not snapshot.exists(), snapshot
        snapshot.mkdir()
        with tempfile.TemporaryFile(dir=TMP) as archive:
            subprocess.run(['git','archive',revision,'CMakeLists.txt','src'],cwd=ROOT,stdout=archive,check=True)
            archive.seek(0)
            with tarfile.open(fileobj=archive) as tar:
                tar.extractall(snapshot, filter='data')
        hashes = {str(p.relative_to(snapshot)):digest(p) for p in sorted(snapshot.rglob('*')) if p.is_file()}
        aggregate = hashlib.sha256(json.dumps(hashes,sort_keys=True).encode()).hexdigest()
        provenance['sources'][label] = {'revision':revision,'snapshot':str(snapshot),'files':hashes,'sha256':aggregate}
        for p in sorted(snapshot.rglob('*'),reverse=True):
            p.chmod(0o555 if p.is_dir() else 0o444)
        snapshot.chmod(0o555)
    PROVENANCE.write_text(json.dumps(provenance,indent=2)+'\n')

def evaluate(src, note):
    source = Path(src).resolve()
    provenance = json.loads(PROVENANCE.read_text())
    record = next((s for s in provenance['sources'].values() if s['snapshot']==str(source)),None)
    assert record, 'Source missing from baseline manifest'
    assert all(digest(source/p)==sha for p,sha in record['files'].items())
    old = [r for r in read_tsv(RESULTS) if r['note'].split()[0]==note]
    assert not old, 'Evaluation already recorded; avoid duplicate baseline'
    selected = []
    for case in cases():
        name, limits = case.rsplit(':',1)
        selected.append(str(ROOT/'benchmark/hard'/f'{name}.blif')+':'+limits)
    detail = f"{note} source={record['revision']} sha256={record['sha256']}"
    cmd = ['taskset','-c','4-7','python3',str(ROOT/'tools/eval.py'),'--src',str(source),'--cpu','4-7','--mode','h','--tsv',str(RESULTS),'--note',detail,*selected]
    log = NOTES / (note+'.log')
    (NOTES/(note+'-command.json')).write_text(json.dumps({'command':cmd,'TMPDIR':str(TMP)},indent=2)+'\n')
    with log.open('w') as out:
        result = subprocess.run(cmd,cwd=ROOT,env={**os.environ,'TMPDIR':str(TMP)},stdout=out,stderr=subprocess.STDOUT)
    summarize()
    raise SystemExit(result.returncode)

def summarize():
    best = {r['case']:int(r['best_latency']) for r in read_tsv(REFERENCE)}
    data = read_tsv(RESULTS)
    summary = {'reference_sha256':digest(REFERENCE),'stages':{}}
    matched = {}
    for label in ['codex','claude']:
        tag = 'hard-baseline-'+label
        rows = [r for r in data if r['note'].split()[0]==tag]
        valid = {r['case']:r for r in rows if r['check']=='OK' and r['case'] in best}
        matched[label] = valid
        ratios = [int(r['latency'])/best[c] for c,r in valid.items()]
        summary['stages'][label] = {'rows':len(rows),'matched_checker_ok':len(valid),'failures':[r for r in rows if r['check']!='OK'], 'geomean':math.exp(sum(map(math.log,ratios))/len(ratios)) if ratios else None,'total_runtime':sum(float(r['time']) for r in rows),'max_runtime':max((float(r['time']) for r in rows),default=0),'equal_best':sum(int(r['latency'])==best[c] for c,r in valid.items())}
    common = sorted(matched['codex'].keys() & matched['claude'].keys())
    pairs = [{'case':c,'codex':int(matched['codex'][c]['latency']),'claude':int(matched['claude'][c]['latency']),'best':best[c]} for c in common]
    summary['paired'] = {'count':len(pairs),'codex_better':sum(p['codex']<p['claude'] for p in pairs),'claude_better':sum(p['claude']<p['codex'] for p in pairs),'equal':sum(p['claude']==p['codex'] for p in pairs)}
    (NOTES/'hard-baselines-summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    with (NOTES/'hard-baselines-paired.tsv').open('w') as out:
        writer=csv.DictWriter(out,fieldnames=['case','codex','claude','best'],delimiter='\t');writer.writeheader();writer.writerows(pairs)
    print(json.dumps(summary,indent=2))

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepare',action='store_true');parser.add_argument('--summarize',action='store_true');parser.add_argument('--src');parser.add_argument('--note')
    args=parser.parse_args()
    if args.prepare:prepare()
    elif args.summarize:summarize()
    else:
        assert args.src and args.note
        evaluate(args.src,args.note)
