"""Framebuffer observations and binary/input provenance, not firmware boot tests."""
import hashlib
import json
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock

import pytest

from tools.cdj_main import nxs_vm


FRAME = b'P6\n2 1\n255\n' + b'\n# \x00\xff\x80'


def test_sync_profile_collects_commands_before_teardown(tmp_path, monkeypatch):
    monitor = Mock()
    monitor.__enter__ = Mock(return_value=monitor)
    monitor.__exit__ = Mock(return_value=False)
    monitor.recv.side_effect = [b'QEMU\r\n(qe', b'mu) ',
                               b'total waits\r\n(qemu) ', b'mean waits\r\n(qemu) ']
    factory = Mock(return_value=monitor)
    monkeypatch.setattr(nxs_vm.socket, 'socket', factory)
    process = Mock()
    process.poll.return_value = None
    result = nxs_vm.capture_sync_profile(tmp_path, process)
    assert result['status'] == 'captured'
    assert [call.args[0] for call in monitor.sendall.call_args_list] == [
        b'info sync-profile -n 30\n', b'info sync-profile -m -n 30\n']
    data = (tmp_path / result['file']).read_bytes()
    assert result['sha256'] == hashlib.sha256(data).hexdigest()
    process.terminate.assert_not_called()


@pytest.mark.parametrize('failure', ['closed', 'timeout', 'missing'])
def test_sync_profile_capture_failure_is_nonfatal(tmp_path, monkeypatch, failure):
    monitor = Mock()
    monitor.__enter__ = Mock(return_value=monitor)
    monitor.__exit__ = Mock(return_value=False)
    if failure == 'missing': monitor.connect.side_effect = FileNotFoundError('missing')
    elif failure == 'timeout': monitor.recv.side_effect = TimeoutError('timeout')
    else: monitor.recv.return_value = b''
    monkeypatch.setattr(nxs_vm.socket, 'socket', Mock(return_value=monitor))
    result = nxs_vm.capture_sync_profile(tmp_path, SimpleNamespace(poll=lambda: None))
    assert result['status'] == 'unavailable' and result['error']


def test_sync_profile_dead_qemu_does_not_open_socket(tmp_path, monkeypatch):
    factory = Mock()
    monkeypatch.setattr(nxs_vm.socket, 'socket', factory)
    result = nxs_vm.capture_sync_profile(tmp_path, SimpleNamespace(poll=lambda: 1))
    assert result['status'] == 'unavailable'
    factory.assert_not_called()


@pytest.mark.parametrize('raw', [
    FRAME, b'P6\n# comment\n2\t1\n255\n' + FRAME[-6:],
    b'P6\r\n2 1\r\n255\r\n' + FRAME[-6:],
])
def test_complete_frame_preserves_raster_whitespace(raw):
    assert nxs_vm.complete_ppm_dimensions(raw) == (2, 1)


@pytest.mark.parametrize('raw', [
    b'', b'P6', FRAME[:-1], FRAME + b'\x00', FRAME + FRAME,
    b'P6\n0 1\n255\n', b'P6\n-2 1\n255\n' + FRAME[-6:],
    b'P6\n2 1\n256\n' + FRAME[-6:], b'P3\n2 1\n255\n0 0 0 0 0 0',
    b'P6\n2 1\n255', b'P6\n2 1\n255# no raster delimiter\n' + FRAME[-6:],
    b'P6\n# unterminated', b' ' + FRAME,
])
def test_incomplete_or_other_output_is_not_saved_as_frame(raw):
    assert nxs_vm.complete_ppm_dimensions(raw) is None


def test_frame_reader_rejects_in_place_rewrite(tmp_path, monkeypatch):
    path = tmp_path / 'screen.ppm'
    path.write_bytes(FRAME)
    original = path.stat()
    changed = SimpleNamespace(**{name: getattr(original, name) for name in
        ('st_dev', 'st_ino', 'st_size', 'st_mtime_ns', 'st_ctime_ns')})
    changed.st_mtime_ns += 1
    monkeypatch.setattr(nxs_vm.os, 'fstat', Mock(side_effect=[original, changed]))
    raw, metadata = nxs_vm.read_frame(path)
    assert raw is None and metadata['status'] == 'changed_during_read'


