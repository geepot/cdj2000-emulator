"""The replay tool must distinguish faults, limits and pre-execution stops."""
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def run(dump, output, *args):
    return subprocess.run([sys.executable, '-m', 'tools.cdj_dsp.replay',
                           str(dump), str(output), *args], cwd=ROOT,
                          text=True, capture_output=True, timeout=20)


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
    assert failure['distinct_unsupported_encodings'] == [{
        'word': 0xffffffff, 'pc': 0x11800024,
        'reason': 'reserved predicate', 'width': None,
        'width_limitation': 'fault latch does not retain compact/full width',
    }]
    assert failure['distinct_fault_encodings'] == \
        failure['distinct_unsupported_encodings']
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
