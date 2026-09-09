import struct

from tools.cdj_dsp.event_replay import checkpoint_start, replay
from tools.cdj_dsp.inventory import CHECKPOINT_HEADER, SHARED_RAM_SIZE, _fnv1a


def event(sequence, kind, *, value=0, address=0, offset=0, size=0,
          hint=False, dspint=False):
    return dict(sequence=sequence, event=kind, value=value, address=address,
                offset=offset, size=size, hint=hint, dspint=dspint)


def test_replays_hpi_upload_and_control_edges():
    l2 = bytearray(0x40000)
    l2[:4] = b'\x78\x56\x34\x12'
    events = [
        event(1, 'rom_hpi_ready', value=1, hint=True),
        event(2, 'hpi_host_control_write', value=0x01050105),
        event(3, 'hpi_host_address_write', value=0x11800000),
        event(4, 'hpi_host_data_autoincrement_write', value=0x12345678,
              address=0x11800000, offset=0x80000, size=4),
        event(5, 'hpi_host_control_write', value=0x01030103, dspint=True),
        event(6, 'dsp_start', address=0x11800000, dspint=True),
        event(7, 'dsp_hpic_write', value=6, hint=True),
        event(8, 'hpi_host_control_write', value=4),
    ]
    start = dict(event_sequence=6, hpi_address=0x11800004, boot_phase=0,
                 words=1, l2=bytes(l2))
    result = replay(events, start)
    assert result['uploaded_words'] == 1
    assert result['hint_acknowledgements'] == 2
    assert result['dsp_hint_edges'] == 1
    assert result['dspint_edges'] == 1
    assert result['chunks'][0]['address'] == 0x11800000


def test_rejects_event_sequence_gap():
    try:
        replay([event(2, 'rom_hpi_ready', hint=True)], {})
    except ValueError as error:
        assert 'sequence gap' in str(error)
    else:
        raise AssertionError('sequence gap accepted')


def test_schema2_start_checkpoint_verifies_checksum_and_l2(tmp_path):
    state = bytearray(100)
    struct.pack_into('<IIQQQ', state, 0, 0x11800004, 2, 7, 19, 3)
    state[36:36 + len(b'DSP start boundary')] = b'DSP start boundary'
    l2 = bytearray(0x40000)
    l2[:4] = b'code'
    payload = state + l2 + bytes(SHARED_RAM_SIZE) + bytes(1024)
    header = CHECKPOINT_HEADER.pack(
        b'CDJDSP2\0', 2, 0x01020304, CHECKPOINT_HEADER.size, len(state),
        *([1] * 9), 0x40000, 0x2000000, 4096, 8192, 0,
        len(payload), _fnv1a(payload),
    )
    path = tmp_path / 'start.cdjdsp'
    path.write_bytes(header + payload)
    result = checkpoint_start(path)
    assert result['schema'] == 2 and result['reason'] == 'DSP start boundary'
    assert result['hpi_address'] == 0x11800004 and result['boot_phase'] == 2
    assert result['l2'][:4] == b'code'
    damaged = bytearray(header + payload)
    damaged[-1] ^= 1
    path.write_bytes(damaged)
    try:
        checkpoint_start(path)
    except ValueError as error:
        assert 'checksum' in str(error)
    else:
        raise AssertionError('damaged schema-2 checkpoint accepted')