def test_frame_poll_records_observations_without_fabricating_missed_times(tmp_path):
    recorder = nxs_vm.FrameSnapshots(tmp_path, interval=1, started=100)
    frame = tmp_path / 'screen.ppm'
    recorder.poll(100)
    frame.write_bytes(FRAME[:-1])
    recorder.poll(100.5)  # Not due; no extra observation.
    recorder.poll(101)
    frame.write_bytes(FRAME)
    recorder.poll(102)
    recorder.poll(102.1)
    recorder.poll(103)  # Same bytes remain a new timed observation, not a new GUI frame.
    frame.write_bytes(FRAME[:-1] + b'\x81')
    recorder.poll(105.5)  # No made-up samples at104/105.
    observations = recorder.observations
    assert [row['elapsed_seconds'] for row in observations] == [0, 1, 2, 3, 5.5]
    assert [row['status'] for row in observations] == [
        'missing', 'invalid_or_incomplete_ppm', 'captured', 'captured', 'captured']
    assert [row['same_as_previous_capture'] for row in observations[2:]] == [False, True, False]
    assert len(list((tmp_path / 'frames').glob('*.ppm'))) == 3
    for row in observations[2:]:
        raw = (tmp_path / row['file']).read_bytes()
        assert nxs_vm.complete_ppm_dimensions(raw) == (2, 1)
        assert row['sha256'] == hashlib.sha256(raw).hexdigest()
    assert (tmp_path / observations[2]['file']).read_bytes() == FRAME
    assert json.loads((tmp_path / 'frames/manifest.json').read_text()) == recorder.manifest()
    assert 'do not prove renderer liveness' in recorder.manifest()['meaning']


def test_input_hashes_resolve_the_invoked_artifact(tmp_path):
    binary = tmp_path / 'actual'
    binary.write_bytes(b'actual executable bytes')
    link = tmp_path / 'binary'
    link.symlink_to(binary)
    metadata = nxs_vm.input_metadata(link)
    assert metadata == dict(path=str(binary), size=23,
                            sha256=hashlib.sha256(binary.read_bytes()).hexdigest())


def test_input_mutation_while_hashing_is_rejected(tmp_path, monkeypatch):
    path = tmp_path / 'binary'
    path.write_bytes(b'old')
    def changing_hash(target):
        target.write_bytes(b'changed bytes')
        return 'not a stable hash'
    monkeypatch.setattr(nxs_vm, 'sha256', changing_hash)
    with pytest.raises(RuntimeError, match='input changed while hashing'):
        nxs_vm.input_metadata(path)


@pytest.mark.parametrize('mode,eligible', [
    ('legacy', True), ('deferred-v1', False),
])
def test_dsp_artifact_manifest_records_scheduler_validation_scope(
        tmp_path, mode, eligible):
    firmware = tmp_path / 'firmware'
    firmware.mkdir()
    for name in ('main-firmware.bin', 'gui-boot-memory.elf',
                 'gui-flash-image.bin'):
        (firmware / name).write_bytes(name.encode())
    run = tmp_path / 'run'
    run.mkdir()
    nxs_vm.finalize_dsp_artifacts(
        run, firmware, False, False, False, mode)
    manifest = json.loads((run / 'dsp-checkpoints/manifest.json').read_text())
    assert manifest['dsp_scheduler_mode'] == mode
    assert manifest['architectural_validation_eligible'] is eligible
    scheduling = [item for item in manifest['approximations']
                  if 'deferred-v1' in item]
    assert bool(scheduling) is not eligible
    if scheduling:
        assert 'not a DSP timing fix' in scheduling[0]


@pytest.mark.parametrize('interval', ['-1', 'nan', 'inf'])
def test_invalid_frame_interval_rejected_before_launch(monkeypatch, interval):
    monkeypatch.setattr(nxs_vm.sys, 'argv', ['nxs_vm', 'unused', '--frame-interval', interval])
    with pytest.raises(SystemExit) as error:
        nxs_vm.main()
    assert error.value.code == 2


