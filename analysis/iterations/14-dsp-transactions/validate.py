"""Focused architecture and byte-equivalence gates. Run after measurements."""
import json,os,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3];OUT=Path(__file__).resolve().parent
BASE=ROOT/'build/performance/14-baseline';WORK=ROOT/'build/performance/14/work'
env=os.environ.copy();env['DEVELOPER_DIR']='/Library/Developer/CommandLineTools'
env['C6X_TI_BIN']='/Applications/ti/ti-cgt-c6000_8.5.0.LTS/bin'
tests=['tests/test_c674x.py','tests/test_c674x_spkernel_fields.py','tests/test_dsp_checkpoint_replay.py']
core=WORK/'emulator/qemu/cdj_c674x.c';saved=core.read_bytes()
try:
 for label,root in [('baseline',BASE),('clear',WORK),('combined',WORK)]:
  if label!='baseline':core.write_bytes((ROOT/f'build/performance/14/{label}.c').read_bytes())
  chosen=tests+(['tests/test_c674x_transaction.py'] if label!='baseline' else [])
  with (OUT/(label+'-tests.txt')).open('w') as log:
   subprocess.run([sys.executable,'-m','pytest','-q',*chosen],cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
  print(label,(OUT/(label+'-tests.txt')).read_text().strip(),flush=True)
 # Run the complete ISA C fixture with the combined core under sanitizers.
 binary=ROOT/'build/performance/14/full-core-san'
 sources=[WORK/'emulator/qemu/cdj_c674x.c',*[WORK/f'emulator/qemu/cdj_c674x_{n}.c' for n in ('uncond','mpy','sp','control','loop')]]
 with (OUT/'full-core-sanitizers.txt').open('w') as log:
  subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-ftrivial-auto-var-init=zero','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-I',str(WORK/'emulator/qemu'),str(WORK/'tests/cstub/c674x.c'),*map(str,sources),'-o',str(binary)],env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
  subprocess.run([str(binary)],env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
  log.write('Complete C674x ISA fixture passed ASan/UBSan.\n')
finally:core.write_bytes(saved)
