"""The replay tool must distinguish faults, limits and pre-execution stops."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import pytest

from tools.cdj_dsp.tx_capture import tx_capture_metadata

ROOT = Path(__file__).resolve().parents[1]


def run(dump, output, *args):
    return subprocess.run([sys.executable, '-m', 'tools.cdj_dsp.replay',
                           str(dump), str(output), *args], cwd=ROOT,
                          text=True, capture_output=True, timeout=20)


def test_replay_records_false_and_true_source_predicates(tmp_path):
    data = bytearray(0x40000)
    struct.pack_into('<I', data, 0, 0x00800020)
    struct.pack_into('<III', data, 0x20,
                     (1 << 29) | (3 << 23) | (123 << 7) | 0x28,
                     (1 << 7) | 0x2a,
                     (1 << 29) | (4 << 23) | (123 << 7) | 0x28)
    dump = tmp_path / 'predicates.bin'
    dump.write_bytes(data)
    output = tmp_path / 'replay'
    result = run(dump, output, '--steps', '3', '--verify-repeat')
    assert result.returncode == 0, result.stderr
    report = json.loads((output / 'coverage.json').read_text())
    rows = sorted(report['confirmed_instructions'], key=lambda row: row['pc'])
    assert [row['source_predicate_outcomes'] for row in rows] == [[False], [True], [True]]
    assert report['source_predicate_audit'] == dict(true_observed_addresses=2,
        false_only_addresses=1, unavailable_addresses=0, no_observations_addresses=0)
    stop = json.loads((output / 'trace.jsonl').read_text().splitlines()[-1])
    assert stop['registers'][0][3] == 0
    assert stop['registers'][0][4] == 123


def test_replay_determinism_breakpoints_and_limits(tmp_path):
    data = bytearray(0x40000)
    struct.pack_into('<I', data, 0, 0x00800020)  # local L2 alias
    struct.pack_into('<II', data, 0x20, (3 << 23) | (123 << 7) | 0x28, 0xffffffff)
    dump = tmp_path / 'dump.bin'
    dump.write_bytes(data)
    traces = []
    for name in ('first', 'second'):
        output = tmp_path / name
        result = run(dump, output)
        assert result.returncode == 0, result.stderr
        traces.append((output / 'trace.jsonl').read_bytes())
    assert traces[0] == traces[1]
    stop = json.loads(traces[0].splitlines()[-1])
    assert stop['reason'] == 'fault' and stop['fault_word'] == 0xffffffff
    assert stop['packets'] == 1 and stop['registers'][0][3] == 123
    for args, reason in [(('--steps', '1'), 'step_limit'),
                         (('--packets', '1'), 'packet_limit'),
                         (('--cycles', '1'), 'cycle_limit'),
                         (('--break-pc', '0x00800024'), 'breakpoint')]:
        result = run(dump, tmp_path / reason, *args)
        assert result.returncode == 0, result.stderr
        stop = json.loads(result.stdout)
        assert stop['reason'] == reason and stop['fault'] == ''
        assert stop['pc'] == 0x00800024 and stop['packets'] == 1
    manifest = json.loads((tmp_path / 'first/manifest.json').read_text())
    assert manifest['boot_rom_executed'] is False and len(manifest['dump_sha256']) == 64
    assert 'emulator/qemu/cdj_c674x.h' in manifest['sources']
    assert manifest['coverage']['counts']['confirmed_source_packets'] == 1
    assert manifest['complete'] and manifest['progress']['packet_delta'] == 1
    assert manifest['progress']['cycle_delta'] == 1
    assert manifest['limits']['packets'] == 0 and manifest['limits']['cycles'] == 0
    assert manifest['approximations'] == []
    assert manifest['output_checkpoint']['file'] == 'final.cdjdsp'
    assert (tmp_path / 'first/coverage.json').is_file()
    failure = json.loads((tmp_path / 'first/failure.json').read_text())
    assert failure['outcome'] == 'fail_closed_fault'
    assert failure['distinct_unsupported_encodings'] == []
    assert failure['distinct_fault_encodings'] == [{
        'word': 0xffffffff, 'pc': 0x11800024,
        'reason': 'reserved predicate', 'width': None,
        'width_limitation': 'fault latch does not retain compact/full width',
    }]
    assert failure['resumable_checkpoint']['checkpoint_sha256'] == \
        manifest['output_checkpoint']['checkpoint_sha256']
    # Even a single diagnostic run is resumable with explicit (non-repeat)
    # provenance; this does not claim deterministic equivalence.
    resumed = tmp_path / 'resumed'
    result = run(tmp_path / 'first/final.cdjdsp', resumed, '--instructions', '1')
    assert result.returncode == 0, result.stderr
    assert json.loads((resumed / 'manifest.json').read_text())['input_kind'] == \
        'diagnostic_replay_checkpoint'
    for reason in ('packet_limit', 'cycle_limit', 'breakpoint'):
        assert not (tmp_path / reason / 'failure.json').exists()


def test_explicit_strict_resume_preserves_exploratory_ancestry(tmp_path):
    data = bytearray(0x40000)
    struct.pack_into('<I', data, 0, 0x00800020)
    dump = tmp_path / 'nop.bin'
    dump.write_bytes(data)
    first = tmp_path / 'exploratory'
    result = run(dump, first, '--steps', '1', '--functional-dsp-timing',
                 '--verify-repeat')
    assert result.returncode == 0, result.stderr
    for name in ('strict_resume', 'strict_again'):
        output = tmp_path / name
        result = run(first / 'final.cdjdsp', output, '--steps', '1', '--verify-repeat')
        assert result.returncode == 0, result.stderr
        manifest = json.loads((output / 'manifest.json').read_text())
        gate = json.loads((output / 'gate.json').read_text())
        assert manifest['dsp_timing_mode'] == 'strict'
        assert manifest['inherited_exploratory_state']
        assert not manifest['architectural_validation_eligible']
        assert not gate['architectural_validation_eligible']
        assert not gate['coverage_validation_eligible']
        assert gate['passed']
        assert any('SPLOOPD' in item for item in manifest['approximations'])
        first = output
    result = run(first, tmp_path / 'automatic_strict', '--steps', '1')
    assert result.returncode != 0
    assert 'inherits exploratory state' in result.stderr


def test_replay_rejects_invalid_input_without_artifacts(tmp_path):
    dump = tmp_path / 'short.bin'
    dump.write_bytes(b'bad')
    output = tmp_path / 'output'
    result = run(dump, output)
    assert result.returncode != 0 and '256 KiB' in result.stderr
    assert not output.exists()


def test_replay_gate_preserves_faults_and_rejects_changed_baseline(tmp_path):
    data = bytearray(0x40000)
    struct.pack_into('<I', data, 0, 0x11800020)
    struct.pack_into('<I', data, 0x20, 0xffffffff)
    dump = tmp_path / 'dump.bin'
    dump.write_bytes(data)
    first = tmp_path / 'first'
    result = run(dump, first, '--verify-repeat')
    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout)['reason'] == 'fault'
    gate = json.loads((first / 'gate.json').read_text())
    assert gate['passed'] and gate['repeat_matches']
    assert gate['repeat_coverage_matches']
    assert not gate['coverage_validation_eligible']
    assert gate['trace_sha256'] == gate['repeat_sha256']
    assert 'not architectural correctness or boot' in gate['scope']
    assert gate['limits']['steps'] == 10000 and gate['approximations'] == []
    # An exactly repeated final checkpoint is a provenance-bearing resume
    # point; normal iteration must not fall back to a connected checkpoint.
    chained = tmp_path / 'chained'
    result = run(first / 'final.cdjdsp', chained, '--verify-repeat')
    assert result.returncode == 0, result.stderr
    chained_manifest = json.loads((chained / 'manifest.json').read_text())
    assert chained_manifest['input_kind'] == 'deterministic_replay_checkpoint'
    assert chained_manifest['input_checkpoint']['checkpoint_sha256'] == \
        gate['final_checkpoint']['checkpoint_sha256']
    assert chained_manifest['input_manifest_sha256'] and \
        chained_manifest['input_gate_sha256']
    # A directory input selects the newest provenance-valid checkpoint and
    # ignores a newer corrupt artifact instead of requiring manual filename selection.
    (first / 'newer-invalid.cdjdsp').write_bytes(b'not a checkpoint')
    newest = tmp_path / 'newest'
    result = run(first, newest, '--steps', '1', '--verify-repeat')
    assert result.returncode == 0, result.stderr
    newest_manifest = json.loads((newest / 'manifest.json').read_text())
    assert newest_manifest['dump_path'] == str((first / 'final.cdjdsp').resolve())
    assert 'Selected newest compatible checkpoint' in result.stderr
    baseline = first / 'trace.jsonl'
    second = tmp_path / 'second'
    result = run(dump, second, '--verify-repeat', '--expect-trace', str(baseline))
    assert result.returncode == 0, result.stderr
    assert json.loads((second / 'gate.json').read_text())['expected_matches']
    # Same deterministic fault, different diagnostic trace: gate must fail,
    # preserving artifacts rather than quietly updating the baseline.
    changed = tmp_path / 'changed.jsonl'
    changed.write_bytes(baseline.read_bytes() + b'\n')
    third = tmp_path / 'third'
    result = run(dump, third, '--verify-repeat', '--expect-trace', str(changed))
    assert result.returncode == 1
    gate = json.loads((third / 'gate.json').read_text())
    assert gate['repeat_matches'] and not gate['expected_matches'] and not gate['passed']
    assert json.loads(result.stdout)['reason'] == 'fault'
    missing = tmp_path / 'missing'
    result = run(dump, missing, '--expect-trace', str(tmp_path / 'absent'))
    assert result.returncode != 0 and not missing.exists()


def test_transmit_capture_metadata_and_repeat_gate(tmp_path):
    capture = tmp_path / 'capture.jsonl'
    capture.write_text(
        '{"sequence":1,"instance":1,"slot":0,"serializer":0,"word":305419896,'
        '"xbuf_sequence":7,"packets":1024,"cycles":2048,'
        '"source":"genuine_xbuf","clock":"functional-coarse-packet-slot"}\n')
    metadata = tx_capture_metadata(capture)
    assert metadata['records'] == 1
    assert metadata['counts'] == {'mcasp1.serializer0': 1}
    assert metadata['synthesized_samples'] is False
    empty = tmp_path / 'empty.jsonl'
    empty.write_bytes(b'')
    assert tx_capture_metadata(empty)['records'] == 0
    truncated = tmp_path / 'truncated.jsonl'
    truncated.write_bytes(capture.read_bytes()[:-1])
    with pytest.raises((ValueError, json.JSONDecodeError)):
        tx_capture_metadata(truncated)
    malformed = tmp_path / 'malformed.jsonl'
    malformed.write_text(capture.read_text().replace('"genuine_xbuf"', '"synthetic"'))
    with pytest.raises(ValueError, match='invalid DSP transmit capture'):
        tx_capture_metadata(malformed)

    data = bytearray(0x40000)
    struct.pack_into('<I', data, 0, 0x00800020)
    struct.pack_into('<I', data, 0x20, 0xffffffff)
    dump = tmp_path / 'dump.bin'
    dump.write_bytes(data)
    output = tmp_path / 'captured-replay'
    result = run(dump, output, '--functional-dsp-audio', '--capture-dsp-tx',
                 '--verify-repeat')
    assert result.returncode == 0, result.stderr
    manifest = json.loads((output / 'manifest.json').read_text())
    gate = json.loads((output / 'gate.json').read_text())
    assert manifest['dsp_tx_capture']['records'] == 0
    assert manifest['dsp_tx_capture']['synthesized_samples'] is False
    assert gate['dsp_tx_capture_matches'] and gate['passed']
    assert (output / 'dsp-tx.jsonl').read_bytes() == \
        (output / 'repeat-dsp-tx.jsonl').read_bytes()

    rejected = run(dump, tmp_path / 'invalid-capture', '--capture-dsp-tx')
    assert rejected.returncode != 0 and 'requires --functional-dsp-audio' in rejected.stderr
