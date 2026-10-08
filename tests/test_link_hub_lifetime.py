# SPDX-License-Identifier: GPL-2.0-or-later
"""How long the Pro DJ Link hub lives and what it says when it ends.

A two-deck stand that was left running used to lose its hub after 3600 s (the default of --seconds) without a word,
and the decks printed "Connection reset by peer ... MAIN runs on alone".  Now the end is always an event
(`stopped` in events.jsonl and summary.json, with the reason), `--seconds 0` means until killed, and a hangup of the
shell that started the hub does not end it.
"""
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture
def workdir():
    path = Path(tempfile.mkdtemp(prefix="hub", dir="/tmp"))      # a unix socket path must stay short
    yield path
    shutil.rmtree(path, ignore_errors=True)


def start(workdir, seconds):
    out = workdir / "out"
    proc = subprocess.Popen([sys.executable, "-m", "tools.cdj_main.link_hub", str(out), "--sync", "--decks", "2",
                             "--seconds", str(seconds), "--listen", f"unix:{workdir}/h.sock"],
                            cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        if (out / "endpoint.json").exists():
            break
        time.sleep(0.1)
    return proc, out


def stopped(out):
    events = [json.loads(line) for line in (out / "events.jsonl").read_text().splitlines() if line.strip()]
    ends = [e for e in events if e.get("event") == "stopped"]
    assert len(ends) == 1, events
    return ends[0], json.loads((out / "summary.json").read_text())


@pytest.mark.skipif(not hasattr(signal, "SIGHUP"), reason="POSIX signals")
def test_the_deadline_is_a_named_event_not_a_silent_exit(workdir):
    proc, out = start(workdir, 1.5)
    assert proc.wait(timeout=15) == 0
    event, summary = stopped(out)
    assert event["reason"] == "deadline" and summary["stopped"] == "deadline"


@pytest.mark.skipif(not hasattr(signal, "SIGHUP"), reason="POSIX signals")
def test_zero_seconds_means_until_killed_and_a_hangup_does_not_end_it(workdir):
    proc, out = start(workdir, 0)
    try:
        time.sleep(1.5)
        assert proc.poll() is None                     # not "0 seconds, gone at once"
        os.kill(proc.pid, signal.SIGHUP)
        time.sleep(1.0)
        assert proc.poll() is None                     # a closed shell does not take the hub with it
        os.kill(proc.pid, signal.SIGTERM)
        assert proc.wait(timeout=15) == 0
    finally:
        if proc.poll() is None:
            proc.kill()
    event, summary = stopped(out)
    assert event["reason"] == "signal SIGTERM" and summary["stopped"] == "signal SIGTERM"
