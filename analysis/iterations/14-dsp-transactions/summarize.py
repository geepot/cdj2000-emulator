#!/usr/bin/env python3
"""Aggregate completed iteration 14 trials using iteration 11 semantics."""
from __future__ import annotations
import argparse, json, re, statistics
from pathlib import Path

HERE = Path(__file__).resolve().parent
LABELS = ("baseline", "clear", "copy", "combined")

def describe(values):
    return {"median": statistics.median(values), "min": min(values), "max": max(values), "values": values}

def milestones(observations, expected):
    result = {}
    for name, value in expected.items():
        prefix = value.get("sha256", value) if isinstance(value, dict) else value
        result[name] = next((o for o in observations if o.get("sha256", "").startswith(prefix)), None)
        if result[name] is None: raise ValueError(f"missing {name} milestone {prefix}")
    return result

def one(summary, expected, common):
    run = Path(summary["run"]); manifest = json.loads((run / "run.json").read_text())
    ident = summary.get("input_identity", {}); inputs = manifest.get("input_artifacts", {})
    if ident.get("qemu_before", {}).get("sha256") != inputs.get("qemu", {}).get("sha256") or ident.get("simulator_before", {}).get("sha256") != inputs.get("simulator", {}).get("sha256"):
        raise ValueError(f"{summary['name']}: input identity mismatch")
    if inputs.get("qemu", {}).get("sha256") != ident.get("qemu_manifest_binary_sha256") or inputs.get("simulator", {}).get("sha256") != ident.get("expected_simulator_sha256"):
        raise ValueError(f"{summary['name']}: unexpected variant binary")
    if not ident.get("qemu_unchanged") or not ident.get("simulator_unchanged"):
        raise ValueError(f"{summary['name']}: measured binaries changed")
    if ident['qemu_manifest_before']['sha256'] != ident['qemu_manifest_after']['sha256']:
        raise ValueError(f"{summary['name']}: variant manifest changed")
    if manifest.get("inputs_differ_at_exit"):
        raise ValueError(f"{summary['name']}: input artifacts changed at exit")
    comparable = {k: v.get("sha256") for k, v in inputs.items() if k not in ("qemu", "simulator")}
    if common is not None and comparable != common: raise ValueError(f"{summary['name']}: shared input hashes differ")
    result = summary.get("result") or manifest.get("result", {})
    if result.get("gui_exit") != 0 or result.get("timed_out"): raise ValueError(f"{summary['name']}: incomplete result")
    observations = json.loads((run / "frames/manifest.json").read_text())["observations"]
    marks = milestones(observations, expected)
    panel = json.loads((run / "panel-timeline.json").read_text())["events"]
    commands = [e for e in panel if e.get("event") == "command"]
    if len(commands) != 3 or any(not e.get("reply", "").startswith("ok ") for e in commands): raise ValueError(f"{summary['name']}: panel failure")
    response = {}
    for command, mark in (("down 20:08", "utility"), ("rotary 7 1", "encoder")):
        event = next(e for e in commands if e["command"] == command)
        value = marks[mark]["source_mtime_ns"] / 1e9 - (summary["started_epoch"] + event["send_elapsed_seconds"])
        if not 0 < value < 15: raise ValueError(f"{summary['name']}: invalid {mark} response {value}")
        response[mark] = value
    launch = (run / "benchmark-launcher.log").read_text(); match = re.search(r"MAIN (\d+), GUI (\d+)", launch)
    if not match: raise ValueError(f"{summary['name']}: missing process identities")
    pids = dict(zip(("main", "gui"), map(int, match.groups())))
    samples = [json.loads(x) for x in (run / "process-samples.jsonl").read_text().splitlines() if x.strip()]
    shared = [s for s in samples if set(pids.values()).issubset({r["pid"] for r in s["rows"]})]; window = [s for s in shared if 10 <= s["elapsed_seconds"] <= 80]
    if len(window) < 200: raise ValueError(f"{summary['name']}: insufficient CPU samples")
    def cpu(s, pid): return next(r["cpu_time_seconds"] for r in s["rows"] if r["pid"] == pid)
    elapsed = window[-1]["elapsed_seconds"] - window[0]["elapsed_seconds"]
    cpu_pct = {role: 100 * (cpu(window[-1], pid) - cpu(window[0], pid)) / elapsed for role, pid in pids.items()}; cpu_pct["combined"] = sum(cpu_pct.values())
    log = (run / "main-stderr.log").read_text(errors="replace"); packets = re.findall(r"packets=(\d+) cycles=(\d+)", log); durations = [int(x) / 1e6 for x in re.findall(r"nxs-dsp-host-time: execution-ns=(\d+)", log)]
    stats_lines = [x for x in (run / "gui.log").read_text(errors="replace").splitlines() if x.startswith("STATS wall=")]; final_stats = dict(re.findall(r"(\w+)=(\S+)", stats_lines[-1])) if stats_lines else {}
    stops = re.findall(r" stop=(.*?) B15=", log); unexpected = sorted(set(stops) - {"phase budget exhausted", "HINT host-event yield"})
    if unexpected: raise ValueError(f"{summary['name']}: unexpected stop reasons {unexpected}")
    callbacks = {"count": len(durations), "execution_seconds": sum(durations) / 1000, "p50_ms": statistics.median(durations), "p95_ms": statistics.quantiles(durations, n=100, method="inclusive")[94], "max_ms": max(durations)} if durations else None
    return {"name": summary["name"], "label": summary["label"], "trial": summary["trial"], "input_hashes": {k: v.get("sha256") for k, v in inputs.items()}, "normal_player_observed_gui_seconds": marks["normal"]["elapsed_seconds"], "milestone_hashes": {k: v["sha256"] for k, v in marks.items()}, "response_seconds": response, "ack_latency_seconds": {e["command"]: e["ack_latency_seconds"] for e in commands}, "cpu_percent": cpu_pct, "cpu_window_seconds": elapsed, "cpu_seconds_last_shared": {role: cpu(shared[-1], pid) for role, pid in pids.items()}, "dsp_packets": int(packets[-1][0]) if packets else None, "dsp_cycles": int(packets[-1][1]) if packets else None, "dsp_packets_per_gui_budget_second": int(packets[-1][0]) / 85 if packets else None, "callbacks": callbacks, "final_stats": final_stats, "stop_reasons": sorted(set(stops)), "unexpected_stop_reasons": unexpected}

