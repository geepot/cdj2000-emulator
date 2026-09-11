"""Bounded diagnostic of installed binaries; no runtime/source modification."""
import hashlib,json,os,re,shutil,subprocess,time
from pathlib import Path
root=Path(__file__).resolve().parents[3]
os.chdir(root)
out=Path('analysis/iterations/13-dsp-copy'); run=Path('runs/optimization-13-copy-profile')
snap=Path('build/performance/13'); snap.mkdir(parents=True,exist_ok=True)
def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
q=Path('build/qemu/build/qemu-system-sh4'); saved=snap/'qemu-system-sh4'; shutil.copy2(q,saved)
meta={'head':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),'qemu_sha256':sha(saved),'blackfin_before':sha('bin/cdj-run'),'sample_scope':'three 5-second 1ms stack samples, observer overhead; no candidate'}
(out/'provenance.json').write_text(json.dumps(meta,indent=2)+'\n')
with (out/'launch.txt').open('w') as log:
 p=subprocess.Popen(['.venv/bin/python','-u','-m','tools.cdj_main.nxs_vm',str(run),'--seconds','70','--frame-interval','0.2','--port','6380','--qemu',str(saved.resolve())],stdout=log,stderr=subprocess.STDOUT)
 for i in range(100):
  match=re.search(r'MAIN (\d+), GUI (\d+)',(out/'launch.txt').read_text())
  if match or p.poll() is not None: break
  time.sleep(.2)
 if match:
  for i in range(3):
   time.sleep(8)
   subprocess.run(['/usr/bin/sample',match[1],'5','1','-file',str(out/f'sh4-{i}.sample.txt')],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,check=True)
 meta['exit']=p.wait()
meta['blackfin_after']=sha('bin/cdj-run');meta['saved_qemu_after']=sha(saved)
(out/'provenance.json').write_text(json.dumps(meta,indent=2)+'\n')
print(json.dumps(meta))
