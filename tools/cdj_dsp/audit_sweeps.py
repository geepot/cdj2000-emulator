"""Reproducible C674x audit sweeps: compact space, control registers, rollback.

These three measurements were first made with throwaway scratch programs, whose
numbers no one could regenerate once the session ended.  An audit number that
cannot be reproduced is not evidence, so the sweeps live in
``tests/cstub/c674x-isa-probe.c`` and this module drives them and records the
result with the hashes of everything that produced it.

    python -m tools.cdj_dsp.audit_sweeps analysis/dsp/audit_sweeps.json

What each sweep does and does not establish:

* ``compact`` - all 65,536 16-bit words under 13 named compact-header
  configurations and 3 named register profiles.  A word counts as accepted if any
  configuration accepts it, and the record names which.  This is DECODE
  ACCEPTANCE, not semantics.  The defensible coverage figure is the count of
  words rejected as "compact instruction not implemented": it is a property of
  the decoder alone, unlike the accepted total, which moves with the probe's
  memory window and register values.
* ``control`` - drives a real ``MVC`` in both directions for all 32 control
  register ids, using encodings taken from TI's assembler.  The recorded mask is
  what reads back after writing all ones, so it is directly comparable to the
  reserved-bit columns of the SPRUFE8B per-register field tables.
* ``atomicity`` - a rejected execute packet must leave CPU state and memory
  bit-identical.  Failures here would mean a partially applied packet.
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PROBE = ROOT / "tests" / "cstub" / "c674x-isa-probe.c"
CORE = (ROOT / "emulator" / "qemu" / "cdj_c674x.c",
        ROOT / "emulator" / "qemu" / "cdj_c674x_loop.c")


def build(workdir: Path) -> Path:
    compiler = shutil.which("cc")
    if not compiler:
        raise SystemExit("no C compiler on PATH")
    binary = workdir / "audit-probe"
    subprocess.run(
        [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
         "-I", str(ROOT / "emulator" / "qemu"), str(PROBE),
         *[str(p) for p in CORE], "-o", str(binary)],
        check=True, capture_output=True, text=True)
    return binary


def run(binary: Path, mode: str) -> str:
    # atomicity exits nonzero when a rollback fails, which is a result, not an error.
    return subprocess.run([str(binary), mode], capture_output=True, text=True,
                          timeout=1800).stdout


def compact(text: str) -> dict:
    accepted, rejects, configs = {}, collections.Counter(), collections.Counter()
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[1] == "accept":
            accepted[parts[0]] = (parts[2], parts[3])
            configs[f"{parts[2]}/{parts[3]}"] += 1
        elif len(parts) >= 3:
            rejects[" ".join(parts[2:])] += 1
    words = len(accepted) + sum(rejects.values())
    return {
        "words_swept": words,
        "accepted": len(accepted),
        "rejected": sum(rejects.values()),
        "rejection_reasons": dict(rejects.most_common()),
        "accepting_configurations": dict(configs.most_common()),
        "defensible_coverage_figure": {
            "metric": "16-bit words rejected as \"compact instruction not implemented\"",
            "value": rejects.get("compact instruction not implemented", 0),
            "of": words,
            "why": ("Independent of the probe's memory window and register values, "
                    "unlike the accepted total."),
        },
    }


def control(text: str) -> dict:
    rows = {}
    for line in text.splitlines():
        m = re.match(r"\s*(\d+) write=(\w+) read=(\w+) mask=([0-9a-f]{8})", line)
        if m:
            rows[int(m.group(1))] = {
                "write": m.group(2), "read": m.group(3), "read_mask": m.group(4),
            }
    readable = sorted(i for i, r in rows.items() if r["read"] == "accept")
    writable = sorted(i for i, r in rows.items() if r["write"] == "accept")
    unmasked = sorted(i for i, r in rows.items()
                      if r["read"] == "accept" and r["read_mask"] == "ffffffff")
    return {
        "ids_swept": len(rows),
        "readable_ids": readable,
        "writable_ids": writable,
        "absent_ids": sorted(set(range(32)) - set(readable) - set(writable)),
        "read_accept_but_unmasked_ids": unmasked,
        "note": ("read_mask is what reads back after writing 0xffffffff, so a mask "
                 "of ffffffff means the model stores every bit.  Compare each mask "
                 "against that register's SPRUFE8B field table: any bit the manual "
                 "marks Reserved must read 0."),
        "rows": {str(k): v for k, v in sorted(rows.items())},
    }


def atomicity(text: str) -> dict:
    cases, failures = [], None
    for line in text.splitlines():
        m = re.match(r"(\S+)\s+execute=(\d)\s+state_unchanged=(\d)\s+memory_unchanged=(\d)", line)
        if m:
            cases.append({"case": m.group(1), "executed": m.group(2) == "1",
                          "state_unchanged": m.group(3) == "1",
                          "memory_unchanged": m.group(4) == "1"})
        m = re.match(r"atomicity failures=(\d+)", line)
        if m:
            failures = int(m.group(1))
    return {"cases": cases, "failures": failures}


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("output", type=Path)
    args = ap.parse_args(argv)

    import tempfile
    with tempfile.TemporaryDirectory(prefix="cdj-audit-sweeps-") as tmp:
        binary = build(Path(tmp))
        report = {
            "schema": 1,
            "provenance": {
                "probe_source": str(PROBE.relative_to(ROOT)),
                "probe_source_sha256": sha(PROBE),
                "core_sources": {str(p.relative_to(ROOT)): sha(p) for p in CORE},
                "generator_sha256": sha(Path(__file__)),
            },
            "compact": compact(run(binary, "compact")),
            "control_registers": control(run(binary, "control")),
            "packet_atomicity": atomicity(run(binary, "atomicity")),
        }

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                           encoding="utf-8")
    print(json.dumps({
        "compact_not_implemented": report["compact"]["defensible_coverage_figure"]["value"],
        "compact_accepted": report["compact"]["accepted"],
        "control_readable": len(report["control_registers"]["readable_ids"]),
        "control_absent": len(report["control_registers"]["absent_ids"]),
        "control_unmasked": report["control_registers"]["read_accept_but_unmasked_ids"],
        "atomicity_failures": report["packet_atomicity"]["failures"],
    }, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
