import sys,time,resource,json,hashlib
from pathlib import Path
sys.path.insert(0,str(Path.cwd()))
from tools.cdj_dsp.coverage import build_coverage
p=Path('/tmp/cdj-iteration-audit')
mode=sys.argv[1]
trace=p/'-O2-2.jsonl'
if mode=='prepare':
 with trace.open('rb') as f, (p/'compact.jsonl').open('wb') as g:
  for line in f:
   if not line.startswith(b'{"event":"step",'):g.write(line)
 sys.exit()
if mode=='compact':trace=p/'compact.jsonl'
t=time.monotonic()
c=build_coverage((p/'-O2-2.cdjdsp').read_bytes(),trace.read_bytes(),Path('build/gdb-17.2/include/opcode/tic6x-insn-formats.h').read_bytes())
r=dict(mode=mode,seconds=time.monotonic()-t,maxrss_bytes=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,trace_bytes=trace.stat().st_size,coverage_sha256=hashlib.sha256(json.dumps(c,sort_keys=True).encode()).hexdigest())
(p/f'{mode}-analysis.json').write_text(json.dumps(r,indent=2));print(json.dumps(r))
