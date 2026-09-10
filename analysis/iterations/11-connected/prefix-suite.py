#!/usr/bin/env python3
"""Run three sequential, hash-pinned connected baseline/prefix pairs.

Existing completed reports are checked and skipped. Never run concurrently
with another benchmark, build, or regression suite.
"""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
OUT = Path(__file__).resolve().parent
VARIANTS = {
    'baseline': ('baseline', '12c761061fe4c190b6d1721129c985e6fbfe3b72f58a926372a3b6c739677e25'),
    'candidate': ('prefix-candidate', 'f2456de58913620499485fee640926ef05c1207e6bc334a1233a91ababb96535'),
}
QEMU = ROOT / 'build/qemu/build/qemu-system-sh4'
QEMU_HASH = 'ceda8c3f2aee29f41513db98110afc0c68bd35c008bccedac242bd51ee26fa04'

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def check(name, expected):
    report = json.loads((OUT / (name + '.json')).read_text())
    assert report['blackfin_install']['target']['sha256'] == expected
    assert report['input_hashes']['simulator']['sha256'] == expected
    assert report['input_hashes']['qemu']['sha256'] == QEMU_HASH
    assert report['result']['gui_exit'] == 0 and not report['result']['timed_out']
    manifest = json.loads((ROOT / 'runs' / name / 'run.json').read_text())
    assert not manifest['inputs_differ_at_exit']
    print('verified', name, expected, flush=True)

for pair in range(3):
    for label, (binary_name, expected) in VARIANTS.items():
        name = f'optimization-11-prefix-{label}-{pair}'
        binary = ROOT / 'build/performance/11' / binary_name
        assert digest(binary) == expected
        assert digest(QEMU) == QEMU_HASH
        if not (OUT / (name + '.json')).exists():
            print('launch', name, expected, flush=True)
            subprocess.run([sys.executable, str(OUT / 'benchmark.py'),
                            '--binary', str(binary), '--expected-binary-sha256', expected,
                            '--name', name, '--seconds', '85', '--port', '6180'],
                           cwd=ROOT, check=True)
        check(name, expected)
