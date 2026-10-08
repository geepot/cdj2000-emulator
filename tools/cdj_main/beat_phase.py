# SPDX-License-Identifier: GPL-2.0-or-later
"""Beat-phase offset of one deck against another, from the Pro DJ Link hub's capture.

    python -m tools.cdj_main.beat_phase OUT/hub/link.pcap PLAYER_A PLAYER_B [--from S] [--to S] [--every S] [--json]

The beat packets are the type 0x28 datagrams of UDP port 50001 (byte 0x21 = the player number).  For every beat of
deck B in the window the closest PRECEDING beat of deck A is taken and the difference is reported in milliseconds:
"B sits N ms after A's last beat".  Because that figure jumps by a whole beat period when B is slightly AHEAD of A,
the signed offset to the NEAREST beat of A (either side, within half a period) is printed too.

Time stamps are the pcap's.  A hub started with `--sync` (scripts/two-decks.sh) writes the decks' common guest time in
seconds, so the figures are guest milliseconds; a hub without it writes host time and the figures mean what the host
saw (the script says which it looks like).  Statistics are mean, min, max and the standard deviation of the figures
in the window; `--every S` prints one row per S seconds of the window instead of one for all of it.
"""
from __future__ import annotations

import argparse
import bisect
import json
import math
import sys
from pathlib import Path

from tools.cdj_main import link_hub

BEAT_PORT = 50001
BEAT_TYPE = 0x28
PLAYER_AT = 0x21


def beats_by_player(frames) -> dict[int, list[float]]:
    """{player number: sorted beat time stamps} from (stamp, ethernet frame) pairs."""
    out: dict[int, list[float]] = {}
    for stamp, frame in frames:
        parsed = link_hub.udp(frame)
        if parsed is None:
            continue
        _, port, payload = parsed
        if (port != BEAT_PORT or payload[:10] != link_hub.PRODJ_MAGIC or len(payload) <= PLAYER_AT
                or payload[10] != BEAT_TYPE):
            continue
        out.setdefault(payload[PLAYER_AT], []).append(stamp)
    for times in out.values():
        times.sort()
    return out


def stats(values: list[float]) -> dict:
    if not values:
        return {"n": 0, "mean": None, "min": None, "max": None, "stdev": None}
    mean = sum(values) / len(values)
    var = sum((v - mean) ** 2 for v in values) / len(values)
    return {"n": len(values), "mean": round(mean, 3), "min": round(min(values), 3), "max": round(max(values), 3),
            "stdev": round(math.sqrt(var), 3)}


def offsets(a: list[float], b: list[float], t0: float | None = None, t1: float | None = None,
            max_gap: float = 2.0) -> tuple[list[float], list[float]]:
    """(lag after A's previous beat, signed offset to A's nearest beat), both in ms, one pair per beat of B in
    [t0, t1).  A beat of B with no beat of A within `max_gap` seconds before it is skipped."""
    after, nearest = [], []
    for tb in b:
        if (t0 is not None and tb < t0) or (t1 is not None and tb >= t1):
            continue
        i = bisect.bisect_right(a, tb)           # a[i - 1] is the last beat of A at or before tb
        if i == 0 or tb - a[i - 1] > max_gap:
            continue
        prev = a[i - 1]
        after.append((tb - prev) * 1000.0)
        near = prev
        if i < len(a) and a[i] - tb < tb - prev:
            near = a[i]
        nearest.append((tb - near) * 1000.0)
    return after, nearest


def report(frames, player_a: int, player_b: int, t0=None, t1=None, every=None, max_gap=2.0) -> dict:
    beats = beats_by_player(frames)
    a, b = beats.get(player_a, []), beats.get(player_b, [])
    lo = t0 if t0 is not None else (b[0] if b else 0.0)
    hi = t1 if t1 is not None else (b[-1] + 1e-6 if b else 0.0)
    windows = []
    step = every if every else (hi - lo) or 1.0
    start = lo
    while start < hi:
        end = min(start + step, hi)
        after, nearest = offsets(a, b, start, end, max_gap)
        windows.append({"from": round(start, 3), "to": round(end, 3), "after_previous": stats(after),
                        "nearest_signed": stats(nearest)})
        start = end
    return {"player_a": player_a, "player_b": player_b, "beats_a": len(a), "beats_b": len(b),
            "players_seen": sorted(beats), "stamps": "host time" if (b and b[0] > 1e8) else "guest time (sync hub)",
            "windows": windows}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pcap", type=Path)
    ap.add_argument("player_a", type=int, help="the reference deck's player number")
    ap.add_argument("player_b", type=int, help="the deck whose beats are placed against A's")
    ap.add_argument("--from", dest="t0", type=float, help="window start, seconds of the capture's clock")
    ap.add_argument("--to", dest="t1", type=float, help="window end (exclusive)")
    ap.add_argument("--every", type=float, help="one row per this many seconds")
    ap.add_argument("--max-gap", type=float, default=2.0, help="skip a beat of B when A's last beat is older (s)")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args(argv)
    result = report(link_hub.read_pcap(a.pcap), a.player_a, a.player_b, a.t0, a.t1, a.every, a.max_gap)
    if a.json:
        print(json.dumps(result, indent=1))
        return 0
    print(f"{a.pcap}: beats of player {a.player_a} = {result['beats_a']}, of player {a.player_b} = "
          f"{result['beats_b']}; players with beats: {result['players_seen']}; time stamps: {result['stamps']}")
    if not result["beats_a"] or not result["beats_b"]:
        print("no beats from one of the two players (the decks must play, and the hub must have seen port 50001)",
              file=sys.stderr)
        return 1
    print(f"{'window (s)':>16}  {'B after A prev: n':>5} {'mean':>8} {'min':>8} {'max':>8} {'sd':>7}   "
          f"{'nearest (signed): mean':>8} {'min':>8} {'max':>8} {'sd':>7}   (ms)")
    for w in result["windows"]:
        p, n = w["after_previous"], w["nearest_signed"]
        if not p["n"]:
            print(f"{w['from']:7.1f}-{w['to']:7.1f}  (no beat pairs)")
            continue
        print(f"{w['from']:7.1f}-{w['to']:7.1f}  {p['n']:>17} {p['mean']:8.1f} {p['min']:8.1f} {p['max']:8.1f} "
              f"{p['stdev']:7.1f}   {n['mean']:>22.1f} {n['min']:8.1f} {n['max']:8.1f} {n['stdev']:7.1f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
