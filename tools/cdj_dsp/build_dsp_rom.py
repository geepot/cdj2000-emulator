# SPDX-License-Identifier: GPL-2.0-or-later
"""Build a C6747 DSP L2 ROM table image from the user's own TI codec download.

Stock's AAC decoder (TI MP4AACDEC_TIJ) reads constant tables from the DSP's
1 MiB L2 ROM at 0x11700000.  We do not have the ROM, so this lays down the
tables the decoder references, taken from TI's MP4AACDEC 1.01 objects (same
lineage; the rest of 1.01's tables match stock byte for byte), at the ROM
addresses stock itself uses.  Everything else in the image is zero.  The
image is TI data: it goes to a git-ignored path and is never committed; the
board maps it read-only with --dsp-rom / CDJ_DSP_ROM.

Placement per symbol was recovered from stock's own references
(cdj-2000nxs-sdk docs/dsp-aac.md): tables.obj relocations give the sfb tables,
the HuffDecTable entries at 0x1180CA24.. give NTuples_*, and the MVKL/MVKH
pairs in imdct_filterbank, the CRC routine and codec_byte_table_lookup give
the windows, crcTable and MaximumTnsBandsTable.

usage: build_dsp_rom.py [TI_HEAAC_OBJ_DIR] [-o build/dsp-rom/c6747-l2-rom.bin]
"""
import argparse
import hashlib
import os
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ROM_BASE, ROM_SIZE = 0x11700000, 0x100000
DEFAULT_OBJ = Path('~/.cache/dspdec/ti-x/lib/heaac').expanduser()
DEFAULT_OUT = ROOT / 'build/dsp-rom/c6747-l2-rom.bin'
PLACE = {   # TI symbol -> ROM address
    'MP4AACDEC_TIJ_WinKaiserLong': 0x117ce780, 'MP4AACDEC_TIJ_WinSinusLong': 0x117cf780,
    'MP4AACDEC_TIJ_WinKaiserShort': 0x117d1b58, 'MP4AACDEC_TIJ_WinSinusShort': 0x117d1958,
    'MP4AACDEC_TIJ_NTuples_1': 0x117d21d8, 'MP4AACDEC_TIJ_NTuples_3': 0x117d2468,
    'MP4AACDEC_TIJ_NTuples_5': 0x117d2ba0, 'MP4AACDEC_TIJ_NTuples_7': 0x117d3000,
    'MP4AACDEC_TIJ_NTuples_9': 0x117d2080, 'MP4AACDEC_TIJ_NTuples_11': 0x117d1510,
    'MP4AACDEC_TIJ_sfb_96_1024': 0x117d33e0, 'MP4AACDEC_TIJ_sfb_96_128': 0x117d35e0,
    'MP4AACDEC_TIJ_sfb_64_1024': 0x117d3328, 'MP4AACDEC_TIJ_sfb_48_1024': 0x117d3260,
    'MP4AACDEC_TIJ_sfb_48_128': 0x117d35a0, 'MP4AACDEC_TIJ_sfb_32_1024': 0x117d31f8,
    'MP4AACDEC_TIJ_sfb_24_1024': 0x117d32c8, 'MP4AACDEC_TIJ_sfb_24_128': 0x117d3560,
    'MP4AACDEC_TIJ_sfb_16_1024': 0x117d3388, 'MP4AACDEC_TIJ_sfb_16_128': 0x117d3540,
    'MP4AACDEC_TIJ_sfb_8_1024': 0x117d3438, 'MP4AACDEC_TIJ_sfb_8_128': 0x117d3580,
    'MP4AACDEC_TIJ_MaximumTnsBandsTable': 0x117d3600, 'MP4AACDEC_TIJ_crcTable': 0x117efc70,
}


def symbols(obj_dir):
    """Section bytes of every PLACE symbol in the ELF objects of obj_dir (a
    symbol runs to the next symbol of its section)."""
    out = {}
    for f in sorted(os.listdir(obj_dir)):
        b = (Path(obj_dir) / f).read_bytes()
        if b[:4] != b'\x7fELF':
            continue
        shoff, = struct.unpack_from('<I', b, 0x20)
        es, n, _ = struct.unpack_from('<HHH', b, 0x2e)
        sh = [struct.unpack_from('<IIIIIIIIII', b, shoff + i * es) for i in range(n)]
        for s in sh:
            if s[1] != 2:
                continue
            st = sh[s[6]]
            ents = [struct.unpack_from('<IIIBBH', b, o) for o in range(s[4], s[4] + s[5], 16)]
            for nm, val, sz, info, oth, shn in ents:
                name = b[st[4] + nm:b.index(b'\0', st[4] + nm)].decode()
                if name in PLACE and 0 < shn < n and sh[shn][1] == 1:
                    sec = sh[shn]
                    nxt = min([v for _, v, _, _, _, x in ents if x == shn and v > val] + [sec[5]])
                    out[name] = b[sec[4] + val:sec[4] + nxt]
    return out


def build(obj_dir):
    syms = symbols(obj_dir)
    missing = sorted(set(PLACE) - set(syms))
    if missing:
        raise SystemExit(f'missing in {obj_dir}: {missing}')
    image = bytearray(ROM_SIZE)
    used = []
    for name, address in sorted(PLACE.items(), key=lambda kv: kv[1]):
        data, start = syms[name], address - ROM_BASE
        if not (0 <= start and start + len(data) <= ROM_SIZE):
            raise SystemExit(f'{name} outside the ROM')
        if used and start < used[-1][1]:
            raise SystemExit(f'{name} overlaps {used[-1][2]}')
        image[start:start + len(data)] = data
        used.append((start, start + len(data), name))
    return bytes(image)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('objects', nargs='?', type=Path, default=DEFAULT_OBJ,
                    help='directory of TI MP4AACDEC 1.01 (HE-AAC) ELF objects')
    ap.add_argument('-o', '--output', type=Path, default=DEFAULT_OUT)
    args = ap.parse_args()
    image = build(args.objects.expanduser())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    print(f'{args.output}: {len(image)} bytes, {len(PLACE)} tables, '
          f'sha256 {hashlib.sha256(image).hexdigest()}')


if __name__ == '__main__':
    sys.exit(main())
