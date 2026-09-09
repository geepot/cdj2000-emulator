"""Keep NXS key bindings separate and check names against the real image."""
from pathlib import Path
import struct

import pytest

from tools.cdj_main import nxs_panel, panel_control
from tools.cdj_gui import faceplate, view_ui


def test_sources_and_time_are_shifted_only_in_nxs():
    assert panel_control.button_mask("usb") == (19, 2)
    assert [nxs_panel.deck_input(f"19.{i}") for i in range(5)] == [
        "19.1", "19.2", "19.3", "19.4", "19.5"]
    for source, expected in zip(("LINK", "USB", "SD", "DISC"), range(1, 5)):
        control = next(c for c in view_ui.controls(True) if c.label == source)
        assert control.input_id == f"19.{expected}"
        assert control.lines == (panel_control.encode_press(19, 1 << expected,
                                                           view_ui.WINDOW_HOLD_MS),)


def test_nxs_inventory_and_deck_have_no_lost_or_duplicate_bindings():
    built = view_ui.controls(True)
    reached, missing, stray = view_ui.coverage(built, True)
    assert not missing and not stray
    assert set(reached) == set(nxs_panel.input_ids())
    mapped = [nxs_panel.deck_input(key) for key in faceplate.PLACEMENTS]
    assert len(mapped) == len(set(mapped))
    assert set(mapped) <= {c.input_id for c in built}
    assert view_ui.coverage(view_ui.controls())[1:] == ([], [])


def test_changed_contacts_match_nxs_firmware_service_names():
    image = Path(__file__).resolve().parents[1] / "firmware/nxs/main-unpacked.bin"
    if not image.exists():
        pytest.skip("NXS firmware is not supplied")
    data = image.read_bytes()
    def u32(offset):
        return struct.unpack_from("<I", data, offset)[0]
    def name(code):
        offset = u32(0x3c32f0 + code * 4) - 0xa4000000
        return data[offset:offset + 40].split(b"\0")[0].decode("ascii")
    # Decoder 042f5810: physical contact -> byte/bit in status record.
    decoded = {
        (18, 0): (0x53, 7), (18, 1): (0x4e, 5),
        (18, 2): (0x4e, 0), (18, 3): (0x4d, 7),
        (18, 4): (0x4d, 6), (18, 5): (0x4d, 5),
        (18, 6): (0x4c, 4), (18, 7): (0x4c, 3),
        (19, 0): (0x5e, 4), (19, 1): (0x5f, 7),
        (19, 2): (0x5f, 6), (19, 3): (0x5f, 5),
        (19, 4): (0x5f, 4), (19, 5): (0x4d, 0),
        (19, 6): (0x4c, 5), (19, 7): (0x4c, 6),
        (20, 6): (0x4c, 7), (21, 0): (0x4d, 4),
        (21, 1): (0x53, 6), (21, 2): (0x53, 5),
        (21, 3): (0x4d, 3), (21, 4): (0x4d, 2),
        (21, 5): (0x4d, 1),
    }
    names = {}
    for base, table in [(0x4c, 0xb0eb4), (0x50, 0xb0fac), (0x5c, 0xb0fdc)]:
        while (mask := u32(table)):
            bit = mask.bit_length() - 1
            names[base + bit // 8, bit % 8] = name(u32(table + 4))
            table += 8
    for contact, status in decoded.items():
        assert nxs_panel.KEY_NAMES[contact] == names[status]
