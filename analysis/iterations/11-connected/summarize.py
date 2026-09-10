#!/usr/bin/env python3
"""Analyze the six exact, hash-pinned prefix trials; requires all complete."""
import json
from pathlib import Path
import re
import statistics as stats

ROOT = Path(__file__).resolve().parents[3]
OUT = Path(__file__).resolve().parent
HASHES = {'baseline': '12c761061fe4c190b6d1721129c985e6fbfe3b72f58a926372a3b6c739677e25',
          'candidate': 'f2456de58913620499485fee640926ef05c1207e6bc334a1233a91ababb96535'}
MILESTONES = {'normal': '1eab3e54d27d', 'utility': 'cf4226e6abcd', 'encoder': '59644051a335'}

def quantile(values, percent):
    return stats.quantiles(values, n=100, method='inclusive')[percent-1]

reports = []
for pair in range(3):
    for label in HASHES:
        name = f'optimization-11-prefix-{label}-{pair}'
        run = ROOT/'runs'/name
        summary = json.loads((OUT/(name+'.json')).read_text())
        manifest = json.loads((run/'run.json').read_text())
        assert summary['result']['gui_exit'] == 0 and not summary['result']['timed_out']
        assert not manifest['inputs_differ_at_exit']
        assert manifest['input_artifacts']['simulator']['sha256'] == HASHES[label]
        assert summary['blackfin_install']['target']['sha256'] == HASHES[label]
        inputs = {key: item['sha256'] for key, item in manifest['input_artifacts'].items()}
        if reports:
            assert {k:v for k,v in inputs.items() if k != 'simulator'} == {
                k:v for k,v in reports[0]['input_hashes'].items() if k != 'simulator'}
        observations = json.loads((run/'frames/manifest.json').read_text())['observations']
        milestones = {key: next(o for o in observations if o.get('sha256','').startswith(prefix))
                      for key,prefix in MILESTONES.items()}
        panel = json.loads((run/'panel-timeline.json').read_text())['events']
        commands = [e for e in panel if e['event'] == 'command']
        assert len(commands) == 3 and all(e['reply'].startswith('ok ') for e in commands)
        latency = {}
        for command, milestone in [('down 20:08', 'utility'), ('rotary 7 1', 'encoder')]:
            event = next(e for e in commands if e['command'] == command)
            publication = milestones[milestone]['source_mtime_ns']/1e9
            seconds = publication - (summary['started_epoch']+event['send_elapsed_seconds'])
            assert 0 < seconds < 15, (name, milestone, seconds)
            latency[milestone] = seconds
        launch = (run/'benchmark-launcher.log').read_text()
        pids = dict(zip(('main','gui'), map(int,re.search(r'MAIN (\d+), GUI (\d+)',launch).groups())))
        samples = [json.loads(line) for line in (run/'process-samples.jsonl').read_text().splitlines()]
        shared = [s for s in samples if set(pids.values()).issubset({r['pid'] for r in s['rows']})]
        window = [s for s in shared if 10 <= s['elapsed_seconds'] <= 80]
        assert len(window)>200
        def cpu(sample, pid):
            return next(r['cpu_time_seconds'] for r in sample['rows'] if r['pid']==pid)
        elapsed = window[-1]['elapsed_seconds']-window[0]['elapsed_seconds']
        cpu_percent = {role:100*(cpu(window[-1],pid)-cpu(window[0],pid))/elapsed for role,pid in pids.items()}
        cpu_percent['combined']=sum(cpu_percent.values())
        total_cpu = {role:cpu(shared[-1],pid) for role,pid in pids.items()}
        log=(run/'main-stderr.log').read_text()
        packets,cycles=map(int,re.findall(r'packets=(\d+) cycles=(\d+)',log)[-1])
        durations=[int(n)/1e6 for n in re.findall(r'nxs-dsp-host-time: execution-ns=(\d+)',log)]
        terminal=[s for s in (run/'gui.log').read_text().splitlines() if s.startswith('STATS wall=')][-1]
        counters=dict(re.findall(r'(\w+)=(\S+)',terminal))
        stops=re.findall(r' stop=(.*?) B15=',log)
        unexpected=sorted(set(stops)-{'phase budget exhausted','HINT host-event yield'})
        reports.append(dict(name=name,label=label,pair=pair,input_hashes=inputs,
            result=summary['result'],inputs_differ_at_exit=manifest['inputs_differ_at_exit'],
            normal_player_observed_gui_seconds=milestones['normal']['elapsed_seconds'],
            milestone_hashes={k:v['sha256'] for k,v in milestones.items()},
            response_seconds=latency,ack_latency_seconds={e['command']:e['ack_latency_seconds'] for e in commands},
            cpu_percent=cpu_percent,cpu_window_seconds=elapsed,cpu_seconds_last_shared=total_cpu,
            last_shared_elapsed_seconds=shared[-1]['elapsed_seconds'],
            dsp_packets=packets,dsp_cycles=cycles,dsp_packets_per_gui_budget_second=packets/85,
            callbacks=dict(count=len(durations),execution_seconds=sum(durations)/1000,
                           p50_ms=quantile(durations,50),p95_ms=quantile(durations,95),max_ms=max(durations)),
            final_stats=counters,stop_reasons=sorted(set(stops)),unexpected_stop_reasons=unexpected))

def describe(values):
    return dict(median=stats.median(values),min=min(values),max=max(values),values=values)
aggregate={}
for label in HASHES:
    rows=[r for r in reports if r['label']==label]
    aggregate[label]={
        'normal_player_observed_gui_seconds':describe([r['normal_player_observed_gui_seconds'] for r in rows]),
        'utility_response_seconds':describe([r['response_seconds']['utility'] for r in rows]),
        'encoder_response_seconds':describe([r['response_seconds']['encoder'] for r in rows]),
        'dsp_packets_per_gui_budget_second':describe([r['dsp_packets_per_gui_budget_second'] for r in rows]),
        'blackfin_instructions':describe([int(r['final_stats']['insns']) for r in rows]),
        **{role+'_cpu_percent':describe([r['cpu_percent'][role] for r in rows]) for role in ('main','gui','combined')}}
report=dict(schema=1,runs=reports,aggregate=aggregate,notes=[
    'Only the exact hash-pinned prefix suite contributes. Earlier formatter trials excluded.',
    'CPU percent: process CPU delta / host elapsed in the shared harness 10..80-second window; 100% is one core.',
    'Response: first identified frame host publication mtime minus panel send epoch. Assumes no host wall-clock step relative to monotonic during a run.',
    'Boot milestone: first normal-player observation, GUI-launch-relative, sampled every 0.2 seconds; observation scheduling adds jitter.',
    'DSP packet rate divides terminal packet progress by the matched 85-second GUI budget; MAIN launches about one second earlier in both variants.',
    'Blackfin instruction counts include idle-loop execution; higher counts do not alone establish faster useful firmware work.',
    'No full-state, cycle-accuracy, storage, or audio claim is made.'])
(OUT/'prefix-connected-summary.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(aggregate,indent=2))
