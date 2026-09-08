"""NXS framing must reject corrupt inputs before extracting components."""
import binascii

import pytest

from tools.cdj_gui.nxs_container import COMPONENTS, split_container


def container():
    payloads = []
    for name, _, width, order in COMPONENTS:
        title = {"drive": "DRIV", "panel": "PANL"}.get(name, name.upper())
        raw = f"CDJ-2000NXS {title}".encode().ljust(32, b" ") + b"payload\x00\xff"
        payloads.append(raw + binascii.crc_hqx(raw, 0).to_bytes(width, order))
    return b"".join(str(len(p)).encode() + b"\r\n" for p in payloads) + b"".join(payloads), payloads


def test_preserves_all_component_bytes():
    raw, payloads = container()
    assert list(split_container(raw).values()) == payloads


@pytest.mark.parametrize("component", range(4))
def test_rejects_each_corrupt_component(component):
    raw, payloads = container()
    start = len(raw) - sum(map(len, payloads)) + sum(map(len, payloads[:component]))
    damaged = bytearray(raw)
    damaged[start + 32] ^= 1
    with pytest.raises(ValueError, match="CRC mismatch"):
        split_container(bytes(damaged))


@pytest.mark.parametrize("suffix", [b"extra", None])
def test_rejects_length_mismatch(suffix):
    raw, _ = container()
    with pytest.raises(ValueError, match="size"):
        split_container(raw + suffix if suffix else raw[:-1])


def test_rejects_other_model_even_with_valid_crc():
    raw, _ = container()
    with pytest.raises(ValueError, match="unsupported component title"):
        split_container(raw.replace(b"CDJ-2000NXS", b"CDJ-2000NX2"))
