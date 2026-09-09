"""Deck geometry and gestures, independent of firmware and TCP networking."""
import math
import os
from types import SimpleNamespace
from unittest.mock import Mock

import pytest
from PIL import Image

from tools.cdj_gui import faceplate, view_ui
from tools.cdj_main import panel_control


@pytest.mark.parametrize('width,height', [(700, 450), (1100, 700), (1440, 1000)])
def test_fit_keeps_the_entire_deck_in_the_viewport(width, height):
    scale = faceplate.fit_scale(width, height)
    assert round(faceplate.PANEL_W * scale) <= width - 24
    assert round(faceplate.PANEL_H * scale) <= height - 24


def test_controls_never_overlap_the_lcd_or_leave_the_deck():
    lcd = (faceplate.LCD_X, faceplate.LCD_Y, faceplate.LCD_W, faceplate.LCD_H)
    def overlaps(a, b):
        return a[0] < b[0]+b[2] and b[0] < a[0]+a[2] and a[1] < b[1]+b[3] and b[1] < a[1]+a[3]
    places = list(faceplate.PLACEMENTS.items())
    for index, (name, p) in enumerate(places):
        assert p.x >= 0 and p.y >= 0
        assert p.x+p.w <= faceplate.PANEL_W, name
        assert p.y+p.h <= faceplate.PANEL_H, name
        assert not overlaps(p[:4], lcd), name
        for other_name, other in places[index+1:]:
            assert not overlaps(p[:4], other[:4]), (name, other_name)


def test_inspector_completes_the_deck_input_set():
    board = set(panel_control.input_ids())
    placed = {name.split('-')[0] for name in faceplate.placed_ids()}
    placed.add('field7')
    assert placed | set(faceplate.unplaced(list(board))) == board


def test_glow_padding_and_lcd_are_not_click_targets():
    deck = object.__new__(faceplate.Faceplate)
    deck.scale = 1.5
    assert deck.hit_control(300 * 1.5, 200 * 1.5) is None
    assert deck.hit_control(5 * 1.5, 70 * 1.5) is None
    assert deck.hit_control(40 * 1.5, 70 * 1.5) == '19.0'
    assert deck.hit_control(242 * 1.5, 302 * 1.5) is None  # platter bounding-box corner


def knob():
    deck = object.__new__(faceplate.Faceplate)
    deck.focus_set = Mock()
    deck._pressed = Mock()
    deck.on_rotate = Mock()
    deck.flash = Mock()
    deck._knob_angle = lambda e: e.angle
    deck._drag_angle = None
    deck._drag_started = False
    return deck


def test_knob_drag_does_not_also_push():
    deck = knob()
    deck._knob_down(SimpleNamespace(angle=0))
    deck._knob_drag(SimpleNamespace(angle=math.pi/2), '17.0')
    deck._knob_up('17.0')
    deck.on_rotate.assert_called_once_with(faceplate.ENCODER_FIELD, 6)
    deck._pressed.assert_not_called()


def test_knob_click_pushes_once_and_modified_release_does_not_push():
    deck = knob()
    deck._knob_down(SimpleNamespace(angle=0))
    deck._knob_up('17.0')
    deck._pressed.assert_called_once_with('17.0')
    deck._knob_up('17.0')
    deck._pressed.assert_called_once()


def test_attach_mode_does_not_start_a_simulator_or_remove_frame(tmp_path):
    frame = tmp_path / 'frame.ppm'
    frame.write_bytes(b'keep existing frame')
    args = view_ui.parse_args(['--attach', '--output', str(frame)])
    viewer = object.__new__(view_ui.UiViewer)
    viewer.args, viewer.status = args, Mock()
    viewer.start_simulator()
    assert frame.read_bytes() == b'keep existing frame'
    viewer.status.set.assert_called_once()


def test_nxs_window_attaches_and_closing_it_stops_owned_boards(tmp_path, monkeypatch):
    from tools.cdj_main import nxs_vm
    for name in ('bin/cdj-run', 'build/qemu/build/qemu-system-sh4',
                 'firmware/nxs/main-firmware.bin', 'firmware/nxs/gui-boot-memory.elf',
                 'firmware/nxs/gui-flash-image.bin'):
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.touch()
    monkeypatch.setattr(nxs_vm, 'ROOT', tmp_path)
    monkeypatch.setattr(nxs_vm.sys, 'argv', ['nxs_vm', 'run', '--ui'])
    monkeypatch.setattr(nxs_vm.time, 'sleep', lambda _: None)
    monkeypatch.setattr(nxs_vm, 'finalize_dsp_artifacts', Mock())
    launched = []
    def start(command, **kwargs):
        process = Mock(pid=len(launched)+100)
        process.poll.return_value = 0 if '--attach' in command else None
        process.returncode = process.poll.return_value
        launched.append((command, process))
        return process
    monkeypatch.setattr(nxs_vm.subprocess, 'Popen', start)
    assert nxs_vm.main() == 0
    assert len(launched) == 3
    assert '--attach' in launched[2][0]
    assert '--control-port' in launched[2][0]
    for _, process in launched[:2]:
        process.terminate.assert_called_once()


@pytest.mark.skipif(os.environ.get('CDJ_TEST_TK') != '1', reason='opt-in native Tk smoke test')
def test_native_deck_resize_and_inspector(tmp_path):
    args = view_ui.parse_args(['--attach', '--output', str(tmp_path / 'not-yet.ppm')])
    viewer = view_ui.UiViewer(args)
    try:
        viewer.root.update_idletasks()
        assert not viewer.inspector.winfo_viewable()
        assert set(viewer.analog_value) == set(range(7))
        frame = Image.new('RGB', (480, 234), (13, 57, 91))
        viewer.show_panel(frame)
        viewer.deck.set_latched('16.0', True)
        for scale in (0.9, 1.25, 2.0):
            viewer.deck.set_scale(scale)
            assert viewer.deck.lcd_photo.width() == round(480 * scale)
            assert viewer.deck.lcd_photo.height() == round(234 * scale)
            assert viewer.deck.last_frame.tobytes() == frame.tobytes()
            assert '16.0' in viewer.deck.latched
        viewer.show_inspector()
        viewer.root.update_idletasks()
        assert viewer.inspector.winfo_viewable()
    finally:
        viewer.close()
