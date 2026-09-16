"""One hash-pinned connected trial; no builds or sampling profilers alongside it."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[3]
OUT = Path(__file__).resolve().parent
WORK = ROOT/'build/performance/15'

def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result

helpers = module('connected11', ROOT/'analysis/iterations/11-connected/benchmark.py')
analysis = module('analysis14', ROOT/'analysis/iterations/14-dsp-transactions/summarize.py')

def summarize_all():
    reference = json.loads((ROOT/'analysis/iterations/14-dsp-transactions/dsp-transactions-summary.json').read_text())
    expected = reference['runs'][0]['milestone_hashes']
    rows, common = [], None
    for path in sorted(OUT.glob('optimization-15-dsp-lookup-*.json')):
        row = analysis.one(json.loads(path.read_text()), expected, common)
        rows.append(row)
        if common is None:
            common = {k:v for k,v in row['input_hashes'].items() if k not in ('qemu','simulator')}
    assert len({r['input_hashes']['simulator'] for r in rows}) == 1
    aggregate = {}
    metrics = ('normal_player_observed_gui_seconds', 'dsp_packets_per_gui_budget_second')
    for label in ('baseline', 'candidate'):
        group = [r for r in rows if r['label'] == label]
        if not group:
            continue
        assert len({r['input_hashes']['qemu'] for r in group}) == 1
        aggregate[label] = {k:analysis.describe([r[k] for r in group]) for k in metrics}
        aggregate[label].update({k+'_response_seconds':analysis.describe([r['response_seconds'][k] for r in group]) for k in ('utility','encoder')})
        aggregate[label].update({k+'_cpu_percent':analysis.describe([r['cpu_percent'][k] for r in group]) for k in ('main','gui','combined')})
    (OUT/'connected-summary.json').write_text(json.dumps(dict(runs=rows, aggregate=aggregate), indent=2)+'\n')
    print(json.dumps(aggregate), flush=True)

def run(label, trial):
    qemu = WORK/label/'qemu-system-sh4'
    manifest_path = qemu.parent/'manifest.json'
    variant = json.loads(manifest_path.read_text())
    expected_simulator = json.loads((OUT/'build-inputs.json').read_text())['blackfin_sha256']
    before = helpers.artifact(qemu)
    sim_before = helpers.artifact(ROOT/'bin/cdj-run')
    manifest_before = helpers.artifact(manifest_path)
    assert before['sha256'] == variant['binary_sha256']
    assert sim_before['sha256'] == expected_simulator
    name = f'optimization-15-dsp-lookup-{label}-{trial}'
    run = ROOT/'runs'/name
    assert not run.exists(), run
    # The launcher owns run creation; temporary collectors live beside reports.
    log_path, samples, panel = (OUT/f'.{name}.{suffix}' for suffix in ('log','cpu.jsonl','panel.json'))
    command = [sys.executable, '-u', '-m', 'tools.cdj_main.nxs_vm', str(run),
               '--seconds', '85', '--frame-interval', '0.2', '--port', '6680', '--qemu', str(qemu)]
    started, epoch, stop = time.monotonic(), time.time(), threading.Event()
    with log_path.open('w') as log:
        process = subprocess.Popen(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
        threads = [threading.Thread(target=helpers.poll_processes, args=(process.pid, started, samples, stop)),
                   threading.Thread(target=helpers.panel_worker, args=(6684, started, 85, panel, stop))]
        for thread in threads: thread.start()
        try:
            rc = process.wait()
        finally:
            stop.set()
            for thread in threads: thread.join(timeout=5)
    for path, name_in_run in ((log_path,'benchmark-launcher.log'),(samples,'process-samples.jsonl'),(panel,'panel-timeline.json')):
        path.replace(run/name_in_run)
    after, sim_after = helpers.artifact(qemu), helpers.artifact(ROOT/'bin/cdj-run')
    identity = dict(qemu_before=before, qemu_after=after, simulator_before=sim_before, simulator_after=sim_after,
                    qemu_unchanged=before['sha256']==after['sha256'], simulator_unchanged=sim_before['sha256']==sim_after['sha256'],
                    qemu_manifest_before=manifest_before, qemu_manifest_after=helpers.artifact(manifest_path),
                    qemu_manifest_binary_sha256=variant['binary_sha256'], expected_simulator_sha256=expected_simulator)
    summary = helpers.summarize(run, None, command, epoch, run/'process-samples.jsonl', run/'panel-timeline.json', run/'benchmark-launcher.log')
    summary.update(label=label, trial=trial, input_identity=identity)
    (OUT/(name+'.json')).write_text(json.dumps(summary, indent=2)+'\n')
    assert rc == 0, rc
    summarize_all()

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--label', choices=('baseline','candidate'))
    parser.add_argument('--trial', type=int)
    parser.add_argument('--summarize', action='store_true')
    args = parser.parse_args()
    if args.summarize:
        summarize_all()
    else:
        assert args.label and args.trial is not None
        run(args.label,args.trial)
