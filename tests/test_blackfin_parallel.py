"""Execute a parallel instruction packet in the built Blackfin simulator."""
import os
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_parallel_store_reads_old_register_value(tmp_path):
    assembler = os.environ.get('BFIN_AS') or shutil.which('bfin-elf-as')
    linker = os.environ.get('BFIN_LD') or shutil.which('bfin-elf-ld')
    simulator = ROOT / 'bin' / ('cdj-run.exe' if os.name == 'nt' else 'cdj-run')
    if not assembler or not linker or not simulator.is_file():
        pytest.skip('requires built Blackfin simulator and BFIN_AS/BFIN_LD tools')
    obj, elf = tmp_path / 'parallel.o', tmp_path / 'parallel.elf'
    subprocess.run([assembler, str(ROOT / 'tests/bfin/parallel-store.s'), '-o', str(obj)], check=True)
    subprocess.run([linker, '-Ttext=0x1000', '-e', '_start', str(obj), '-o', str(elf)], check=True)
    result = subprocess.run(
        [str(simulator), '--environment', 'user', '--memory-region', '0,64M', str(elf)],
        env=dict(os.environ, BFIN_PARALLEL_WRITEBACK='1'),
        capture_output=True, text=True, timeout=10,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert 'FAIL' not in result.stdout + result.stderr
