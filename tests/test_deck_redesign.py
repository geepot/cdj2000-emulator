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


def contact_viewer():
    viewer = object.__new__(view_ui.UiViewer)
    viewer.held, viewer.momentary = {}, {}
    viewer.deck, viewer.control_note = Mock(), Mock()
    viewer.send = Mock(return_value='ok')
    return viewer


def play_control():
    return next(c for c in view_ui.controls() if c.input_id == '16.0')


def test_physical_contact_sends_down_and_up_without_a_timed_pulse():
    viewer, control = contact_viewer(), play_control()
    assert viewer.contact(control, True)
    assert viewer.contact(control, True)  # repeated press is idempotent
    assert viewer.contact(control, False)
    assert [call.args[1] for call in viewer.send.call_args_list] == [
        panel_control.encode_hold(16, 1, True),
        panel_control.encode_hold(16, 1, False)]
    assert not viewer.momentary


def test_mouse_release_preserves_an_explicit_latch():
    viewer, control = contact_viewer(), play_control()
    viewer.toggle_hold(control)
    viewer.contact(control, True)
    viewer.contact(control, False)
    assert (16, 1) in viewer.held
    viewer.send.assert_called_once()
    viewer.toggle_hold(control)
    assert viewer.send.call_args.args[1] == panel_control.encode_hold(16, 1, False)


def test_failed_contact_is_not_painted_as_delivered():
    viewer, control = contact_viewer(), play_control()
    viewer.send.return_value = None
    assert not viewer.contact(control, True)
    assert not viewer.momentary
    viewer.deck.set_latched.assert_not_called()


def test_rejected_protocol_reply_is_a_failure():
    viewer = contact_viewer()
    viewer.args = SimpleNamespace(control_port=5984)
    channel = Mock()
    channel.send.return_value = 'err queue full'
    viewer.control = Mock(return_value=channel)
    assert view_ui.UiViewer.send(viewer, play_control(), 'down 16 01') is None


def test_closing_attached_viewer_releases_owned_contacts_only():
    viewer, control = contact_viewer(), play_control()
    viewer.momentary[(16, 1)] = control
    viewer.panel, viewer.root = Mock(), Mock()
    viewer.process = viewer.log_stream = None
    viewer.close()
    viewer.send.assert_called_once_with(control, panel_control.encode_hold(16, 1, False))
    assert viewer.panel is None
    viewer.root.destroy.assert_called_once()


def pointer_deck():
    deck = knob()
    deck.active_pointer = None
    deck.on_contact = Mock(return_value=True)
    deck.resolve = lambda name: name
    deck.hit_control = Mock(return_value='16.0')
    deck._show_resting = Mock()
    return deck


def test_mouse_up_outside_releases_the_original_key():
    deck = pointer_deck()
    deck._pointer_down(SimpleNamespace(x=0, y=0, state=0))
    deck._pointer_drag(SimpleNamespace(x=900, y=900))
    deck._pointer_up(SimpleNamespace(x=900, y=900))
    assert [call.args for call in deck.on_contact.call_args_list] == [
        ('16.0', True), ('16.0', False)]
    deck._pressed.assert_not_called()
    deck.on_rotate.assert_not_called()


def test_focus_loss_releases_once_and_cancels_knob_without_a_push():
    deck = pointer_deck()
    deck._pointer_down(SimpleNamespace(x=0, y=0, state=0))
    deck._focus_out(None)
    deck._pointer_up(None)
    assert deck.on_contact.call_count == 2
    deck.active_pointer = '17.0'
    deck._drag_angle = 1
    deck._focus_out(None)
    assert deck._drag_angle is None
    deck._pressed.assert_not_called()


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
        viewer.send = Mock(return_value='ok')
        viewer.deck.set_scale(1)
        viewer.root.update_idletasks()
        p = faceplate.PLACEMENTS['16.0']
        viewer.deck.event_generate('<ButtonPress-1>', x=int(p.x+p.w/2),
                                   y=int(p.y+p.h/2))
        assert (16, 1) in viewer.momentary
        viewer.deck.event_generate('<ButtonRelease-1>', x=-20, y=-20)
        assert not viewer.momentary
        assert [call.args[1] for call in viewer.send.call_args_list] == [
            panel_control.encode_hold(16, 1, True),
            panel_control.encode_hold(16, 1, False)]
    finally:
        viewer.close()
