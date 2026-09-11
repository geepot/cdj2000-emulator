"""Fixed-work synthetic diagnostic using the exact QEMU core objects."""
import hashlib,json,os,re,statistics,subprocess,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3];OUT=Path(__file__).resolve().parent
WORK=ROOT/'build/performance/14';BASE=ROOT/'build/performance/14-baseline'
source=WORK/'micro.c';source.write_text((BASE/'tools/cdj_dsp/benchmark_core.c').read_text().replace('3000000','9000000'))
env=os.environ.copy();env['DEVELOPER_DIR']='/Library/Developer/CommandLineTools'
labels=['baseline','clear','copy','combined']
for label in labels:
 cmd=['cc','-O2','-ftrivial-auto-var-init=zero','-fstack-protector-strong','-fzero-call-used-regs=used-gpr','-I',str(BASE/'emulator/qemu'),str(source),str(WORK/label/'core.o'),*[str(BASE/'emulator/qemu'/f'cdj_c674x_{n}.c') for n in ('loop','sp','control','mpy','uncond')],'-o',str(WORK/label/'micro')]
 subprocess.run(cmd,env=env,check=True)
rows=[]
for trial in range(5):
 for label in labels if trial%2==0 else labels[::-1]:
  text=subprocess.check_output([str(WORK/label/'micro')],text=True)
  elapsed=float(re.search(r'execute ([0-9.]+) s',text)[1]);assert 'packets=9000000 reg=123' in text
  rows.append({'label':label,'trial':trial,'execute_cpu_seconds':elapsed,'stdout':text})
medians={l:statistics.median(r['execute_cpu_seconds'] for r in rows if r['label']==l) for l in labels}
report={'scope':'Synthetic 9-million-MVK execute calls; exact production core object. Not firmware/replay/whole-player speed. Loop-issue timing is a separate unchanged synthetic path.','trials':rows,'medians':medians,'speedup_vs_baseline':{l:medians['baseline']/v for l,v in medians.items()}}
(OUT/'micro.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({k:v for k,v in report.items() if k!='trials'},indent=2))
