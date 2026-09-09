"""End-to-end gates for connected-event injection into DSP checkpoints."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]


def encoded_event(sequence, kind, *, offset=0, address=0, value=0, size=0,
                  boot_phase=0, hint=False, dspint=False, packets=0, cycles=0):
    event = dict(sequence=sequence, event=kind, offset=offset, address=address,
                 value=value, size=size, boot_phase=boot_phase, hint=hint,
                 dspint=dspint, packets=packets, cycles=cycles)
    return json.dumps(event, separators=(',', ':')) + '\n'


def make_checkpoint(tmp_path):
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
        str(ROOT / 'emulator/qemu/cdj_c6747_intc.c'),
        '-o', str(maker),
    ], check=True)
    subprocess.run([str(maker), str(checkpoint)], check=True, timeout=5)
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
