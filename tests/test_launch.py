"""The unified launcher selects the requested hardware profile."""

from tools.cdj_main import launch, nxs_vm, view_vm


def test_profile_backends_are_explicit():
    assert launch.backend("2000") is view_vm
    assert launch.backend("nxs") is nxs_vm
