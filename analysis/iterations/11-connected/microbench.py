#!/usr/bin/env python3
"""Alternate five baseline/candidate fixed-tick replay pairs.

The candidate binary is supplied by the caller; this script never builds or
installs it. Outputs are disposable ``runs/optimization-11-micro-*`` dirs.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[3]


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def run_one(label: str, n: int, binary: Path, elf: Path, link: Path,
            benchmark: Path, output: Path, ticks: int) -> dict:
    target = output / f"optimization-11-micro-{label}-{n}"
    subprocess.run([
        sys.executable, str(benchmark), "--binary", str(binary),
        "--elf", str(elf), "--link-dump", str(link), "--ticks", str(ticks),
        "--trials", "1", "--output", str(target),
    ], cwd=ROOT, check=True)
    report = json.loads((target / "measurements.json").read_text())
    trial = report["trials"][0]
    return {
        "label": label, "pair": n, "output": str(target),
        "binary": str(binary), "binary_sha256": sha256(binary),
        "seconds": trial["seconds"], "ticks": trial["ticks"],
        "instructions": trial["instructions"],
        "frame_sha256": trial["frame_sha256"], "tx_sha256": trial["tx_sha256"],
    }


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--baseline", type=Path, default=ROOT / "build/performance/11/baseline")
    p.add_argument("--candidate", type=Path, required=True)
    p.add_argument("--elf", type=Path, default=ROOT / "firmware/nxs/gui-boot-memory.elf")
    p.add_argument("--link-dump", type=Path,
                   default=ROOT / "runs/optimization-11-profile-2/main-link.bin")
    p.add_argument("--benchmark", type=Path,
                   default=ROOT / "tools/cdj_gui/benchmark_sim.py")
    p.add_argument("--output", type=Path, default=ROOT / "runs")
    p.add_argument("--ticks", type=int, default=60_000_000)
    p.add_argument("--pairs", type=int, default=5)
    p.add_argument("--report", type=Path,
                   default=Path(__file__).with_name("microbench-summary.json"))
    args = p.parse_args()
    rows = []
    for n in range(args.pairs):
        rows.append(run_one("baseline", n, args.baseline, args.elf,
                            args.link_dump, args.benchmark, args.output, args.ticks))
        rows.append(run_one("candidate", n, args.candidate, args.elf,
                            args.link_dump, args.benchmark, args.output, args.ticks))
    for n in range(args.pairs):
        base, cand = rows[2 * n:2 * n + 2]
        cand["matched"] = {k: base[k] == cand[k]
                           for k in ("ticks", "instructions", "frame_sha256", "tx_sha256")}
    report = {"ticks_requested": args.ticks, "pairs": args.pairs,
              "inputs": {"elf_sha256": sha256(args.elf),
                         "link_dump_sha256": sha256(args.link_dump),
                         "board_sha256": sha256(ROOT / "emulator/cdj2000-gui-nxs.hw")},
              "baseline_sha256": sha256(args.baseline),
              "candidate_sha256": sha256(args.candidate), "trials": rows}
    destination = args.report
    destination.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
