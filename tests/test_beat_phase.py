# SPDX-License-Identifier: GPL-2.0-or-later
"""tools.cdj_main.beat_phase on synthetic captures: known offsets in, the same offsets out."""
import json
import socket
import struct

import pytest

from tools.cdj_main import beat_phase, link_hub

SRC_MAC = bytes.fromhex("020000000002")


def udp_frame(port, payload):
    udp = struct.pack("!HHHH", port, port, 8 + len(payload), 0) + payload
    ip = bytearray(struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(udp), 0, 0, 64, 17, 0,
                               socket.inet_aton("169.254.1.2"), socket.inet_aton("169.254.255.255")))
    ip[10:12] = struct.pack("!H", link_hub.ip_checksum(bytes(ip)))
    return b"\xff" * 6 + SRC_MAC + b"\x08\x00" + bytes(ip) + udp


def packet(kind, player, port=50001, size=0x60):
    payload = bytearray(link_hub.PRODJ_MAGIC + bytes([kind]) + b"\0" + b"CDJ-2000".ljust(20, b"\0"))
    payload += b"\0" * (size - len(payload))
    payload[0x21] = player
    return udp_frame(port, bytes(payload))


def capture(a_beats, b_beats, extra=()):
    frames = [(t, packet(0x28, 1)) for t in a_beats] + [(t, packet(0x28, 2)) for t in b_beats] + list(extra)
    return sorted(frames, key=lambda f: f[0])


def beats(start, period, count, shift=0.0):
    return [start + shift + i * period for i in range(count)]


def test_a_known_lag_is_reported_in_milliseconds():
    a = beats(10.0, 0.5, 40)
    result = beat_phase.report(capture(a, beats(10.0, 0.5, 40, 0.012)), 1, 2)
    w = result["windows"][0]
    assert w["after_previous"]["n"] == 40
    assert w["after_previous"]["mean"] == pytest.approx(12.0, abs=0.01)
    assert w["after_previous"]["min"] == pytest.approx(12.0, abs=0.01)
    assert w["after_previous"]["max"] == pytest.approx(12.0, abs=0.01)
    assert w["nearest_signed"]["mean"] == pytest.approx(12.0, abs=0.01)
    assert result["stamps"].startswith("guest")


def test_b_ahead_of_a_is_a_negative_nearest_offset_and_a_long_lag_after_the_previous_beat():
    result = beat_phase.report(capture(beats(10.0, 0.5, 40), beats(10.0, 0.5, 40, -0.008)), 1, 2)
    w = result["windows"][0]
    # the first beat of B has no beat of A before it; the other 39 sit 492 ms after the previous one
    assert w["after_previous"]["n"] == 39
    assert w["after_previous"]["mean"] == pytest.approx(492.0, abs=0.01)
    assert w["nearest_signed"]["mean"] == pytest.approx(-8.0, abs=0.01)


def test_jitter_shows_in_min_max_and_sd():
    b = [t + (0.010 if i % 2 else 0.004) for i, t in enumerate(beats(10.0, 0.5, 40))]
    w = beat_phase.report(capture(beats(10.0, 0.5, 40), b), 1, 2)["windows"][0]["after_previous"]
    assert (w["min"], w["max"]) == (pytest.approx(4.0, abs=0.01), pytest.approx(10.0, abs=0.01))
    assert w["mean"] == pytest.approx(7.0, abs=0.01)
    assert w["stdev"] == pytest.approx(3.0, abs=0.01)


def test_a_window_selects_beats_of_b_by_time_and_every_splits_it():
    frames = capture(beats(10.0, 0.5, 80), beats(10.0, 0.5, 80, 0.020))
    only = beat_phase.report(frames, 1, 2, t0=20.0, t1=30.0)["windows"]
    assert len(only) == 1 and only[0]["after_previous"]["n"] == 20
    rows = beat_phase.report(frames, 1, 2, t0=10.0, t1=50.0, every=10.0)["windows"]
    assert [r["from"] for r in rows] == [10.0, 20.0, 30.0, 40.0]
    assert all(r["after_previous"]["n"] == 20 for r in rows)


def test_a_drifting_deck_shows_its_drift_window_by_window():
    b = [t + 0.001 * (t - 10.0) for t in beats(10.0, 0.5, 80)]       # 1 ms more lag every second
    rows = beat_phase.report(capture(beats(10.0, 0.5, 80), b), 1, 2, every=10.0)["windows"]
    means = [r["after_previous"]["mean"] for r in rows]
    assert means == sorted(means) and means[-1] - means[0] > 25


def test_only_beat_packets_of_port_50001_and_the_two_players_count():
    noise = [(10.1, packet(0x0a, 2, port=50002)),                    # a status packet of B
             (10.2, packet(0x28, 3)),                                 # a third deck's beat
             (10.3, packet(0x28, 2, port=50000)),                     # a beat-type packet on the wrong port
             (10.4, b"\x00" * 60)]                                    # not UDP at all
    got = beat_phase.beats_by_player(capture([10.0], [10.0], noise))
    assert sorted(got) == [1, 2, 3] and got[2] == [10.0] and got[3] == [10.2]


def test_a_stale_reference_beat_is_skipped():
    a = [10.0]
    b = [10.01, 14.0]                                                # the second is 4 s after A's last beat
    w = beat_phase.report(capture(a, b), 1, 2)["windows"][0]["after_previous"]
    assert w["n"] == 1 and w["mean"] == pytest.approx(10.0, abs=0.01)


def test_command_line_reads_a_pcap_file(tmp_path, capsys):
    path = tmp_path / "link.pcap"
    path.write_bytes(link_hub.pcap_header() + b"".join(
        link_hub.pcap_record(t, f) for t, f in capture(beats(10.0, 0.5, 20), beats(10.0, 0.5, 20, 0.015))))
    assert beat_phase.main([str(path), "1", "2", "--json"]) == 0
    result = json.loads(capsys.readouterr().out)
    assert result["windows"][0]["after_previous"]["mean"] == pytest.approx(15.0, abs=0.01)
    assert beat_phase.main([str(path), "1", "2", "--every", "5"]) == 0
    out = capsys.readouterr().out
    assert "players with beats: [1, 2]" in out and "guest time" in out
    assert beat_phase.main([str(path), "1", "9"]) == 1               # player 9 never played
