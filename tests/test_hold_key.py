# SPDX-License-Identifier: GPL-2.0-or-later
"""--hold-key: a panel key held from guest time zero, as a CDJ_PANEL_KEYS entry with its own hold time."""
import pytest

from tools.cdj_main.boot_vm import hold_key_entry
from tools.cdj_main.panel_control import button_mask


def test_a_key_by_the_name_main_knows_it_by():
    assert button_mask("delete") == (21, 0x04)
    assert button_mask("DELETE") == (21, 0x04)
    assert button_mask("hot cue a") == (16, 0x20)
    assert button_mask("hot_cue_a") == (16, 0x20)


def test_the_old_spellings_still_resolve():
    assert button_mask("sd") == (19, 0x04)
    assert button_mask("20.3-hold") == (20, 0x08)
    assert button_mask("21:04") == (21, 0x04)


def test_unknown_key_is_refused():
    with pytest.raises(ValueError):
        button_mask("no such key")


def test_hold_entry_starts_at_zero_and_defaults_to_twenty_seconds():
    assert hold_key_entry("delete") == "0:21:04:20"
    assert hold_key_entry("delete:5") == "0:21:04:5"
    assert hold_key_entry("21.2:1.5") == "0:21:04:1.5"
