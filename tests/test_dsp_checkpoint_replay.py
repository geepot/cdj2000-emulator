"""End-to-end gates for connected-event injection into DSP checkpoints."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_newest_checkpoint_filters_modes_and_transcript(tmp_path):
    from tools.cdj_dsp.replay import newest_checkpoint

    _, original = make_checkpoint(tmp_path)
    payload = original.read_bytes()
    candidates = tmp_path / 'candidates'
    for index, (name, timing, audio, transcript) in enumerate([
            ('strict', 'strict', 'stopped-clock', 'wanted'),
            ('other_events', 'strict', 'stopped-clock', 'other'),
            ('audio', 'strict', 'coarse-packet-slots', 'wanted'),
            ('exploratory', 'functional-runahead', 'stopped-clock', 'wanted')]):
        directory = candidates / name
        directory.mkdir(parents=True)
        path = directory / 'state.cdjdsp'
        path.write_bytes(payload)
        os.utime(path, ns=(index + 1, index + 1))
        (directory / 'manifest.json').write_text(json.dumps({
            'complete': True,
            'checkpoints': [{'file': path.name,
                             'sha256': hashlib.sha256(payload).hexdigest()}],
            'dsp_timing_mode': timing, 'dsp_audio_mode': audio,
            'event_transcript': {'sha256': transcript},
        }))
    selected, *_ = newest_checkpoint(candidates, timing_mode='strict',
                                     audio_mode='stopped-clock', event_hash='wanted')
    assert selected.parent.name == 'strict'
    selected, *_ = newest_checkpoint(candidates, timing_mode='functional-runahead',
                                     audio_mode='stopped-clock', event_hash='wanted')
    assert selected.parent.name == 'exploratory'
    with pytest.raises(ValueError, match='no compatible'):
        newest_checkpoint(candidates, event_hash='absent')


def encoded_event(sequence, kind, *, offset=0, address=0, value=0, size=0,
                  boot_phase=0, hint=False, dspint=False, packets=0, cycles=0):
    event = dict(sequence=sequence, event=kind, offset=offset, address=address,
                 value=value, size=size, boot_phase=boot_phase, hint=hint,
                 dspint=dspint, packets=packets, cycles=cycles)
    return json.dumps(event, separators=(',', ':')) + '\n'


def make_checkpoint(tmp_path, stop_reason='boot-phase boundary'):
    cc = shutil.which('cc')
    if not cc:
        pytest.skip('requires C compiler')
    checkpoint_dir = tmp_path / 'checkpoints'
    checkpoint_dir.mkdir()
    checkpoint = checkpoint_dir / 'state.cdjdsp'
    maker = tmp_path / 'checkpoint-maker'
    subprocess.run([
        cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'),
        str(ROOT / 'tests/cstub/dsp-event-checkpoint.c'),
        str(ROOT / 'emulator/qemu/cdj_dsp_checkpoint.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_syscfg.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_intc.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_timer.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_spi.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_cache.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_mcasp.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_edma.c'),
        '-o', str(maker),
    ], check=True)
    subprocess.run([str(maker), str(checkpoint), stop_reason],
                   check=True, timeout=5)
    return checkpoint_dir, checkpoint


def test_injects_and_repeat_gates_connected_stop(tmp_path):
    checkpoint_dir, checkpoint = make_checkpoint(tmp_path)

    transcript = tmp_path / 'events.jsonl'
    transcript.write_text(
        encoded_event(1, 'boot_phase') +
        encoded_event(2, 'dsp_stop', address=0x11800020,
                      value=0xdeadcafe)
    )
    checkpoint_hash = hashlib.sha256(checkpoint.read_bytes()).hexdigest()
    event_hash = hashlib.sha256(transcript.read_bytes()).hexdigest()
    (checkpoint_dir / 'manifest.json').write_text(json.dumps({
        'complete': True,
        'checkpoints': [{'file': checkpoint.name, 'sha256': checkpoint_hash}],
        'event_transcript': {'sha256': event_hash},
    }))

    output = tmp_path / 'replay'
    result = subprocess.run([
        sys.executable, '-m', 'tools.cdj_dsp.replay', str(checkpoint), str(output),
        '--steps', '100', '--events', str(transcript), '--verify-repeat',
    ], cwd=ROOT, text=True, capture_output=True, timeout=30)
    assert result.returncode == 0, result.stderr
    gate = json.loads((output / 'gate.json').read_text())
    assert gate['passed']
    assert gate['verified_connected_stops'] == 1
    assert gate['repeat_matches'] and gate['final_state_and_memory_match']
    failure = json.loads((output / 'failure.json').read_text())
    assert failure['outcome'] == 'fail_closed_fault'
    assert failure['stop']['fault']
    assert failure['stop']['reason'] == failure['stop']['fault']


def test_rejects_event_transcript_without_manifest_provenance(tmp_path):
    checkpoint_dir, checkpoint = make_checkpoint(tmp_path)
    (checkpoint_dir / 'manifest.json').write_text(json.dumps({
        'complete': True,
        'checkpoints': [{
            'file': checkpoint.name,
            'sha256': hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
        }],
    }))
    transcript = tmp_path / 'events.jsonl'
    transcript.write_text(encoded_event(1, 'boot_phase'))
    result = subprocess.run([
        sys.executable, '-m', 'tools.cdj_dsp.replay', str(checkpoint),
        str(tmp_path / 'replay'), '--events', str(transcript),
    ], cwd=ROOT, text=True, capture_output=True, timeout=10)
    assert result.returncode != 0
    assert 'event-transcript provenance' in result.stderr


def test_rejects_connected_stop_state_mismatch(tmp_path):
    checkpoint_dir, checkpoint = make_checkpoint(tmp_path)
    transcript = tmp_path / 'events.jsonl'
    transcript.write_text(
        encoded_event(1, 'boot_phase') +
        encoded_event(2, 'dsp_stop', address=0x11800020,
                      value=0xdeadcafe, packets=1)
    )
    (checkpoint_dir / 'manifest.json').write_text(json.dumps({
        'complete': True,
        'checkpoints': [{
            'file': checkpoint.name,
            'sha256': hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
        }],
        'event_transcript': {
            'sha256': hashlib.sha256(transcript.read_bytes()).hexdigest(),
        },
    }))
    result = subprocess.run([
        sys.executable, '-m', 'tools.cdj_dsp.replay', str(checkpoint),
        str(tmp_path / 'replay'), '--steps', '100', '--events', str(transcript),
    ], cwd=ROOT, text=True, capture_output=True, timeout=30)
    assert result.returncode != 0
    assert 'event replay mismatch at sequence 2 (dsp_stop)' in result.stderr


def test_later_dspint_alone_resumes_event_replay(tmp_path):
    checkpoint_dir, checkpoint = make_checkpoint(
        tmp_path, stop_reason='phase budget exhausted')
    transcript = tmp_path / 'events.jsonl'
    transcript.write_text(
        encoded_event(1, 'boot_phase') +
        encoded_event(2, 'hpi_host_control_write', address=0x11800000,
                      value=2, size=4, dspint=True) +
        encoded_event(3, 'dsp_stop', address=0x118001e0,
                      value=0xdeadcafe, dspint=True)
    )
    (checkpoint_dir / 'manifest.json').write_text(json.dumps({
        'complete': True,
        'checkpoints': [{
            'file': checkpoint.name,
            'sha256': hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
        }],
        'event_transcript': {
            'sha256': hashlib.sha256(transcript.read_bytes()).hexdigest(),
        },
    }))
    output = tmp_path / 'replay'
    result = subprocess.run([
        sys.executable, '-m', 'tools.cdj_dsp.replay', str(checkpoint),
        str(output), '--steps', '100', '--events', str(transcript),
        '--verify-repeat',
    ], cwd=ROOT, text=True, capture_output=True, timeout=30)
    assert result.returncode == 0, result.stderr
    gate = json.loads((output / 'gate.json').read_text())
    assert gate['passed'] and gate['verified_connected_stops'] == 1
    assert gate['repeat_matches'] and gate['final_state_and_memory_match']
