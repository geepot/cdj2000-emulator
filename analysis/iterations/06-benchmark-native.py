"""Run from the repository root; pre-guard is a git archive of 4ae1778.

Compile both versions identically, then isolate native execution from Python
build/coverage overhead. Keep outputs under ignored build/performance.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import sys

sys.path.insert(0, str(Path.cwd()))
from tools.cdj_dsp.replay import SOURCES, ROOT

output = Path('build/performance/06-native')
output.mkdir(exist_ok=False)
rows = []
checkpoint = Path('runs/dsp-post-interrupt-pipedown-5m-replay-2/final.cdjdsp')
env = {k: v for k, v in os.environ.items() if not k.startswith('CDJ_')}
env.update(CDJ_NXS_DSP_FUNCTIONAL_TIMING='1', CDJ_NXS_DSP_FUNCTIONAL_AUDIO='1',
           CDJ_DSP_COMPACT_TRACE='1')
for label, root in [('before', Path('build/performance/pre-guard')), ('after', ROOT)]:
    subprocess.run(['cc', '-O2', '-std=c11', '-I', str(root / 'emulator/qemu'),
                    *[str(root / p.relative_to(ROOT)) for p in SOURCES],
                    '-o', str(output / label)], env=env, check=True)
for trial in range(3):
    for label in (('before', 'after') if trial % 2 == 0 else ('after', 'before')):
        binary = output / label
        trace, final = output / f'{label}-{trial}.jsonl', output / f'{label}-{trial}.cdjdsp'
        started = time.perf_counter()
        with trace.open('wb') as stream:
            subprocess.run([str(binary), str(checkpoint), '1000000', '0', '0', '0', '7',
                            str(final)], env=env, stdout=stream, check=True)
        elapsed = time.perf_counter() - started
        data = trace.read_bytes()
        stop = next(e for e in map(json.loads, data.splitlines()) if e.get('event') == 'stop')
        assert not stop['fault'], stop
        rows.append(dict(label=label, trial=trial, seconds=elapsed,
                         binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                         trace_sha256=hashlib.sha256(data).hexdigest(),
                         checkpoint_sha256=hashlib.sha256(final.read_bytes()).hexdigest(),
                         stop=stop))
report = dict(steps=1000000, input_sha256=hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
              trials=rows)
Path('analysis/iterations/06-native-guard.json').write_text(json.dumps(report, indent=2) + '\n')
print([(r['label'], r['seconds']) for r in rows])
