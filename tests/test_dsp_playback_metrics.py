"""Playback-only accounting never falls back to whole-run DSP averages."""
import pytest
from tools.cdj_dsp.playback_metrics import read_window
from tools.cdj_dsp.qemu_pgo import replace_output, dsp_source
from pathlib import Path


def test_playback_window_ignores_boot_and_error_polling(tmp_path):
    log = tmp_path / 'main.log'
    log.write_text('nxs-dsp-thread: virtual=100s packets=999999 thread-cpu=30s\n'
                   'nxs-dsp-playback: final=0 virtual=1s cpu=1s packets=3 background=1 slots=8 underruns=7 position=4\n'
                   'nxs-dsp-playback: final=1 virtual=2s cpu=0.5s packets=100 background=5 slots=20 underruns=8 position=9\n')
    row = read_window(log)
    assert row['packets_per_cpu_second'] == 200
    assert row['background_per_virtual_second'] == 2.5
    assert row['underrun_fraction'] == 0.4
    log.write_text('nxs-dsp-thread: virtual=100s packets=999999 thread-cpu=30s\n')
    with pytest.raises(ValueError, match='completed playback'):
        read_window(log)


def test_isolated_object_compile_does_not_touch_ninja_depfiles():
    command = ['clang', '-MD', '-MQ', 'base.o', '-MF', 'base.o.d', '-o', 'base.o', '-c', 'core.c']
    out = replace_output(command, Path('/tmp/variant.o'))
    assert out == ['clang', '-MD', '-o', '/tmp/variant.o', '-c', 'core.c']
    assert command[command.index('-o') + 1] == 'base.o'
    assert dsp_source(Path('cdj_c674x.c'))
    assert dsp_source(Path('cdj2000_nxs_hpi.c'))
    assert not dsp_source(Path('cdj2000_nxs_main.c'))
    assert not dsp_source(Path('translate.c'))


def test_training_profile_refuses_existing_raw_counters(tmp_path, monkeypatch, capsys):
    from tools.cdj_main import nxs_vm
    raw = tmp_path / 'already-trained.profraw'
    raw.write_bytes(b'existing profile')
    monkeypatch.setattr('sys.argv', ['nxs_vm', '--dsp-playback-profile', str(raw)])
    with pytest.raises(SystemExit) as error:
        nxs_vm.main()
    assert error.value.code == 2
    assert 'must name a new file' in capsys.readouterr().err
    assert raw.read_bytes() == b'existing profile'


def test_qemu_pgo_rejects_stale_training_before_writing_objects(tmp_path, monkeypatch, capsys):
    import json
    from tools.cdj_dsp import qemu_pgo
    build = tmp_path / 'qemu/build'
    source = tmp_path / 'qemu/hw/sh4'
    source.mkdir(parents=True)
    build.mkdir()
    (source / 'cdj_c674x.c').write_text('/* new core */\n')
    (source / 'cdj_c674x_aot.inc').write_text('/* synthetic generated fixture */\n')
    (build / 'other.o').write_bytes(b'unchanged QEMU object')
    (build / 'compile_commands.json').write_text(json.dumps([dict(
        file='../hw/sh4/cdj_c674x.c', command='clang -Iconfig-sh4-softmmu -o core.o -c ../hw/sh4/cdj_c674x.c')]))
    training = tmp_path / 'train.json'
    training.write_text(json.dumps(dict(mode='train', sources={'cdj_c674x.c': 'old hash'})))
    profile = tmp_path / 'profile.profdata'
    profile.write_bytes(b'fixture profile')
    output = tmp_path / 'candidate'
    monkeypatch.setattr(qemu_pgo.subprocess, 'check_output',
                        lambda command, **kw: ('clang -o qemu-system-sh4 core.o other.o\n'
                                               if command[0] == 'ninja' else 'clang fixture compiler'))
    monkeypatch.setattr('sys.argv', ['qemu_pgo', '--build', str(build), '--output', str(output),
                                   '--profile', str(profile), '--training-build', str(training)])
    with pytest.raises(SystemExit) as error:
        qemu_pgo.main()
    assert error.value.code == 2
    assert 'changed since training' in capsys.readouterr().err
    assert not output.exists()
    assert (build / 'other.o').read_bytes() == b'unchanged QEMU object'
