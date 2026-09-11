#!/usr/bin/env python3
"""Run hash-pinned connected trials for the iteration 14 QEMU variants.

This harness never installs or replaces ``bin/cdj-run``.  Each run records the
QEMU and simulator artifacts before and after launch, while reusing the stable
panel and process collectors from iteration 11.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
RUNS = ROOT / "runs"
QEMU_ROOT = ROOT / "build/performance/14"
SIMULATOR = ROOT / "bin/cdj-run"

spec = importlib.util.spec_from_file_location("connected11", ROOT / "analysis/iterations/11-connected/benchmark.py")
connected = importlib.util.module_from_spec(spec)
assert spec.loader
spec.loader.exec_module(connected)


def run_one(label: str, trial: int, seconds: float = 85, port: int = 6480) -> dict:
    qemu = (QEMU_ROOT / label / "qemu-system-sh4").resolve()
    if not qemu.is_file():
        raise FileNotFoundError(qemu)
    if not SIMULATOR.is_file():
        raise FileNotFoundError(SIMULATOR)
    qemu_manifest_path = qemu.parent / "manifest.json"
    qemu_manifest = json.loads(qemu_manifest_path.read_text())
    qemu_manifest_before = connected.artifact(qemu_manifest_path)
    qemu_before = connected.artifact(qemu)
    if qemu_manifest.get("binary_sha256") != qemu_before["sha256"]:
        raise RuntimeError(f"{label}: qemu manifest binary_sha256 does not match executable")
    input_candidates = (qemu.parent / "build-inputs.json", QEMU_ROOT / "build-inputs.json",
                        ROOT / "build-inputs.json")
    input_path = next((path for path in input_candidates if path.is_file()), None)
    if input_path is None:
        raise FileNotFoundError("build-inputs.json (expected blackfin_sha256)")
    build_inputs = json.loads(input_path.read_text())
    expected_simulator = build_inputs.get("blackfin_sha256")
    if not expected_simulator:
        raise RuntimeError(f"{input_path}: missing blackfin_sha256")
    simulator_before = connected.artifact(SIMULATOR)
    if simulator_before["sha256"] != expected_simulator:
        raise RuntimeError("bin/cdj-run does not match build-inputs.json blackfin_sha256")
    run_name = f"optimization-14-dsp-transactions-{label}-{trial}"
    run = (RUNS / run_name).resolve()
    if run.exists():
        raise FileExistsError(run)
    started = time.monotonic()
    epoch = time.time()
    command = [sys.executable, "-m", "tools.cdj_main.nxs_vm", str(run.relative_to(ROOT)),
               "--seconds", str(seconds), "--frame-interval", "0.2",
               "--port", str(port), "--qemu", str(qemu)]
    stem = HERE / f".{run_name}"
    launcher = stem.with_suffix(".launcher.log")
    samples = stem.with_suffix(".process-samples.jsonl")
    panel = stem.with_suffix(".panel-timeline.json")
    stop = threading.Event()
    with launcher.open("w", encoding="utf-8") as log:
        process = subprocess.Popen(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
        sampler = threading.Thread(target=connected.poll_processes,
                                    args=(process.pid, started, samples, stop), daemon=True)
        panel_thread = threading.Thread(target=connected.panel_worker,
                                        args=(port + 4, started, seconds, panel, stop), daemon=True)
        sampler.start(); panel_thread.start()
        try:
            process.wait()
        finally:
            stop.set(); sampler.join(timeout=2); panel_thread.join(timeout=2)
    run.mkdir(parents=True, exist_ok=True)
    destinations = [(launcher, run / "benchmark-launcher.log"),
                    (samples, run / "process-samples.jsonl"),
                    (panel, run / "panel-timeline.json")]
    for source, destination in destinations:
        if source.exists():
            os.replace(source, destination)
    qemu_after = connected.artifact(qemu)
    simulator_after = connected.artifact(SIMULATOR)
    qemu_manifest_after = connected.artifact(qemu_manifest_path)
    identity = {"label": label, "trial": trial, "command": command,
                "seconds": seconds, "main_port": port, "panel_port": port + 4,
                "qemu_manifest": str(qemu_manifest_path), "qemu_manifest_binary_sha256": qemu_manifest["binary_sha256"],
                "qemu_manifest_before": qemu_manifest_before, "qemu_manifest_after": qemu_manifest_after,
                "build_inputs": str(input_path), "expected_simulator_sha256": expected_simulator,
                "qemu_before": qemu_before, "qemu_after": qemu_after,
                "simulator_before": simulator_before, "simulator_after": simulator_after,
                "qemu_unchanged": qemu_before["sha256"] == qemu_after["sha256"],
                "simulator_unchanged": simulator_before["sha256"] == simulator_after["sha256"]}
    (run / "measurement-inputs.json").write_text(json.dumps(identity, indent=2) + "\n")
    summary = connected.summarize(run, None, command, epoch, run / "process-samples.jsonl",
                                  run / "panel-timeline.json", run / "benchmark-launcher.log")
    summary.update({"label": label, "trial": trial, "measurement_inputs": "measurement-inputs.json",
                    "input_identity": identity})
    output = HERE / f"{run_name}.json"
    output.write_text(json.dumps(summary, indent=2) + "\n")
    return {"run": str(run), "summary": str(output), "exit_code": process.returncode}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--label", choices=("baseline", "clear", "copy", "combined"))
    parser.add_argument("--trial", type=int)
    parser.add_argument("--suite", action="store_true", help="run all four labels for --trials")
    parser.add_argument("--trials", type=int, default=2)
    parser.add_argument("--seconds", type=float, default=85)
    parser.add_argument("--port", type=int, default=6480)
    args = parser.parse_args()
    if args.seconds <= 0 or args.trials <= 0 or not 1024 <= args.port <= 65531:
        parser.error("positive seconds/trials and port 1024..65531 required")
    if args.suite:
        jobs = [(label, trial) for trial in range(args.trials)
                for label in (("baseline", "clear", "copy", "combined") if trial % 2 == 0
                              else ("combined", "copy", "clear", "baseline"))]
    elif args.label is not None and args.trial is not None:
        if args.trial < 0:
            parser.error("trial must be nonnegative")
        jobs = [(args.label, args.trial)]
    else:
        parser.error("provide --label and --trial, or --suite")
    results = []
    for label, trial in jobs:
        print(f"launch {label} trial {trial}", flush=True)
        result = run_one(label, trial, args.seconds, args.port)
        results.append(result)
        if result['exit_code']:
            raise RuntimeError(result)
        subprocess.run([sys.executable, str(HERE/'summarize.py'), '--reference-summary',
                        str(ROOT/'analysis/iterations/11-connected/prefix-connected-summary.json')],
                       check=True, stdout=subprocess.DEVNULL)
        print(f"verified {label} trial {trial}", flush=True)
    print(json.dumps(results, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
