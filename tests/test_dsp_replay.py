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
                         (('--break-pc', '0x00800024'), 'breakpoint')]:
        result = run(dump, tmp_path / reason, *args)
        assert result.returncode == 0, result.stderr
        stop = json.loads(result.stdout)
        assert stop['reason'] == reason and stop['fault'] == ''
        assert stop['pc'] == 0x00800024 and stop['packets'] == 1
    manifest = json.loads((tmp_path / 'first/manifest.json').read_text())
    assert manifest['boot_rom_executed'] is False and len(manifest['dump_sha256']) == 64
    assert 'emulator/qemu/cdj_c674x.h' in manifest['sources']


def test_replay_rejects_invalid_input_without_artifacts(tmp_path):
    dump = tmp_path / 'short.bin'
    dump.write_bytes(b'bad')
    output = tmp_path / 'output'
    result = run(dump, output)
    assert result.returncode != 0 and '256 KiB' in result.stderr
    assert not output.exists()
