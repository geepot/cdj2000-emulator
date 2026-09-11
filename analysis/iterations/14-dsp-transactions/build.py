"""Link isolated QEMU variants from frozen objects and identical compiler flags.

prepare link-inputs/build-recipe.json before use; never write the shared build.
"""
import argparse,hashlib,json,os,shlex,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
WORK=ROOT/'build/performance/14'
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
p=argparse.ArgumentParser();p.add_argument('label');p.add_argument('source',type=Path);a=p.parse_args()
recipe=json.loads((WORK/'build-recipe.json').read_text());src=a.source.resolve()
out=WORK/a.label;out.mkdir(exist_ok=True)
assert all(sha(WORK/'link-inputs'/r['file'])==r['sha256'] for r in recipe['objects'])
args=recipe['compile'].copy(); obj=out/'core.o'
args[1:1]=['-I',str(ROOT/'build/performance/14-baseline/emulator/qemu')]
for flag,value in [('-o',str(obj)),('-MF',str(out/'core.d')),('-MQ',str(obj))]:
 if flag in args:args[args.index(flag)+1]=value
args[args.index('-c')+1]=str(src)
# Local quoted includes resolve beside source; flags otherwise exactly match.
link=recipe['link'].copy();link[link.index('-o')+1]=str(out/'qemu-unsigned')
old=[i for i,v in enumerate(link) if v.endswith('/hw_sh4_cdj_c674x.c.o')];assert len(old)==1;link[old[0]]=str(obj)
env=os.environ.copy();env['DEVELOPER_DIR']='/Library/Developer/CommandLineTools'
with (out/'build.log').open('w') as log:
 for cmd in [args,link]:
  log.write(shlex.join(cmd)+'\n');log.flush();subprocess.run(cmd,cwd=recipe['build'],env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
 # Match normal packaging (atomic rename), avoid any in-place executable write.
 cmd=['sh',str(ROOT/'build/qemu/scripts/entitlement.sh'),str(out/'qemu-system-sh4'),str(out/'qemu-unsigned'),str(ROOT/'build/qemu/pc-bios/qemu.rsrc')]
 subprocess.run(cmd,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
 subprocess.run([str(out/'qemu-system-sh4'),'-version'],env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
meta={'label':a.label,'source':str(src),'source_sha256':sha(src),'binary_sha256':sha(out/'qemu-system-sh4'),'compile':args,'link':link}
(out/'manifest.json').write_text(json.dumps(meta,indent=2)+'\n');print(a.label,meta['binary_sha256'])
