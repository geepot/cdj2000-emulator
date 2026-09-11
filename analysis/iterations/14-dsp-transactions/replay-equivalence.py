"""Fixed firmware continuation equivalence across production core objects.

Uses the same snapshot replay adapter/peripherals for every variant; this is
cross-variant determinism, not a connected transcript or timing-oracle gate.
Run after connected measurements, never concurrently with them.
"""
import hashlib,json,os,subprocess,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3];OUT=Path(__file__).resolve().parent
BASE=ROOT/'build/performance/14-baseline';WORK=ROOT/'build/performance/14'
sys.path.insert(0,str(BASE))
from tools.cdj_dsp.replay import SOURCES
labels=('baseline','clear','copy','combined')
env=os.environ.copy();env['DEVELOPER_DIR']='/Library/Developer/CommandLineTools';env['CDJ_DSP_COMPACT_TRACE']='1'
for key in ('CDJ_NXS_DSP_FUNCTIONAL_TIMING','CDJ_NXS_DSP_FUNCTIONAL_AUDIO','CDJ_NXS_DSP_TX_CAPTURE','CDJ_DSP_CONNECTED_STOPS','CDJ_DSP_OBSERVE_PCM'): env.pop(key,None)
for label in labels:
 cmd=['cc','-O2','-I',str(BASE/'emulator/qemu'),str(WORK/label/'core.o'),*[str(p) for p in SOURCES if p.name!='cdj_c674x.c'],'-o',str(WORK/label/'replay')]
 subprocess.run(cmd,env=env,check=True)
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
rows=[]
for checkpoint_number in (1,25,250):
 checkpoint=ROOT/'runs/optimization-13-copy-profile/dsp-checkpoints'/f'{checkpoint_number:020d}.cdjdsp'
 expected=None
 for label in labels:
  dest=WORK/label/f'replay-{checkpoint_number}';dest.mkdir(exist_ok=False)
  cmd=[str(WORK/label/'replay'),str(checkpoint),'1000000','0','0','0','0',str(dest/'final.cdjdsp')]
  with (dest/'trace.jsonl').open('wb') as trace, (dest/'stderr.txt').open('wb') as errors:
   start=time.perf_counter();p=subprocess.run(cmd,env=env,stdout=trace,stderr=errors,check=True);elapsed=time.perf_counter()-start
  hashes={n:sha(dest/n) for n in ('trace.jsonl','final.cdjdsp')}
  if expected is None:expected=hashes
  assert hashes==expected,(checkpoint_number,label,hashes,expected)
  rows.append({'checkpoint':checkpoint_number,'input_sha256':sha(checkpoint),'label':label,'elapsed_seconds':elapsed,'hashes':hashes,'trace_tail':(dest/'trace.jsonl').read_text().splitlines()[-1]})
(OUT/'replay-equivalence.json').write_text(json.dumps({'scope':__doc__,'runs':rows},indent=2)+'\n')
print('All 12 continuations match exact trace and final checkpoint across variants.')
