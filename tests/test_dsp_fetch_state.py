"""Coverage fetch must be independent of registers, pipelines and loop state."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_fetch_requires_only_pc_and_fault(tmp_path):
    source = tmp_path / 'fetch.c'
    source.write_text(r'''
#include <assert.h>
#include <string.h>
#include "cdj_c674x.h"
static uint32_t memory[24];
static unsigned reads;
static bool read_word(void *opaque, uint32_t address, uint32_t *value) {
    (void)opaque;
    ++reads;
    if (address >= sizeof(memory)) return false;
    *value = memory[address / 4];
    return true;
}
int main(void) {
    for (unsigned mode=0; mode<4; ++mode) {
        for (unsigned i=0; i<24; ++i)
            memory[i] = mode == 1 ? 1 : 0;
        if (mode == 2) memory[7] = memory[15] = memory[23] = 0xefffffff;
        for (unsigned pc=0; pc<=100; ++pc) {
            CdjC674x full, sparse;
            memset(&full, 0, sizeof(full));
            memset(&sparse, 1, sizeof(sparse));
            full.pc = sparse.pc = pc;
            full.fault = sparse.fault = mode == 3 ? "prior fault" : NULL;
            full.fault_pc = sparse.fault_pc = 123;
            full.fault_word = sparse.fault_word = 456;
            CdjC674xPacket a = {0}, b = {0};
            reads = 0;
            bool accepted = cdj_c674x_fetch(&full, read_word, NULL, &a);
            unsigned count = reads;
            reads = 0;
            assert(accepted == cdj_c674x_fetch(&sparse, read_word, NULL, &b));
            assert(count == reads);
            assert(full.pc == sparse.pc);
            assert(full.fault_pc == sparse.fault_pc);
            assert(full.fault_word == sparse.fault_word);
            assert((full.fault == NULL) == (sparse.fault == NULL));
            if (full.fault) assert(!strcmp(full.fault, sparse.fault));
            assert(a.count == b.count && a.next_pc == b.next_pc);
            for (unsigned i=0; i<a.count; ++i) {
                assert(a.instructions[i].pc == b.instructions[i].pc);
                assert(a.instructions[i].word == b.instructions[i].word);
                assert(a.instructions[i].header == b.instructions[i].header);
                assert(a.instructions[i].compact == b.instructions[i].compact);
            }
        }
    }
}
''')
    binary = tmp_path / 'fetch'
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I', str(ROOT / 'emulator/qemu'), str(source),
                    str(ROOT / 'emulator/qemu/cdj_c674x.c'),
                    str(ROOT / 'emulator/qemu/cdj_c674x_loop.c'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
