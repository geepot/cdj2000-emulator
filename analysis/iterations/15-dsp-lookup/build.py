"""Build a variant using frozen link inputs and the production compiler flags."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[3]
WORK = ROOT/'build/performance/15'
def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

parser = argparse.ArgumentParser()
parser.add_argument('label', choices=('baseline', 'candidate'))
args = parser.parse_args()
tree = WORK/('base' if args.label == 'baseline' else 'candidate-src')
source = tree/'emulator/qemu/cdj_c674x.c'
out = WORK/args.label
out.mkdir(exist_ok=True)
recipe = json.loads((WORK/'build-recipe.json').read_text())
assert all(sha(WORK/'link-inputs'/r['file']) == r['sha256'] for r in recipe['objects'])
command = recipe['compile'].copy()
for flag, value in (('-o', str(out/'core.o')), ('-MF', str(out/'core.d')), ('-MQ', str(out/'core.o'))):
    if flag in command:
        command[command.index(flag)+1] = value
command[command.index('-c')+1] = str(source)
link = recipe['link'].copy()
link[link.index('-o')+1] = str(out/'qemu-unsigned')
positions = [i for i, s in enumerate(link) if s.endswith('/hw_sh4_cdj_c674x.c.o')]
assert len(positions) == 1
link[positions[0]] = str(out/'core.o')
env = os.environ.copy()
env['DEVELOPER_DIR'] = '/Library/Developer/CommandLineTools'
with (out/'build.log').open('w') as log:
    for cmd in (command, link):
        subprocess.run(cmd, cwd=recipe['build'], env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
    subprocess.run(['sh', str(ROOT/'build/qemu/scripts/entitlement.sh'),
                    str(out/'qemu-system-sh4'), str(out/'qemu-unsigned'),
                    str(ROOT/'build/qemu/pc-bios/qemu.rsrc')],
                   env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
    subprocess.run([str(out/'qemu-system-sh4'), '-version'], stdout=log, stderr=subprocess.STDOUT, check=True)
manifest = dict(label=args.label, source_sha256=sha(source), binary_sha256=sha(out/'qemu-system-sh4'), compile=command)
(out/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
print(json.dumps(manifest))
