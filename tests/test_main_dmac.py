"""Exercise the real QEMU board's DMA registers, with the guest CPU stopped.

No proprietary firmware is needed. Build QEMU with scripts/build-qemu-sh4.sh
first, or set CDJ_QEMU to the binary to test.
"""

from __future__ import annotations

import os
from pathlib import Path
import select
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]
DMAC = 0x1F608000
CHANNEL = DMAC + 0x80


@pytest.fixture
def dma(tmp_path):
    binary = Path(os.environ.get("CDJ_QEMU", ROOT / "build/qemu/build/qemu-system-sh4"))
    if not binary.is_file():
        pytest.skip("build the CDJ QEMU board or set CDJ_QEMU")
    bios = tmp_path / "dummy.bin"
    bios.write_bytes(bytes(16))
    env = {k: v for k, v in os.environ.items() if not k.startswith(("CDJ_", "BFIN_"))}
    with (tmp_path / "qemu.log").open("w") as log:
        proc = subprocess.Popen(
            [str(binary), "-M", "cdj2000nxs-main", "-bios", str(bios),
             "-display", "none", "-serial", "null", "-monitor", "none",
             "-S", "-qtest", "stdio", "-qtest-log", os.devnull],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, env=env,
            bufsize=0,
        )

        def command(line):
            proc.stdin.write((line + "\n").encode())
            assert select.select([proc.stdout], [], [], 5)[0], "QEMU qtest timed out"
            response = proc.stdout.readline().decode().strip()
            assert response.startswith("OK"), response
            return response[2:].strip()

        try:
            yield command
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            proc.stdin.close()
            proc.stdout.close()


@pytest.mark.parametrize("unit,ts", [(4, 0x30), (16, 0x38)])
@pytest.mark.parametrize("sm,dm", [(0, 1), (1, 0), (1, 1), (2, 1), (1, 2), (0, 0)])
def test_dma_address_modes(dma, unit, ts, sm, dm):
    source_base, dest_base = 0x04100000, 0x04200000
    count = 1031  # Cross the bulk-copy chunk boundary, including a short tail.
    data = bytes((i * 37 + i // unit) & 255 for i in range(count * unit))
    dma(f"write {source_base:#x} {len(data):#x} 0x{data.hex()}")
    dma(f"memset {dest_base:#x} {len(data):#x} 0xcc")
    source = source_base + ((count - 1) * unit if sm == 2 else 0)
    dest = dest_base + ((count - 1) * unit if dm == 2 else 0)
    expected = bytearray([0xCC] * len(data))
    src, dst = source, dest
    for _ in range(count):
        expected[dst - dest_base:dst - dest_base + unit] = data[src - source_base:src - source_base + unit]
        src += {0: 0, 1: unit, 2: -unit}[sm]
        dst += {0: 0, 1: unit, 2: -unit}[dm]
    for offset, value in [(0, source), (4, dest), (8, count)]:
        dma(f"writel {CHANNEL + offset:#x} {value:#x}")
    dma(f"writew {DMAC + 0x60:#x} 1")
    chcr = (sm << 12) | (dm << 14) | 0x400 | ts
    dma(f"writel {CHANNEL + 12:#x} {chcr | 1:#x}")
    actual = bytes.fromhex(dma(f"read {dest_base:#x} {len(data):#x}")[2:])
    assert actual == expected
    for offset, value in [(0, src), (4, dst), (8, 0), (12, chcr | 2)]:
        assert int(dma(f"readl {CHANNEL + offset:#x}"), 0) == value
