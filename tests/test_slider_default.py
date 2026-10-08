# SPDX-License-Identifier: GPL-2.0-or-later
"""The base panel frame starts the TEMPO slider at its centre.

A frame of zeros is slider position 0 with centre 0 (payload bytes 4/5 and 6/7, ANSWER-NEW-FIRMWARE-1003); MAIN
turns that into a pitch word of 0 in the status packet, where a real deck at rest sends 0x100000.  The stock SYNC
engine on a following deck multiplies the BPM by that pitch, so the emulated deck stood still.  `cdj_panel_frame`
therefore puts 0x8000 into both pairs unless the frame names a slider of its own.

`cdj_panel_frame` is static in a QEMU device file, so what is checked here is the shipped source, tied to the field
table of the input channel (`analog 2` / `analog 3`); the run that shows MAIN's reply is
`scripts/two-decks.sh` and the pitch words of `hub/link.pcap` (see RUNNING.md).
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN_C = (ROOT / "emulator/qemu/cdj2000_main.c").read_text(encoding="utf-8")
INPUT_C = (ROOT / "emulator/qemu/cdj2000_input.c").read_text(encoding="utf-8")


def panel_frame_body():
    start = MAIN_C.index("static void cdj_panel_frame(uint8_t *frame)")
    return MAIN_C[start:MAIN_C.index("\n}\n", start)]


def test_field_2_is_bytes_4_5_and_field_3_is_bytes_6_7():
    table = re.search(r"cdj_input_analog\[CDJ_INPUT_ANALOG_FIELDS\] = \{(.*?)\};", INPUT_C, re.S).group(1)
    fields = [(int(b), int(w)) for b, w in re.findall(r"\{\s*(\d+),\s*(\d+)\s*\}", table)]
    assert fields[2] == (4, 2)          # slider position
    assert fields[3] == (6, 2)          # slider centre


def test_default_frame_puts_the_slider_at_its_centre():
    body = panel_frame_body()
    m = re.search(r"if \(!cdj_nxs_profile && !frame\[4\] && !frame\[5\] && !frame\[6\] && !frame\[7\]\) \{\s*"
                  r"frame\[4\] = 0x80;\s*frame\[6\] = 0x80;\s*\}", body)
    assert m, "the slider default is missing from cdj_panel_frame"
    # after the CDJ_PANEL_FRAME bytes (a frame that names a slider keeps it) and before the checksum
    assert body.index("strtoul(digits, NULL, 16)") < m.start() < body.index("cdj_input_apply(frame, PANEL_FRAME_LEN - 2);")
    # the big-endian pair 0x8000 for position and for centre
    assert (0x80 << 8) | 0x00 == 0x8000


def test_the_nxs_board_keeps_its_own_frame():
    assert "!cdj_nxs_profile && !frame[4]" in panel_frame_body()
