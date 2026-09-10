#!/usr/bin/env python3
"""Run and summarize a connected NXS performance benchmark.

The harness deliberately leaves emulation policy in ``tools.cdj_main.nxs_vm``.
It installs one selected Blackfin executable atomically, records its hash, then
keeps the host process, panel, frame, and DSP timelines in the same run.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from tools.cdj_main.panel_control import PanelControl, resolve

SUMMARY_DIR = Path(__file__).resolve().parent
DEFAULT_QEMU = ROOT / "build/qemu/build/qemu-system-sh4"
CPU_POLL_SECONDS = 0.2


def cpu_time_seconds(value: str) -> float | None:
    """Parse macOS/Linux ps cputime (MM:SS, HH:MM:SS, or DD-HH:MM:SS)."""
    try:
        days, separator, clock = value.partition("-")
        if not separator:
            days, clock = "", value
        fields = [float(part) for part in clock.split(":")]
        if len(fields) == 2:
            seconds = fields[0] * 60 + fields[1]
        elif len(fields) == 3:
            seconds = fields[0] * 3600 + fields[1] * 60 + fields[2]
        else:
            return None
        return seconds + (float(days) * 86400 if days else 0)
    except ValueError:
        return None


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def artifact(path: Path) -> dict:
    stat = path.stat()
    return {"path": str(path.resolve()), "size": stat.st_size,
            "sha256": sha256(path), "mtime_ns": stat.st_mtime_ns}


def install_blackfin(source: Path, target: Path) -> dict:
    """Replace target with source using same-directory rename semantics."""
    source = source.resolve()
    target = target.resolve()
    if not source.is_file():
        raise FileNotFoundError(source)
    before = artifact(target) if target.exists() else None
    selected = artifact(source)
    target.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=f".{target.name}.", dir=target.parent)
    os.close(fd)
    temporary_path = Path(temporary)
    try:
        shutil.copyfile(source, temporary_path)
        os.chmod(temporary_path, source.stat().st_mode & 0o777)
        if sha256(temporary_path) != selected["sha256"]:
            raise RuntimeError("temporary Blackfin install failed hash check")
        os.replace(temporary_path, target)
    finally:
        temporary_path.unlink(missing_ok=True)
    installed = artifact(target)
    return {"source": selected, "target": installed, "target_before": before,
            "atomic_replace": True}


def descendants(root_pid: int) -> list[int]:
    """Return root and descendants using portable ``ps`` output."""
    result = subprocess.run(["ps", "-axo", "pid=,ppid="], text=True,
                            capture_output=True, check=False)
    children: dict[int, list[int]] = {}
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) == 2 and all(field.isdigit() for field in fields):
            children.setdefault(int(fields[1]), []).append(int(fields[0]))
    found = [root_pid]
    for pid in found:
        found.extend(children.get(pid, []))
    return sorted(set(found))


def poll_processes(pid: int, started: float, output: Path,
                   stop: threading.Event) -> None:
    """Sample all nxs_vm descendants with host ``ps`` until teardown."""
    with output.open("w", encoding="utf-8") as stream:
        while not stop.is_set():
            now = time.monotonic()
            pids = descendants(pid)
            result = subprocess.run(
                ["ps", "-p", ",".join(map(str, pids)), "-o",
                 "pid=,ppid=,comm=,cputime=,rss=,state="],
                text=True, capture_output=True, check=False)
            rows = []
            for line in result.stdout.splitlines():
                fields = line.split(None, 5)
                if len(fields) >= 6 and fields[0].isdigit():
                    rows.append({"pid": int(fields[0]), "ppid": int(fields[1]),
                                 "command": fields[2], "cpu_time": fields[3],
                                 "cpu_time_seconds": cpu_time_seconds(fields[3]),
                                 "rss_kb": int(fields[4]) if fields[4].isdigit() else None,
                                 "state": fields[5]})
            stream.write(json.dumps({"elapsed_seconds": now - started,
                                     "rows": rows}) + "\n")
            stream.flush()
            stop.wait(CPU_POLL_SECONDS)


def panel_worker(port: int, started: float, seconds: float, output: Path,
                 stop: threading.Event) -> None:
    """Drive the fixed interaction while recording send and acknowledgement times."""
    schedule = [(45.0, "down 20:08"), (57.0, "up 20:08"),
                (68.0, "rotary 7 1")]
    records = []
    session = None
    try:
        deadline = max(30.0, min(seconds, 45.0))
        while not stop.is_set():
            try:
                session = PanelControl("127.0.0.1", port, timeout=3)
                greeting = session.open_when_ready(deadline)
                records.append({"event": "connected", "elapsed_seconds": time.monotonic() - started,
                                "reply": greeting})
                break
            except (OSError, ConnectionError):
                if session:
                    session.close()
                session = None
                time.sleep(0.5)
        if session is None:
            records.append({"event": "unavailable", "elapsed_seconds": time.monotonic() - started})
        else:
            for when, command in schedule:
                if when >= seconds:
                    continue
                wait = started + when - time.monotonic()
                if wait > 0:
                    stop.wait(wait)
                if stop.is_set():
                    break
                sent = time.monotonic()
                try:
                    reply = session.send_resilient(resolve(command))
                    acknowledged = time.monotonic()
                    records.append({"event": "command", "scheduled_elapsed_seconds": when,
                                    "send_elapsed_seconds": sent - started,
                                    "ack_elapsed_seconds": acknowledged - started,
                                    "command": command, "wire": resolve(command),
                                    "reply": reply,
                                    "ack_latency_seconds": acknowledged - sent})
                except (OSError, ConnectionError) as error:
                    records.append({"event": "command_error", "scheduled_elapsed_seconds": when,
                                    "send_elapsed_seconds": sent - started,
                                    "command": command, "error": str(error)})
                    break
    finally:
        if session:
            session.close()
        output.write_text(json.dumps({"schema": 1, "events": records}, indent=2) + "\n")


def summarize(run: Path, install: dict, command: list[str], started_epoch: float,
              process_samples: Path, panel_log: Path, launcher_log: Path) -> dict:
    def artifact_name(path: Path) -> str:
        try:
            return str(path.relative_to(run))
        except ValueError:
            return str(path)

    run_manifest = json.loads((run / "run.json").read_text()) if (run / "run.json").exists() else {}
    result = json.loads((run / "result.json").read_text()) if (run / "result.json").exists() else {}
    frames = run / "frames/manifest.json"
    frame_manifest = json.loads(frames.read_text()) if frames.exists() else None
    event_path = run / "dsp-events.jsonl"
    event_count = 0
    dsp_progress = []
    if event_path.exists():
        for line in event_path.open(encoding="utf-8"):
            if not line.strip():
                continue
            event_count += 1
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                continue
            if "packets" in event or "cycles" in event:
                dsp_progress.append({key: event[key] for key in ("sequence", "packets", "cycles")
                                     if key in event})
    stats = []
    callback_durations = []
    for path in (run / "gui.log", run / "main-stderr.log"):
        if path.exists():
            for line in path.read_text(errors="replace").splitlines():
                if "STATS wall=" in line:
                    stats.append(line[line.index("STATS "):].strip())
                match = re.search(r"packets=(\d+) cycles=(\d+)", line)
                if match:
                    dsp_progress.append({"packets": int(match.group(1)),
                                         "cycles": int(match.group(2)), "source": path.name})
                duration = re.search(r"nxs-dsp-host-time: execution-ns=(\d+)", line)
                if duration:
                    callback_durations.append(int(duration.group(1)) / 1e6)
    inputs = run_manifest.get("input_artifacts", {})
    process_last = None
    if process_samples.exists():
        for line in process_samples.read_text(errors="replace").splitlines():
            try:
                process_last = json.loads(line)
            except json.JSONDecodeError:
                pass
    run_result = run_manifest.get("result", result)
    if not run_result and launcher_log.exists():
        for line in reversed(launcher_log.read_text(errors="replace").splitlines()):
            try:
                candidate = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(candidate, dict) and ("gui_exit" in candidate or "timed_out" in candidate):
                run_result = candidate
                break
    if frame_manifest:
        seen = set()
        unique = []
        for observation in frame_manifest["observations"]:
            digest = observation.get("sha256")
            if digest and digest not in seen:
                seen.add(digest)
                unique.append(observation)
        frame_manifest = {**frame_manifest, "observations": unique,
                          "summary_scope": "unique frames; full timeline retained in run directory"}
    return {"schema": 1, "name": run.name, "run": str(run),
            "started_epoch": started_epoch, "command": command,
            "blackfin_install": install, "input_hashes": inputs,
            "result": run_result, "frame_manifest": frame_manifest,
            "process_last_sample": process_last,
            "dsp_event_records": event_count,
            "dsp_progress": dsp_progress[-1:],
            "dsp_progress_count": len(dsp_progress),
            "last_stats": stats[-1] if stats else None,
            "callback_durations_ms": callback_durations,
            "artifacts": {"run_json": "run.json", "result_json": "result.json",
                           "process_samples": artifact_name(process_samples),
                           "panel_timeline": artifact_name(panel_log),
                           "launcher_log": artifact_name(launcher_log)},
            "timeline_notes": [
                "Panel send and acknowledgement clocks are recorded separately from scheduled time.",
                "Frame observations are samples; changed frames are not attributed to commands here.",
                "Frame elapsed times use GUI-launch monotonic time; panel times use harness-start monotonic time.",
                "Use source_mtime_ns against started_epoch plus panel elapsed for host-clock correlation.",
                "Inspect raw logs and manifests before deriving firmware response latency."]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, epilog=(
        "Example: python analysis/iterations/11-connected/benchmark.py "
        "--binary build/performance/11/baseline --expected-binary-sha256 SHA256 --name baseline-1 "
        "--seconds 85 --port 5980"))
    parser.add_argument("--binary", type=Path, required=True,
                        help="saved Blackfin executable to install as bin/cdj-run")
    parser.add_argument("--expected-binary-sha256", required=True,
                        help="expected experiment hash, checked before installation")
    parser.add_argument("--name", required=True, help="run name under runs/")
    parser.add_argument("--seconds", type=float, default=85)
    parser.add_argument("--port", type=int, default=5980)
    parser.add_argument("--qemu", type=Path, default=DEFAULT_QEMU)
    args = parser.parse_args()
    if args.seconds <= 0 or not 1024 <= args.port <= 65531:
        parser.error("positive duration and port 1024..65531 required")
    run = (ROOT / "runs" / args.name).resolve()
    if run.exists():
        parser.error(f"run directory already exists: {run}")
    if sha256(args.binary) != args.expected_binary_sha256:
        parser.error("selected Blackfin binary does not match the expected experiment hash")
    install = install_blackfin(args.binary, ROOT / "bin/cdj-run")
    (ROOT / "runs").mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    epoch = time.time()
    command = [sys.executable, "-m", "tools.cdj_main.nxs_vm", str(run.relative_to(ROOT)),
               "--seconds", str(args.seconds), "--frame-interval", "0.2",
               "--port", str(args.port), "--qemu", str(args.qemu)]
    # nxs_vm creates the run directory itself (with exist_ok=False), so the
    # three live collectors write beside this script and are copied in after
    # teardown. This also keeps a failed early launch diagnosable.
    temporary_stem = SUMMARY_DIR / f".{args.name.replace('/', '_')}"
    launcher_log = temporary_stem.with_suffix(".launcher.log")
    process_samples = temporary_stem.with_suffix(".process-samples.jsonl")
    panel_log = temporary_stem.with_suffix(".panel-timeline.json")
    stop = threading.Event()
    with launcher_log.open("w", encoding="utf-8") as log:
        process = subprocess.Popen(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
        sampler = threading.Thread(target=poll_processes,
                                    args=(process.pid, started, process_samples, stop), daemon=True)
        panel = threading.Thread(target=panel_worker,
                                 args=(args.port + 4, started, args.seconds, panel_log, stop), daemon=True)
        sampler.start(); panel.start()
        try:
            process.wait()
        finally:
            stop.set()
            sampler.join(timeout=2)
            panel.join(timeout=2)
    run.mkdir(parents=True, exist_ok=True)
    run_launcher_log = run / "benchmark-launcher.log"
    run_process_samples = run / "process-samples.jsonl"
    run_panel_log = run / "panel-timeline.json"
    for source, destination in ((launcher_log, run_launcher_log),
                                (process_samples, run_process_samples),
                                (panel_log, run_panel_log)):
        if source.exists():
            os.replace(source, destination)
    launcher_log, process_samples, panel_log = run_launcher_log, run_process_samples, run_panel_log
    summary = summarize(run, install, command, epoch, process_samples, panel_log, launcher_log)
    summary_path = SUMMARY_DIR / f"{args.name}.json"
    temporary = summary_path.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(summary, indent=2) + "\n")
    temporary.replace(summary_path)
    print(json.dumps({"exit_code": process.returncode, "run": str(run),
                      "summary": str(summary_path)}, indent=2))
    return process.returncode


if __name__ == "__main__":
    raise SystemExit(main())
