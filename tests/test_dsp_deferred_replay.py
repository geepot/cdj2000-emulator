"""Deferred event transcript gates, using real checkpoint/core execution."""
import hashlib
import json
import subprocess
import sys

import pytest

from test_dsp_checkpoint_replay import ROOT, encoded_event, make_checkpoint


def run_deferred(tmp_path, mode, events, *, standalone=False):
    directory, checkpoint = make_checkpoint(tmp_path, mode)
    transcript = tmp_path / 'events.jsonl'
    transcript.write_text(encoded_event(1, 'dsp_start') + ''.join(
        encoded_event(i + 2, kind, **fields)
        for i, (kind, fields) in enumerate(events)))
    (directory / 'manifest.json').write_text(json.dumps({
        'complete': True,
        'checkpoints': [{'file': checkpoint.name,
                         'sha256': hashlib.sha256(checkpoint.read_bytes()).hexdigest()}],
        'event_transcript': {'sha256': hashlib.sha256(transcript.read_bytes()).hexdigest()},
    }))
    command = [sys.executable, '-m', 'tools.cdj_dsp.replay', str(checkpoint),
               str(tmp_path / 'output'), '--steps', '8192']
    if not standalone:
        command += ['--events', str(transcript), '--verify-repeat']
    return subprocess.run(command, cwd=ROOT, text=True, capture_output=True, timeout=60)


def fault_events(start=False):
    events = []
    if start:
        events.append(('dsp_schedule', dict(offset=1, value=1000000, size=1)))
    events += [
        ('dsp_slice_begin', dict(offset=1, address=1, value=1000000, size=4096)),
        ('dsp_slice_end', dict(offset=1, address=1)),
        ('dsp_stop', dict(address=0x11800020, value=0xdeadcafe)),
    ]
    return events


@pytest.mark.parametrize('mode', ['deferred-start', 'deferred-fault', 'deferred-phase',
                                  'deferred-phase-pending'])
def test_deferred_fault_slice_repeat(tmp_path, mode):
    events = fault_events(mode != 'deferred-fault')
    if mode == 'deferred-phase-pending':
        events[0][1]['size'] = 3  # request coalesces with pending activation
    result = run_deferred(tmp_path, mode, events)
    assert result.returncode == 0, result.stderr
    gate = json.loads((tmp_path / 'output/gate.json').read_text())
    assert gate['passed'] and gate['verified_connected_stops'] == 1


@pytest.mark.parametrize('change', ['quota', 'missing_end', 'wrong_id', 'overflow'])
def test_deferred_rejects_bad_slice_contract(tmp_path, change):
    events = fault_events()
    if change == 'quota': events[0][1]['size'] = 4095
    elif change == 'missing_end': del events[1]
    elif change == 'wrong_id': events[1][1]['offset'] = 2
    else: events[0][1]['address'] = 2**32
    result = run_deferred(tmp_path, 'deferred-fault', events)
    assert result.returncode != 0
    assert 'event replay mismatch' in result.stderr


def test_deferred_pending_requires_transcript(tmp_path):
    result = run_deferred(tmp_path, 'deferred-fault', [], standalone=True)
    assert result.returncode != 0
    assert 'requires event transcript' in result.stderr


def test_deferred_full_quota_boundary(tmp_path):
    events = [
        ('dsp_slice_begin', dict(offset=1, address=1, value=1000000, size=4096)),
        ('dsp_slice_end', dict(offset=1, address=1, value=995904, size=4096,
                              packets=4096, cycles=4096)),
        ('dsp_stop', dict(address=0x11804020, packets=4096, cycles=4096)),
    ]
    result = run_deferred(tmp_path, 'deferred-running', events)
    assert result.returncode == 0, result.stderr


def test_deferred_host_request_coalesces_between_slices(tmp_path):
    events = [
        ('dsp_slice_begin', dict(offset=1, address=1, value=1000000, size=4096)),
        ('dsp_slice_end', dict(offset=1, address=1, value=995904, size=4096,
                              packets=4096, cycles=4096)),
        ('dsp_stop', dict(address=0x11804020, packets=4096, cycles=4096)),
        ('boot_phase', dict(value=1, boot_phase=1, packets=4096, cycles=4096)),
        ('dsp_schedule', dict(offset=1, address=1, value=995904, size=3,
                             boot_phase=1, packets=4096, cycles=4096)),
        ('dsp_slice_begin', dict(offset=1, address=2, value=995904, size=4096,
                                boot_phase=1, packets=4096, cycles=4096)),
        ('dsp_slice_end', dict(offset=1, address=2, value=991808, size=4096,
                              boot_phase=1, packets=8192, cycles=8192)),
        ('dsp_stop', dict(address=0x11808020, boot_phase=1, packets=8192, cycles=8192)),
    ]
    result = run_deferred(tmp_path, 'deferred-running', events)
    assert result.returncode == 0, result.stderr
    gate = json.loads((tmp_path / 'output/gate.json').read_text())
    assert gate['verified_connected_stops'] == 2


def test_deferred_replay_preserves_64_bit_scheduler_ids(tmp_path):
    activation = 2**32 + 7
    slice_id = 2**32 + 12
    events = [
        ('dsp_slice_begin', dict(offset=activation, address=slice_id,
                                 value=1000000, size=4096)),
        ('dsp_slice_end', dict(offset=activation, address=slice_id)),
        ('dsp_stop', dict(address=0x11800020, value=0xdeadcafe)),
    ]
    result = run_deferred(tmp_path, 'deferred-high-ids', events)
    assert result.returncode == 0, result.stderr
    gate = json.loads((tmp_path / 'output/gate.json').read_text())
    assert gate['passed'] and gate['verified_connected_stops'] == 1


def test_deferred_replay_rejects_actual_uint64_activation_overflow(tmp_path):
    events = [('boot_phase', dict(value=1, boot_phase=1))]
    result = run_deferred(tmp_path, 'deferred-overflow', events)
    assert result.returncode != 0
    assert 'event replay mismatch' in result.stderr
