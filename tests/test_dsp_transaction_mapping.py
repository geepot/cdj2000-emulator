"""Map prefilters must never reject an accepted device write."""
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_transaction_prefilters_cover_registers_and_exclude_ram(tmp_path):
    cc = shutil.which('cc')
    if not cc:
        pytest.skip('requires C compiler')
    source = tmp_path / 'mapping.c'
    source.write_text('''
#include <assert.h>
#include "cdj_c6747_edma.h"
#include "cdj_c6747_mcasp.h"
int main(void) {
    CdjC6747Edma edma;
    CdjC6747McaspControl mcasp = {0};
    cdj_c6747_edma_reset(&edma);
    for (uint32_t a=0x01bffff8; a<0x01c08008; a++)
        for (unsigned size=0; size<=8; size++) {
            bool mapped = cdj_c6747_edma_write_mapped(a,size);
            assert(mapped == ((size==4 || size==8) && !(a&3) &&
                             a>=0x01c00000 && a<0x01c08000));
            if (cdj_c6747_edma_write(&edma,a,0,size,false,0)) assert(mapped);
        }
    for (uint32_t a=0x01cffff8; a<0x01d0c008; a++)
        for (unsigned size=0; size<=8; size++) {
            bool mapped = cdj_c6747_mcasp_control_write_mapped(a,size);
            assert(mapped == (size==4 && !(a&3) && a>=0x01d00000 && a<0x01d0c000));
            if (cdj_c6747_mcasp_control_write(&mcasp,a,0,size,false)) assert(mapped);
        }
    uint32_t ram[] = {0x00800000,0x11800000,0x80000000,0xc0000000,0xffffffff};
    for (unsigned i=0;i<sizeof(ram)/sizeof(ram[0]);i++)
        for (unsigned size=0;size<=8;size++) {
            assert(!cdj_c6747_edma_write_mapped(ram[i],size));
            assert(!cdj_c6747_mcasp_control_write_mapped(ram[i],size));
        }
    return 0;
}
''')
    binary = tmp_path / 'mapping'
    subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I', str(ROOT / 'emulator/qemu'), str(source),
                    str(ROOT / 'emulator/qemu/cdj_c6747_edma.c'),
                    str(ROOT / 'emulator/qemu/cdj_c6747_mcasp.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
