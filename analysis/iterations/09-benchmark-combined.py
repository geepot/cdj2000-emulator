"""Alternating end-to-end comparison; run from repo root after tests finish.

Prepare build/performance/baseline with `git archive c16d1cb`. Pin the same
compiler environment for all runs. Proprietary inputs and outputs stay ignored.
"""
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys
import time

root = Path.cwd()
output = root / 'build/performance/09-combined'
output.mkdir(exist_ok=False)
checkpoint = root / 'runs/dsp-post-interrupt-pipedown-5m-replay-2/final.cdjdsp'
formats = root / 'build/gdb-17.2/include/opcode/tic6x-insn-formats.h'
env = os.environ.copy()
env.pop('PYTHONPATH', None)
env['CDJ_REPLAY_CACHE'] = str(output / 'cache')
rows = []


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


for trial in range(5):
    for label in (('baseline', 'final') if trial % 2 == 0 else ('final', 'baseline')):
        directory = output / f'{label}-{trial}'
        cwd = root / 'build/performance/baseline' if label == 'baseline' else root
        command = [sys.executable, '-m', 'tools.cdj_dsp.replay', str(checkpoint),
                   str(directory), '--formats', str(formats), '--steps', '300000',
                   '--functional-dsp-timing', '--functional-dsp-audio', '--verify-repeat']
        if label == 'final':
            command += ['--trace-mode', 'compact']
        started = time.perf_counter()
        result = subprocess.run(command, cwd=cwd, env=env, capture_output=True)
        elapsed = time.perf_counter() - started
        (output / f'{label}-{trial}.stderr').write_bytes(result.stderr)
        if result.returncode:
            raise RuntimeError(result.stderr.decode(errors='replace'))
        gate = json.loads((directory / 'gate.json').read_text())
        assert gate['passed'], gate
        coverage = json.loads((directory / 'coverage.json').read_text())
        coverage.pop('sha256')
        manifest = json.loads((directory / 'manifest.json').read_text())
        rows.append(dict(label=label, trial=trial, seconds=elapsed, gate_passed=True,
                         cache_hit=manifest.get('build', {}).get('cache_hit'),
                         checkpoint_sha256=digest(directory / 'final.cdjdsp'),
                         trace_sha256=digest(directory / 'trace.jsonl'),
                         trace_bytes=(directory / 'trace.jsonl').stat().st_size,
                         semantic_coverage_sha256=hashlib.sha256(json.dumps(
                             coverage, sort_keys=True).encode()).hexdigest()))
        print(label, trial, round(elapsed, 3), flush=True)

assert len({r['checkpoint_sha256'] for r in rows}) == 1
assert len({r['semantic_coverage_sha256'] for r in rows}) == 1
for label in ('baseline', 'final'):
    assert len({r['trace_sha256'] for r in rows if r['label'] == label}) == 1
warm = {label: statistics.median(r['seconds'] for r in rows
                                if r['label'] == label and r['trial'] > 0)
        for label in ('baseline', 'final')}
report = dict(baseline_commit='c16d1cb', base_of_candidate_commit=subprocess.check_output(
    ['git', 'rev-parse', 'HEAD'], text=True).strip(),
    candidate_replay_sha256=digest(root / 'tools/cdj_dsp/replay.c'),
    workload='300000 exploratory steps, repeat, coverage and gates',
    baseline_trace_mode='detailed', final_trace_mode='compact',
    compiler=subprocess.check_output(['cc', '--version'], text=True),
    developer_dir=env.get('DEVELOPER_DIR'), checkpoint_sha256=digest(checkpoint),
    formats_sha256=digest(formats), trials=rows, warm_median_seconds=warm,
    warm_speedup=warm['baseline'] / warm['final'],
    warm_time_reduction_percent=100 * (1 - warm['final'] / warm['baseline']))
Path('analysis/iterations/09-combined.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(warm), report['warm_speedup'])
