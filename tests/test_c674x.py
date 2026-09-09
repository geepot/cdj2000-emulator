"""Compile and execute architecture-level tests without proprietary firmware."""
from pathlib import Path
import shutil
import subprocess
import pytest
ROOT = Path(__file__).resolve().parents[1]

def test_c6747_spi_registers(tmp_path):
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'spi-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c6747-spi.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_spi.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)

def test_c6747_cache_registers(tmp_path):
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'cache-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c6747-cache.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_cache.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)

def test_c6747_edma_registers_and_transfers(tmp_path):
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'edma-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c6747-edma.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_edma.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)

def test_c6747_timer64p_registers(tmp_path):
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'timer-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c6747-timer.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_timer.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)

def test_c6747_interrupt_controller(tmp_path):
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'intc-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c6747-intc.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_intc.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)

def test_c6747_emifb_configuration(tmp_path):
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'emifb-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c6747-emifb.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_emifb.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)

def test_c6747_hpi_control(tmp_path):
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'hpi-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c6747-hpi.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_hpi.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)

def test_dsp_checkpoint_round_trip(tmp_path):
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'dsp-checkpoint-test'
    checkpoint = tmp_path / 'state.cdjdsp'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/dsp-checkpoint.c'),
        str(ROOT / 'emulator/qemu/cdj_dsp_checkpoint.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_syscfg.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_intc.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_timer.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_spi.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_cache.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_mcasp.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_edma.c'),
        str(ROOT / 'emulator/qemu/cdj_dsp_scheduler.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary), str(checkpoint)], check=True, timeout=5)

def test_c6747_pll_cycle_clock(tmp_path):
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'pll-clock-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c6747-pll-clock.c'),
        *[str(ROOT / 'emulator/qemu' / name) for name in
          ('cdj_c6747_pll.c', 'cdj_c674x.c', 'cdj_c674x_loop.c')],
        '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)

def test_c6747_pll_configuration(tmp_path):
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'pll-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c6747-pll.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_pll.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)

def test_c6747_i2c_gpio_mode(tmp_path):
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'i2c-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c6747-i2c.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_i2c.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)

def test_c6747_gpio_registers(tmp_path):
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'gpio-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c6747-gpio.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_gpio.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)

def test_c6747_mcasp_pin_registers(tmp_path):
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'mcasp-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c6747-mcasp.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_mcasp.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)

def test_c6747_psc_transitions(tmp_path):
    cc = shutil.which('cc')
    if not cc: pytest.skip('requires C compiler')
    binary = tmp_path / 'psc-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c6747-psc.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_psc.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)

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
        str(ROOT / 'emulator/qemu/cdj_c6747_pll.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_loop.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)
