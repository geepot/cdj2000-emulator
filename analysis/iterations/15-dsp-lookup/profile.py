"""Two host stack samples during a bounded strict connected baseline run."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[3]
OUT = Path(__file__).resolve().parent
QEMU = ROOT/'build/performance/15/baseline/qemu-system-sh4'
RUN = ROOT/'runs/optimization-15-lookup-profile'
os.chdir(ROOT)
with (OUT/'profile-launch.txt').open('w') as log:
    process = subprocess.Popen([sys.executable, '-u', '-m', 'tools.cdj_main.nxs_vm',
                                str(RUN), '--seconds', '35', '--frame-interval', '0.2',
                                '--port', '6580', '--qemu', str(QEMU)], stdout=log, stderr=subprocess.STDOUT)
    for _ in range(150):
        match = re.search(r'MAIN (\d+), GUI (\d+)', (OUT/'profile-launch.txt').read_text())
        if match or process.poll() is not None:
            break
        time.sleep(.2)
    if not match:
        raise RuntimeError('Emulator did not start')
    for i in range(2):
        time.sleep(7)
        subprocess.run(['/usr/bin/sample', match[1], '5', '1', '-file', str(OUT/f'sh4-{i}.sample.txt')],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    result = process.wait()
assert result == 0
manifest = json.loads((RUN/'run.json').read_text())
assert not manifest['inputs_differ_at_exit']
print('Profile completed:', RUN)
