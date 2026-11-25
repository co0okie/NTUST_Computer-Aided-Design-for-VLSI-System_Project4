#!/usr/bin/env python3
"""Analyze retained pre/post C-API acceptance data without rerunning schedulers."""
import csv,json,re,subprocess,sys,hashlib
from pathlib import Path
ROOT=Path(__file__).resolve().parent.parent
BASE=(ROOT/'shared/tmp/codex/resume2').resolve()
FIELDS=['case','mode','before_latency','after_latency','before_status','after_status','before_seconds','after_seconds','before_maxrss_kb','after_maxrss_kb','latency_regressed']
def load(stage):
 p=ROOT/'notes'/f'resume2-{stage}-results.tsv'
 with p.open() as f: rows=list(csv.DictReader(f,delimiter='\t'))
 records={}
 for x in sorted((BASE/f'{stage}-outputs').glob('*/run.json')):
  m=json.loads(x.read_text());case=Path(m['blif']).stem+':'+','.join(map(str,m['limits']));key=(case,m['mode']);folder=x.parent
  out=folder/'stdout.txt' if stage=='before' else folder/(Path(m['blif']).stem+'_'+m['mode']+'.out')
  if stage!='before' and not out.exists():
   # The merged common evaluator may use a stable filename for the real output.
   candidates=[q for q in folder.glob('*.out') if q.is_file()]
   assert len(candidates)==1,(folder,candidates);out=candidates[0]
  assert hashlib.sha256((folder/'stdout.txt').read_bytes()).hexdigest()==m['stdout_sha256']
  assert hashlib.sha256((folder/'stderr.txt').read_bytes()).hexdigest()==m['stderr_sha256']
  if stage!='before':assert hashlib.sha256(out.read_bytes()).hexdigest()==m['result_sha256']
  check=subprocess.run([sys.executable,str(ROOT/'tools/check.py'),m['blif'],*map(str,m['limits']),str(out),'--mode',m['mode']],capture_output=True,text=True)
  assert check.returncode==0,(key,check.stdout,check.stderr)
  assert check.stdout.strip()=='OK latency='+m['result']['latency'],(key,check.stdout)
  match=re.search(r'resume2-maxrss-kb=(\d+)',(folder/'stderr.txt').read_text())
  records[key]={'maxrss_kb':int(match[1]) if match else None,'output':str(out),'sha256':hashlib.sha256(out.read_bytes()).hexdigest()}
 assert len(records)==len(rows),(stage,len(records),len(rows))
 assert all(r['check']=='OK' for r in rows)
 return {(r['case'],r['mode']):r for r in rows},records

def main():
 before,bmeta=load('before');after,ameta=load('after');assert before.keys()==after.keys();paired=[]
 for key,b in before.items():
  a=after[key];paired.append(dict(case=key[0],mode=key[1],before_latency=b['latency'],after_latency=a['latency'],before_status=b['status'],after_status=a['status'],before_seconds=b['time'],after_seconds=a['time'],before_maxrss_kb=bmeta[key]['maxrss_kb'],after_maxrss_kb=ameta[key]['maxrss_kb'],latency_regressed=int(a['latency'])>int(b['latency'])))
 with (ROOT/'notes/resume2-paired.tsv').open('w') as f:
  w=csv.DictWriter(f,fieldnames=FIELDS,delimiter='\t');w.writeheader();w.writerows(paired)
 proof=lambda rows:sum(r['mode']=='e' and r['status'] in ('optimal','bound') for r in rows.values())
 summary={'runs_per_stage':len(before),'checker_rechecked':len(before)+len(after),'before_proven':proof(before),'after_proven':proof(after),'latency_regressions':[r for r in paired if r['latency_regressed']],'latency_improvements':[r for r in paired if int(r['after_latency'])<int(r['before_latency'])],'before_total_seconds':round(sum(float(r['time']) for r in before.values()),2),'after_total_seconds':round(sum(float(r['time']) for r in after.values()),2),'before_peak_maxrss_kb':max(v['maxrss_kb'] or 0 for v in bmeta.values()),'after_peak_maxrss_kb':max(v['maxrss_kb'] or 0 for v in ameta.values()),'outputs':{'before':bmeta,'after':ameta}}
 # JSON keys encode case+mode because tuple keys are not JSON serializable.
 summary['e_worse_than_h']={stage:[c for c,m in data if m=='e' and int(data[(c,'e')]['latency'])>int(data[(c,'h')]['latency'])] for stage,data in [('before',before),('after',after)]}
 summary['outputs']={s:{c+' '+m:v for (c,m),v in data.items()} for s,data in summary['outputs'].items()}
 (ROOT/'notes/resume2-summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps({k:v for k,v in summary.items() if k!='outputs'},indent=2))
if __name__=='__main__':main()
