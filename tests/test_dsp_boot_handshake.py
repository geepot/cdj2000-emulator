"""Read-only DSP readiness transcript evidence, never a boot oracle."""
import pytest

from tools.cdj_dsp.boot_handshake import analyze_events


READY = 0x1183FFF4
CLEAR = 0x1183FFEC
ACK = 0x1183FFF0
FIXED = 0xC0000
AUTO = 0x80000


def event(sequence, kind, *, offset=0, address=0, value=0, size=0,
          packets=None, cycles=None):
    return dict(sequence=sequence, event=kind, offset=offset,
                address=address, value=value, size=size,
                packets=sequence if packets is None else packets,
                cycles=2 * sequence if cycles is None else cycles)


def ready(sequence, value=1, **fields):
    return event(sequence, 'hpi_host_data_read', offset=FIXED,
                 address=READY, value=value, size=4, **fields)


def write(sequence, address, value, *, autoincrement=False, **fields):
    return event(sequence, ('hpi_host_data_autoincrement_write'
                            if autoincrement else 'hpi_host_data_fixed_write'),
                 offset=AUTO if autoincrement else FIXED,
                 address=address, value=value, size=4, **fields)


@pytest.mark.parametrize('autoincrement', [False, True])
def test_observes_ordered_ready_clear_ack_in_final_epoch(autoincrement):
    result = analyze_events([
        ready(1, 0),
        ready(2, 1),
        write(3, CLEAR, 0, autoincrement=autoincrement),
        write(4, ACK, 1, autoincrement=autoincrement),
    ])
    assert result == dict(
        ready_reads=2, ready_one_sequence=2, clear_sequence=3,
        acknowledgement_sequence=4, handshake_observed=True,
        reset_epochs=0, events=4)


@pytest.mark.parametrize('events', [
    # A clear before readiness cannot be reused by a later acknowledgement.
    [write(1, CLEAR, 0), ready(2), write(3, ACK, 1)],
    # Acknowledgement must follow the clear, not merely the ready read.
    [ready(1), write(2, ACK, 1), write(3, CLEAR, 0)],
    [ready(1, 0), write(2, CLEAR, 0), write(3, ACK, 1)],
    [ready(1), write(2, CLEAR, 1), write(3, ACK, 1)],
    [ready(1), write(2, CLEAR, 0), write(3, ACK, 0)],
    # A later non-one readiness observation invalidates the earlier candidate.
    [ready(1), write(2, CLEAR, 0), ready(3, 0), write(4, ACK, 1)],
])
def test_wrong_value_or_order_is_not_a_handshake(events):
    result = analyze_events(events)
    assert not result['handshake_observed']
    assert result['acknowledgement_sequence'] is None


def test_reset_invalidates_old_ready_and_clear_candidate():
    result = analyze_events([
        ready(1),
        write(2, CLEAR, 0),
        event(3, 'reset_assert', packets=0, cycles=0),
        write(4, ACK, 1, packets=0, cycles=0),
    ])
    assert result['ready_reads'] == 1
    assert result['reset_epochs'] == 1
    assert result['ready_one_sequence'] is None
    assert result['clear_sequence'] is None
    assert result['acknowledgement_sequence'] is None
    assert not result['handshake_observed']


def test_dsp_start_invalidates_a_stale_prestart_candidate():
    result = analyze_events([
        ready(1),
        write(2, CLEAR, 0),
        event(3, 'dsp_start', packets=0, cycles=0),
        write(4, ACK, 1, packets=0, cycles=0),
    ])
    assert result['reset_epochs'] == 0
    assert result['ready_one_sequence'] is None
    assert result['clear_sequence'] is None
    assert result['acknowledgement_sequence'] is None
    assert not result['handshake_observed']


@pytest.mark.parametrize('sequences', [(2,), (1, 3), (1, 1)])
def test_sequence_must_start_at_one_and_remain_contiguous(sequences):
    with pytest.raises(ValueError, match='contiguous'):
        analyze_events(event(sequence, 'unrelated') for sequence in sequences)


def test_empty_transcript_has_no_handshake_evidence():
    assert analyze_events([]) == dict(
        ready_reads=0, ready_one_sequence=None, clear_sequence=None,
        acknowledgement_sequence=None, handshake_observed=False,
        reset_epochs=0, events=0)


def test_address_selection_and_host_write_do_not_prove_dsp_ready():
    result = analyze_events([
        event(1, 'hpi_host_address_write', offset=0x40000,
              address=0, value=READY, size=4),
        write(2, READY, 1),
        write(3, CLEAR, 0),
        write(4, ACK, 1),
    ])
    assert result['ready_reads'] == 0
    assert result['ready_one_sequence'] is None
    assert not result['handshake_observed']


@pytest.mark.parametrize('mutate,match', [
    (lambda row: row.pop('address'), 'address'),
    (lambda row: row.__setitem__('sequence', True), 'sequence'),
    (lambda row: row.__setitem__('value', '1'), 'value'),
    (lambda row: row.__setitem__('packets', -1), 'packets'),
    (lambda row: row.__setitem__('cycles', False), 'cycles'),
    (lambda row: row.__setitem__('offset', 2**64), 'offset'),
    (lambda row: row.__setitem__('size', 2), 'ready read shape'),
    (lambda row: row.__setitem__('offset', 4), 'ready read shape'),
])
def test_malformed_ready_event_fields_are_rejected(mutate, match):
    row = ready(1)
    mutate(row)
    with pytest.raises(ValueError, match=match):
        analyze_events([row])


@pytest.mark.parametrize('autoincrement', [False, True])
def test_handshake_write_requires_the_matching_hpi_port_shape(autoincrement):
    row = write(1, CLEAR, 0, autoincrement=autoincrement)
    row['offset'] = FIXED if autoincrement else AUTO
    with pytest.raises(ValueError, match='handshake write shape'):
        analyze_events([row])


@pytest.mark.parametrize('field', ['packets', 'cycles'])
def test_counters_cannot_move_backwards_within_an_epoch(field):
    first = event(1, 'unrelated', packets=10, cycles=20)
    second = event(2, 'unrelated', packets=10, cycles=20)
    second[field] -= 1
    with pytest.raises(ValueError, match='moved backwards'):
        analyze_events([first, second])


@pytest.mark.parametrize('boundary', ['reset_assert', 'dsp_start'])
def test_reset_and_dsp_start_begin_a_new_counter_epoch(boundary):
    result = analyze_events([
        event(1, 'unrelated', packets=10, cycles=20),
        event(2, boundary, packets=0, cycles=0),
        ready(3, packets=0, cycles=0),
        write(4, CLEAR, 0, packets=0, cycles=0),
        write(5, ACK, 1, packets=0, cycles=0),
    ])
    assert result['handshake_observed']
    assert result['reset_epochs'] == (1 if boundary == 'reset_assert' else 0)


@pytest.mark.parametrize('bad', [None, [], 'event'])
def test_non_object_events_are_rejected(bad):
    with pytest.raises(ValueError, match='object'):
        analyze_events([bad])
