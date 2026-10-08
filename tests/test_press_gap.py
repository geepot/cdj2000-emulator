"""The per-press gap (press <byte> <mask> <hold> <gap>) and the sub-second pauses of cosim_scenario --then."""
from tools.cdj_main import cosim_scenario as cs
from tools.cdj_main import panel_control as pc


def test_encode_press_with_gap():
    assert pc.encode_press(17, 0x01, 30, 40) == "press 17 01 30 40\n"
    assert pc.encode_press(17, 0x01, 30) == "press 17 01 30\n"          # unchanged without a gap
    assert pc.encode_press(17, 0x01, None) == "press 17 01\n"


def test_sub_second_detection():
    assert cs.sub_second(("17.0@30", "0.07"))
    assert cs.sub_second(("17.0", "0.012"))
    assert not cs.sub_second(("17.0", "1"))          # whole seconds: the old census wait
    assert not cs.sub_second(("17.0", "3"))
    assert not cs.sub_second(("needle=300", "0.5"))  # only plain button presses are paced
    assert not cs.sub_second(("rot+1", "0.5"))
    assert cs.plain_press(("16.0", "3")) and not cs.plain_press(("direction-rev", "3"))
