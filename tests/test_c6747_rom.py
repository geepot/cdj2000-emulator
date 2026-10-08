"""C6747 L2 ROM: loader, hash pin, read-only mapping, and the table builder."""
import hashlib
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_c6747_rom_loader(tmp_path):
    cc = shutil.which('cc')
    if not cc:
        pytest.skip('requires C compiler')
    binary = tmp_path / 'c6747-rom'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(ROOT / 'emulator/qemu'), str(ROOT / 'tests/cstub/c6747-rom.c'),
                    str(ROOT / 'emulator/qemu/cdj_c6747_rom.c'), '-o', str(binary)],
                   check=True)
    subprocess.run([str(binary), str(tmp_path)], check=True, timeout=30)


def test_built_image_matches_the_pinned_hash():
    """Skips without the user's TI download (never in the repository)."""
    from tools.cdj_dsp import build_dsp_rom
    if not build_dsp_rom.DEFAULT_OBJ.is_dir():
        pytest.skip('needs the TI MP4AACDEC 1.01 objects (~/.cache/dspdec/ti-x/lib/heaac)')
    image = build_dsp_rom.build(build_dsp_rom.DEFAULT_OBJ)
    assert len(image) == 0x100000
    pinned = (ROOT / 'emulator/qemu/cdj_c6747_rom.c').read_text()
    assert hashlib.sha256(image).hexdigest() in pinned
    # spot checks: a table lands at its stock address (crcTable, last in the ROM)
    assert any(image[0xefc70:0xefc70 + 16])


def test_no_ti_data_is_tracked():
    tracked = subprocess.run(['git', 'ls-files', 'build'], cwd=ROOT, text=True,
                             capture_output=True, check=True).stdout
    assert not tracked.strip()
