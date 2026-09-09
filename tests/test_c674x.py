"""Compile and execute architecture-level tests without proprietary firmware."""
from pathlib import Path
import shutil
import subprocess
import pytest
ROOT = Path(__file__).resolve().parents[1]

def test_c674x_packets_and_branch_delays(tmp_path):
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'c674x-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c674x.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_loop.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)


def test_c674x_loop_schedule(tmp_path):
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'c674x-loop-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c674x-loop.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_loop.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)


def test_c6747_syscfg_unlock_and_pipeline(tmp_path):
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'c6747-syscfg-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c6747-syscfg.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_syscfg.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_loop.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)
