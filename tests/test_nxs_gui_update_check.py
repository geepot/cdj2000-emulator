# SPDX-License-Identifier: GPL-2.0-or-later
"""nxs_gui_update_check's verdicts, on synthetic flash dumps and MAIN states.

The emulator round trip itself (two nxs_vm runs) is not run here.
"""

from __future__ import annotations

import struct

import pytest

from tools.cdj_main import nxs_gui_update_check as check

LOG_OK = "".join(f"bf531: flash erase 0x{0x20000000 + at:08x} +0x{n:x}\n" for at, n in
                 [(0x1FA000, 0x2000), (0x1F8000, 0x2000), (0x1F0000, 0x8000), (0x10000, 0x10000)])
LOG_OK = "cdj-gui-run: booting the flash's LDR at 0x10000\n" + LOG_OK
DONE_ALL = dict(update_phase=9, update_phase_saved=0, update_write_index=2,
                update_progress_gui=1, gui_update_status=2)
DONE_GUI = dict(DONE_ALL, update_phase=0xFF, update_write_index=6, gui_update_status=3)


def component(body: bytes, mode: bytes = b"1") -> bytes:
    title = b"CDJ-2000NXS GUI Ver1.205 023456" + mode
    return b"2015268\r\n" + title + body + b"\x00\x00\x41\xed"


def test_body_of_and_mode_gate():
    body = bytes(range(256)) * (check.BODY // 256)
    assert check.body_of(component(body)) == body
    with pytest.raises(SystemExit, match="over the loader"):
        check.body_of(component(body, b"0"))


def test_main_verdict():
    assert check.main_verdict(None, "all")[0] is None
    assert check.main_verdict(DONE_ALL, "all")[0] is True
    assert check.main_verdict(dict(DONE_ALL, update_write_index=1), "all")[0] is None
    assert check.main_verdict(dict(DONE_ALL, update_phase_saved=0x89), "all")[0] is False
    assert check.main_verdict(dict(DONE_ALL, gui_update_status=0xFF), "all")[0] is False
    assert check.main_verdict(DONE_ALL, "gui")[0] is None       # not at Update END yet
    assert check.main_verdict(DONE_GUI, "gui")[0] is True
    # Skipped by the version gate: MAIN moves on without sending anything.
    skipped = dict(DONE_ALL, update_progress_gui=0)
    assert check.main_verdict(skipped, "all") == (
        False, "GUI component not selected (version gate)")
    # Moved on without the GUI's success report (a GUI that went silent).
    assert check.main_verdict(dict(DONE_ALL, gui_update_status=1), "all")[0] is False
    assert check.main_verdict(dict(DONE_GUI, update_progress_gui=0), "gui")[0] is False


def test_unpacker_detection():
    def ldr(*dests):
        return b"".join(struct.pack("<IIH", d, 2, 0x8000 if i == len(dests) - 1 else 0) + b"\0\0"
                        for i, d in enumerate(dests))
    assert check.has_unpacker(ldr(0x00C66E44, 0x01E40000, 0xFFA08000))
    assert not check.has_unpacker(ldr(0x00C66E44, 0xFFA08000) + ldr(0x01E40000))


def test_verify(tmp_path):
    body = b"\x11" * check.BODY
    before = check.seed_flash(b"\x22" * check.BODY)
    assert before[check.TOP:check.TOP + 4] == check.JOURNAL_SEED
    good = bytearray(before)
    good[check.BODY_AT:check.BODY_AT + check.BODY] = body
    good[check.TOP + 4:check.TOP + 8] = b"\x00\x03\x00\x03"       # the GUI appended: fine
    dump = tmp_path / "dump.bin"

    def failures(flash, state=DONE_ALL, log=LOG_OK, unpacker=False):
        dump.write_bytes(bytes(flash))
        return [what for what, ok in check.verify("x", dump, before, body, state, "all", log,
                                                  unpacker) if not ok]

    assert failures(good) == []
    bad = bytearray(good)
    bad[5] = 0
    assert failures(bad) == ["x: sector 0 (the first-stage loader's place) untouched"]
    bad = bytearray(good)
    bad[check.TOP] = 0xFF
    assert failures(bad) == ["x: journal entries at 0x1FC000 kept"]
    bad = bytearray(good)
    bad[check.BODY_AT + 7] ^= 1
    assert failures(bad) == ["x: body at 0x10000 byte for byte"]
    wiped = LOG_OK + "bf531: flash erase 0x201fc000 +0x4000\n"
    assert failures(good, log=wiped) == ["x: no erase below 0x10000 or of the 0x1FC000 journal"]
    elf = LOG_OK.replace("booting the flash's LDR at 0x10000", "")
    assert len(failures(good, log=elf)) == 1
    assert len(failures(good, state=dict(DONE_ALL, update_phase_saved=0x87))) == 1
    assert len(failures(good, unpacker=True)) == 1                # no coverage line
    ran = LOG_OK + "RANGE 0x01e40000:0x01e50000 lines_run=9\n"
    assert failures(good, log=ran, unpacker=True) == []
    dump.unlink()
    assert check.verify("x", dump, before, body, DONE_ALL, "all", LOG_OK, False)[-1] == \
        ("x: flash dump written", False)