@pytest.mark.parametrize('interval,deferred,profile', [
    (0, False, False), (0.5, False, False), (0, True, False), (0, True, True),
])
@pytest.mark.parametrize('fresh_link,trace_link', [(False, False), (True, True)])
def test_run_manifest_records_launched_inputs_and_optional_observations(
        tmp_path, monkeypatch, interval, deferred, profile, fresh_link, trace_link):
    paths = ('bin/cdj-run', 'build/qemu/build/qemu-system-sh4',
             'firmware/nxs/main-firmware.bin', 'firmware/nxs/gui-boot-memory.elf',
             'firmware/nxs/gui-flash-image.bin')
    for name in paths:
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(name.encode())
    original_simulator = nxs_vm.sha256(tmp_path / paths[0])
    monkeypatch.setattr(nxs_vm, 'ROOT', tmp_path)
    argv = ['nxs_vm', 'run', '--seconds', '2', '--frame-interval', str(interval)]
    if deferred:
        argv.append('--deferred-dsp-scheduling')
    if profile:
        argv.append('--qemu-sync-profile')
    if fresh_link:
        argv.append('--fresh-link')
    if trace_link:
        argv.append('--trace-link-tx')
    monkeypatch.setattr(nxs_vm.sys, 'argv', argv)
    monkeypatch.setenv('CDJ_NXS_DSP_SCHEDULER', 'inherited-must-not-win')
    monkeypatch.setenv('BFIN_LINK_FRESH_ONLY', 'inherited-must-not-win')
    monkeypatch.setenv('BFIN_SPORT_TX_OUTPUT', '/must/not/be/written')
    clock = [0.0]
    monkeypatch.setattr(nxs_vm.time, 'monotonic', lambda: clock[0])
    monkeypatch.setattr(nxs_vm.time, 'sleep', lambda duration: clock.__setitem__(0, clock[0] + duration))
    monkeypatch.setattr(nxs_vm, 'finalize_dsp_artifacts', Mock())
    def collect(run, process):
        assert process.poll() is None and clock[0] >= 2.5
        return {'status': 'captured', 'commands': list(nxs_vm.SYNC_PROFILE_COMMANDS)}
    collector = Mock(side_effect=collect)
    monkeypatch.setattr(nxs_vm, 'capture_sync_profile', collector)
    class Process:
        pid = 123
        def __init__(self, gui):
            self.gui, self.stopped = gui, False
        def poll(self):
            return 0 if self.stopped or (self.gui and clock[0] >= 2.5) else None
        def terminate(self):
            self.stopped = True
        def wait(self, timeout):
            return 0
    def launch(command, **kwargs):
        gui = '--model' in command
        if not gui:
            assert kwargs['env']['CDJ_REQ_STATUS_FRESH'] == '0'
            assert kwargs['env']['CDJ_LINK_LINK_ROWS'] == 'off'
            assert kwargs['env']['CDJ_NXS_DSP_SCHEDULER'] == (
                'deferred-v1' if deferred else 'legacy')
        if gui:
            assert kwargs['env'].get('BFIN_LINK_FRESH_ONLY') == ('1' if fresh_link else None)
            assert kwargs['env'].get('BFIN_SPORT_TX_OUTPUT') == (
                str(tmp_path / 'run/gui-link-tx.bin') if trace_link else None)
            (tmp_path / 'run/screen.ppm').write_bytes(FRAME)
            (tmp_path / paths[0]).write_bytes(b'rebuilt after GUI launch')
        return Process(gui)
    monkeypatch.setattr(nxs_vm.subprocess, 'Popen', launch)
    assert nxs_vm.main() == 0
    manifest = json.loads((tmp_path / 'run/run.json').read_text())
    assert manifest['main_environment']['CDJ_LINK_LINK_ROWS'] == 'off'
    assert manifest['link_delivery'] == ('fresh-only diagnostic' if fresh_link
                                         else 'legacy cached repeats')
    expected_scheduler = 'deferred-v1' if deferred else 'legacy'
    assert manifest['main_environment']['CDJ_NXS_DSP_SCHEDULER'] == expected_scheduler
    assert manifest['dsp_scheduler_mode'] == expected_scheduler
    assert manifest['qemu_sync_profile']['enabled'] is profile
    assert ('-enable-sync-profile' in manifest['main']) is profile
    if profile:
        collector.assert_called_once()
        assert manifest['qemu_sync_profile']['collection']['status'] == 'captured'
        assert 'host overhead' in manifest['qemu_sync_profile']['observer_overhead']
    else:
        collector.assert_not_called()
    assert manifest['architectural_validation_eligible'] is not deferred
    if deferred:
        assert 'not a DSP timing fix' in manifest['scheduling_provenance']
    else:
        assert manifest['scheduling_provenance'] == \
            'legacy synchronous bounded DSP activation'
    assert len(manifest['input_artifacts']) == 5
    assert manifest['input_artifacts']['simulator']['sha256'] == original_simulator
    assert manifest['inputs_differ_at_exit'] == ['simulator']
    assert manifest['input_artifacts']['qemu']['path'] == str(tmp_path / paths[1])
    if interval:
        observations = manifest['frame_snapshots']['observations']
        assert len(observations) == 3
        assert all(row['status'] == 'captured' for row in observations)
        assert observations[-1]['elapsed_seconds'] < 1.5  # Never observe after process cleanup.
        assert observations[-1]['same_as_previous_capture']
    else:
        assert 'frame_snapshots' not in manifest
        assert not (tmp_path / 'run/frames').exists()
