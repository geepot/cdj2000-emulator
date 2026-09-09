"""Exact-address decoding must preserve evidence boundaries and provenance."""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
from types import SimpleNamespace

import pytest

from tools.cdj_dsp.semantic_inventory import assembly_fields, build_inventory, parse_disassembly


@pytest.mark.parametrize('text,expected', [
    ('|| [!b0] ldw .D1T2 *+a4[3],b6', ('ldw', '.D1T2', '*+a4[3],b6', '!b0')),
    ('spkernel 3,0', ('spkernel', None, '3,0', None)),
    ('[a0] bnop .S1 0x1180254c,5', ('bnop', '.S1', '0x1180254c,5', 'a0')),
    ('mv .L2X a3,b4', ('mv', '.L2X', 'a3,b4', None)),
    ('<fetch packet header 0xee000300>', (None, None, None, None)),
    ('.word 0xffffffff', (None, None, None, None)),
])
def test_operand_text_and_unresolved_disassembly(text, expected):
    result = assembly_fields(text)
    assert tuple(result[key] for key in ('mnemonic', 'unit', 'operands', 'predicate')) == expected


@pytest.mark.parametrize('text', [
    '', '0x11800020:\tmvk .L1 1,a1\t2\n',
    '0x11800024:\tmvk .L1 1,a1\t4\n',
    '0x11800020:\tmvk .L1 1,a1\t4\n' * 2,
    'malformed\n',
])
def test_decoder_cannot_omit_duplicate_or_change_requested_addresses(text):
    with pytest.raises(ValueError):
        parse_disassembly(text, {0x11800020: dict(width=32)})


def fixture(tmp_path):
    image = bytearray(0x40000)
    word = (3 << 23) | (123 << 7) | 0x28
    struct.pack_into('<I', image, 0x20, word)
    checkpoint = tmp_path / 'image.bin'
    checkpoint.write_bytes(image)
    coverage = tmp_path / 'coverage.json'
    coverage.write_text(json.dumps(dict(
        sha256=dict(checkpoint=hashlib.sha256(image).hexdigest()),
        confirmed_instructions=[dict(pc=0x11800020, word=word, width=32,
            header=0, compact=False, source_fetches=7, source_predicate_outcomes=[False])],
        probable_code=[dict(pc=0x11800024)])))
    decoder = tmp_path / 'decoder'
    decoder.write_bytes(b'identity-for-mocked-executable')
    return checkpoint, coverage, decoder


def test_inventory_only_requests_confirmed_addresses_and_retains_aliases(tmp_path, monkeypatch):
    checkpoint, coverage, decoder = fixture(tmp_path)
    calls = []
    def run(command, **kwargs):
        calls.append(command)
        assert command[0] == str(decoder) and command[-1] == '--stdin'
        assert Path(command[1]).read_bytes() == checkpoint.read_bytes()
        assert kwargs['input'] == '0x11800020\n'
        return SimpleNamespace(stdout='0x11800020:\tmvk .S1 123,a3\t4\n')
    monkeypatch.setattr('tools.cdj_dsp.semantic_inventory.subprocess.run', run)
    report = build_inventory(checkpoint, coverage, decoder)
    assert len(calls) == 1
    assert not report['validation_eligible']
    assert report['counts'] == dict(instruction_addresses=1, groups=1, mnemonics=1, unresolved_addresses=0)
    assert report['groups'][0]['false_only_addresses'] == 1
    assert report['groups'][0]['source_fetch_observations'] == 7
    assert report['instructions'][0]['operands'] == '123,a3'


@pytest.mark.parametrize('corruption', ['hash', 'word', 'header', 'duplicate', 'outside'])
def test_snapshot_or_coverage_mismatch_is_rejected_before_decoding(tmp_path, monkeypatch, corruption):
    checkpoint, coverage, decoder = fixture(tmp_path)
    data = json.loads(coverage.read_text())
    if corruption == 'hash': data['sha256']['checkpoint'] = '0' * 64
    elif corruption == 'word': data['confirmed_instructions'][0]['word'] ^= 2
    elif corruption == 'header': data['confirmed_instructions'][0]['header'] = 0xe0000000
    elif corruption == 'duplicate': data['confirmed_instructions'] *= 2
    elif corruption == 'outside': data['confirmed_instructions'][0]['pc'] = 0xdeadbeef
    coverage.write_text(json.dumps(data))
    def run(*args, **kwargs):
        pytest.fail('invalid evidence was passed to decoder')
    monkeypatch.setattr('tools.cdj_dsp.semantic_inventory.subprocess.run', run)
    with pytest.raises(ValueError):
        build_inventory(checkpoint, coverage, decoder)


def test_real_gnu_batch_frontend(tmp_path):
    decoder = os.environ.get('C6X_DISASSEMBLER')
    if not decoder:
        pytest.skip('set C6X_DISASSEMBLER to the built tic6x frontend')
    image = bytearray(128)
    struct.pack_into('<I', image, 0x20, (3 << 23) | (123 << 7) | 0x28)
    struct.pack_into('<H', image, 0x40, 0x0866)
    struct.pack_into('<I', image, 0x5c, 0xe0200000)
    path = tmp_path / 'mixed.bin'
    path.write_bytes(image)
    command = [decoder, str(path), '0x11800000', '--stdin']
    result = subprocess.run(command, input='0x11800020\n0x11800040\n',
                            text=True, capture_output=True, check=True)
    rows = parse_disassembly(result.stdout, {0x11800020: dict(width=32),
                                            0x11800040: dict(width=16)})
    assert rows[0x11800020]['mnemonic'] == 'mvk'
    assert rows[0x11800040]['predicate'] == 'a0'
    for invalid in ('0x11800021\n', '0x117ffffe\n', '0x11800080\n', '0x11800020'):
        result = subprocess.run(command, input=invalid, text=True, capture_output=True)
        assert result.returncode != 0
