import sys, subprocess, time, os, json, hashlib
from pathlib import Path
sys.path.insert(0,str(Path.cwd()))
from tools.cdj_dsp.replay import SOURCES
out=Path('/tmp/cdj-iteration-audit')
env=os.environ.copy()
env['CDJ_NXS_DSP_FUNCTIONAL_TIMING']='1'
env['CDJ_NXS_DSP_FUNCTIONAL_AUDIO']='1'
rows=[]
for opt in ('-O0','-O2'):
 t=time.monotonic(); binary=out/opt[1:]
 subprocess.run(['cc',opt,'-std=c11','-Iemulator/qemu',*map(str,SOURCES),'-o',str(binary)],check=True)
 rows.append(dict(kind='compile',opt=opt,seconds=time.monotonic()-t))
for trial in range(3):
 for opt in ('-O0','-O2') if trial%2==0 else ('-O2','-O0'):
  trace=out/f'{opt}-{trial}.jsonl'; checkpoint=out/f'{opt}-{trial}.cdjdsp'
  t=time.monotonic()
  with trace.open('wb') as f:
   p=subprocess.run([str(out/opt[1:]),'runs/dsp-post-interrupt-pipedown-5m-replay-2/final.cdjdsp','300000','0','0','0','7',str(checkpoint)],stdout=f,stderr=subprocess.PIPE,env=env)
  rows.append(dict(kind='execute',opt=opt,trial=trial,seconds=time.monotonic()-t,returncode=p.returncode,stderr=p.stderr.decode(),bytes=trace.stat().st_size,trace_sha256=hashlib.sha256(trace.read_bytes()).hexdigest(),checkpoint_sha256=hashlib.sha256(checkpoint.read_bytes()).hexdigest() if checkpoint.exists() else None))
(out/'measurements.json').write_text(json.dumps(rows,indent=2))
print(json.dumps(rows,indent=2))
