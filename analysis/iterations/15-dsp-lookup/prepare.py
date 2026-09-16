"""Freeze committed sources and current QEMU link inputs without changing either."""
import hashlib
import io
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[3]
OUT = Path(__file__).resolve().parent
WORK = ROOT / 'build/performance/15'
BUILD = ROOT / 'build/qemu/build'

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

WORK.mkdir(parents=True, exist_ok=False)
base = WORK / 'base'
base.mkdir()
archive = subprocess.check_output(['git', 'archive', 'HEAD'], cwd=ROOT)
with tarfile.open(fileobj=io.BytesIO(archive)) as source:
    source.extractall(base, filter='data')
shutil.copytree(base, WORK / 'candidate-src')
lines = subprocess.check_output(['ninja', '-t', 'commands', 'qemu-system-sh4'],
                                cwd=BUILD, text=True).splitlines()
commands = [s for s in lines if ' -o qemu-system-sh4-unsigned ' in s]
assert len(commands) == 1
link = shlex.split(commands[0])
records = []
for i, arg in enumerate(link):
    source = BUILD / arg
    if not arg.startswith('-') and not Path(arg).is_absolute() and source.is_file():
        target = WORK / 'link-inputs' / arg
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        records.append(dict(file=arg, sha256=sha(target)))
        link[i] = str(target)
database = json.loads((BUILD / 'compile_commands.json').read_text())
core = [x for x in database if x['file'].endswith('/cdj_c674x.c')]
assert len(core) == 1
recipe = dict(build=str(BUILD), link=link, compile=shlex.split(core[0]['command']), objects=records)
(WORK / 'build-recipe.json').write_text(json.dumps(recipe, indent=2) + '\n')
shutil.copy2(BUILD / 'qemu-system-sh4', WORK / 'installed-baseline')
provenance = dict(head=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                  installed_qemu_sha256=sha(WORK/'installed-baseline'),
                  blackfin_sha256=sha(ROOT/'bin/cdj-run'), objects=records,
                  baseline_core_sha256=sha(base/'emulator/qemu/cdj_c674x.c'),
                  prepared_core_sha256=sha(ROOT/'build/qemu/hw/sh4/cdj_c674x.c'),
                  dirty_status=subprocess.check_output(['git', 'status', '--short'], cwd=ROOT, text=True))
(OUT / 'build-inputs.json').write_text(json.dumps(provenance, indent=2) + '\n')
for tree in (base, WORK/'candidate-src'):
    (tree/'build').mkdir(exist_ok=True)
    (tree/'build/gdb-17.2').symlink_to(ROOT/'build/gdb-17.2')
print('Frozen source and', len(records), 'link inputs at', WORK)