def main():
    ap = argparse.ArgumentParser(description=__doc__); ap.add_argument("--summary", action="append", type=Path); ap.add_argument("--reference-summary", type=Path, required=True); ap.add_argument("--output", type=Path, default=HERE / "dsp-transactions-summary.json"); args = ap.parse_args()
    paths = args.summary or sorted(HERE.glob("optimization-14-dsp-transactions-*.json")); reference = json.loads(args.reference_summary.read_text()); expected = reference["runs"][0].get("milestone_hashes", reference.get("milestone_hashes", {}))
    common = None; rows = []
    for path in paths:
        row = one(json.loads(path.read_text()), expected, common); rows.append(row)
        if common is None: common = {k: v for k, v in row["input_hashes"].items() if k not in ("qemu", "simulator")}
    if len({r['input_hashes']['simulator'] for r in rows}) != 1:
        raise ValueError('simulator changed across trials')
    for label in LABELS:
        if len({r['input_hashes']['qemu'] for r in rows if r['label']==label}) > 1:
            raise ValueError(f'{label}: binary changed across trials')
    aggregate = {}
    for label in LABELS:
        group = [r for r in rows if r["label"] == label]
        if group: aggregate[label] = {"normal_player_observed_gui_seconds": describe([r["normal_player_observed_gui_seconds"] for r in group]), "utility_response_seconds": describe([r["response_seconds"]["utility"] for r in group]), "encoder_response_seconds": describe([r["response_seconds"]["encoder"] for r in group]), "dsp_packets_per_gui_budget_second": describe([r["dsp_packets_per_gui_budget_second"] for r in group]), **{role + "_cpu_percent": describe([r["cpu_percent"][role] for r in group]) for role in ("main", "gui", "combined")}}
    args.output.write_text(json.dumps({"schema": 1, "suite": "14-dsp-transactions", "runs": rows, "aggregate": aggregate, "milestone_hashes": expected, "notes": ["Response seconds use source_mtime_ns minus panel send epoch, as in iteration 11.", "CPU uses shared process samples from 10..80 seconds; 100% is one core."]}, indent=2) + "\n"); print(json.dumps(aggregate, indent=2))
if __name__ == "__main__": main()
