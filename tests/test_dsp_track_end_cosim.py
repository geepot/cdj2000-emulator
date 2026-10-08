# SPDX-License-Identifier: GPL-2.0-or-later
"""The DSP model at a track's end and at a reverse run's start, on the real MAIN firmware.

These need the things the repository does not ship (see FIRMWARE.md), so they skip without them:

    firmware/       your own C2KMAIN.UPD / C2KGUI.UPD, unpacked
    bin/cdj-run     the Blackfin GUI simulator (scripts/build-bfin-sim.sh)
    CDJ_QEMU        a qemu-system-sh4 built from this tree
    CDJ_TEST_CARD   a card.img whose playlist holds at least three tracks, so the one
                    loaded has a track before and after it (tools.cdj_main.rb_card)

and optionally

    CDJ_TEST_CARD_ARGS   cosim_scenario arguments that reach the playlist, e.g.
                         "--root-row 4 --playlist-row 0"
    CDJ_TEST_TRACK_ROW   the row of the track to load (default 1: not the first, not the last)
    CDJ_TEST_NEEDLE      the needle position that parks the deck a few seconds before the end of
                         that track (default 380)

Each test boots the machine once and reads the model's log (qemu.err); about 5 minutes of wall
clock each.  The log lines are the model's own, in emulator/qemu/cdj2000_dsp_model.c.
"""
import os
from pathlib import Path
import re
import shlex
import socket
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
CARD = os.environ.get("CDJ_TEST_CARD")
QEMU = os.environ.get("CDJ_QEMU")

pytestmark = pytest.mark.skipif(
    not (CARD and QEMU and Path(CARD).is_file() and Path(QEMU).is_file()
         and (ROOT / "firmware/main-firmware.bin").is_file()
         and (ROOT / "bin/cdj-run").is_file()),
    reason="needs firmware/, bin/cdj-run, CDJ_QEMU and CDJ_TEST_CARD (see the module docstring)")

TRACK_ROW = os.environ.get("CDJ_TEST_TRACK_ROW", "1")
NEEDLE = os.environ.get("CDJ_TEST_NEEDLE", "380")
# The seconds are guest time.  The model holds the 7 for CDJ_DSP_SWITCH_HOLD (100 ms); the stock
# MAIN waits 10 s when nobody answers it.
ANSWER_WITHIN = 1.0
QUIET_AFTER = 8.0


def free_port():
    """A base port with room for the run's PORT..PORT+5."""
    for port in range(7900, 8400, 10):
        sockets = []
        try:
            for offset in range(6):
                sock = socket.socket()
                sock.bind(("127.0.0.1", port + offset))
                sockets.append(sock)
            return port
        except OSError:
            continue
        finally:
            for sock in sockets:
                sock.close()
    pytest.skip("no free port range")


def scenario(tmp_path, then):
    command = [sys.executable, "-m", "tools.cdj_main.cosim_scenario", "--card", CARD,
               "--out", str(tmp_path / "run"), "--port", str(free_port()),
               "--track-row", TRACK_ROW,
               *shlex.split(os.environ.get("CDJ_TEST_CARD_ARGS", "")),
               "--then", then]
    done = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=3000)
    assert done.returncode == 0, done.stdout[-2000:] + done.stderr[-2000:]
    return (tmp_path / "run/qemu.err").read_text(errors="replace").splitlines()


def stamp(line):
    return float(re.search(r" t=(\d+\.\d+)\s*$", line).group(1))


def first(log, pattern):
    for index, line in enumerate(log):
        if re.search(pattern, line):
            return index
    raise AssertionError(f"no log line matches {pattern!r}")


def check_switch(log, switch):
    """The 7 at `switch`, the answer 8 within a second, and nothing reloads afterwards."""
    answer = first(log[switch:], r"\+0x7bf8 = 8 \(answer after the 7\)") + switch
    assert stamp(log[answer]) - stamp(log[switch]) <= ANSWER_WITHIN
    tail = log[answer:]
    cosim = [float(m.group(1)) for line in tail if (m := re.match(r"cdj2000-cosim: t=(\d+\.\d+)", line))]
    assert cosim and cosim[-1] - stamp(log[answer]) >= QUIET_AFTER, \
        "the run ended too soon after the answer to show that nothing reloads"
    # A reload to the cue (the stock behaviour with no answer) flushes the DSP and opens the
    # stream again; with the answer MAIN only stands the deck.
    for line in tail:
        assert "+0x7cb0 =" not in line, line
        assert "stream open" not in line or "names record" not in line, line


def test_track_end_switches_at_once(tmp_path):
    # Pause, park the needle a few seconds before the end, let go, PLAY, wait past the end.
    log = scenario(tmp_path, f"16.0:2,needle={NEEDLE}:3,needle-off:3,16.0:25")
    switch = first(log, r"record \d+ ends at \d+ ms: playing on into record \d+")
    check_switch(log, switch)


def test_reverse_at_the_start_goes_to_the_previous_track(tmp_path):
    # Play on a little, then the DIRECTION lever to REV: the run reaches the start of the record.
    log = scenario(tmp_path, "direction-rev:40")
    switch = first(log, r"reverse run reached the start of record \d+: going on into the previous record 255")
    check_switch(log, switch)
