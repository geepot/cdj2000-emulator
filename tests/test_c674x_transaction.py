"""Byte-exact differential gate against the original full-prefix transaction."""
from pathlib import Path
import re
import shutil
import subprocess
import pytest
ROOT = Path(__file__).resolve().parents[1]
HELPERS = ('uncond', 'mpy', 'dotp', 'packed8', 'packed16', 'packbits',
           'mpy32', 'dp', 'approx', 'sp', 'control', 'loop')

@pytest.mark.parametrize('sanitize', [False, True])
def test_transaction_matches_full_prefix(tmp_path, sanitize):
    cc = shutil.which('cc')
    if not cc:
        pytest.skip('requires C compiler')
    # The reference shares ISA execution but runs every packet against a
    # zero-initialized scratch copy of the whole prefix and commits it whole,
    # the original transaction; the candidate executes in place with rollback.
    source = (ROOT / 'emulator/qemu/cdj_c674x.c').read_text()
    reference = source.replace('CdjC674x out CDJ_C674X_UNINITIALIZED;', 'CdjC674x out;')
    assert reference != source
    ref = tmp_path/'reference.c'; ref.write_text(reference)
    results = []
    for label, core in [('reference', ref), ('candidate', ROOT/'emulator/qemu/cdj_c674x.c')]:
        binary = tmp_path/label
        flags = ['-std=c11','-O2','-Wall','-Wextra','-Werror']
        probe = tmp_path/'compiler-probe'
        initialized = subprocess.run([cc, '-Werror', '-ftrivial-auto-var-init=zero',
                                      '-x', 'c', '-', '-o', str(probe), '-lm'],
                                     input='int main(void) { return 0; }',
                                     text=True, capture_output=True)
        if initialized.returncode == 0:
            flags += ['-ftrivial-auto-var-init=zero']
        if sanitize:
            flags += ['-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer']
            supported = subprocess.run([cc, *flags, '-x', 'c', '-', '-o', str(probe), '-lm'],
                                       input='int main(void) { return 0; }',
                                       text=True, capture_output=True)
            if supported.returncode:
                pytest.skip('compiler requires ASan/UBSan support')
            if subprocess.run([str(probe)], capture_output=True).returncode:
                pytest.skip('requires a working ASan/UBSan runtime')
        if label == 'reference':
            flags += ['-DCDJ_C674X_COPY_TRANSACTIONS']
        subprocess.run([cc,*flags,'-I',str(ROOT/'emulator/qemu'),
                        str(ROOT/'tests/cstub/c674x-transaction.c'),str(core),
                        *(str(ROOT/'emulator/qemu'/f'cdj_c674x_{name}.c') for name in HELPERS),
                        '-o',str(binary), '-lm'],check=True)
        results.append(subprocess.run([str(binary)],check=True,capture_output=True,timeout=20).stdout)
    assert results[0] == results[1]
