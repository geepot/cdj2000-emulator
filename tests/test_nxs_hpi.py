"""Test the actual QEMU NXS host port and DMA without proprietary firmware."""
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_host_addressing_and_fixed_port_dma():
    qemu = Path(os.environ.get('CDJ_TEST_QEMU', ROOT / 'build/qemu/build/qemu-system-sh4'))
    if not qemu.is_file(): pytest.skip('requires the custom QEMU build')
    with tempfile.TemporaryDirectory(prefix='cdj-hpi-', dir='/tmp') as directory:
        root = Path(directory)
        rom = root / 'reset.bin'
        rom.write_bytes(bytes(16))  # CPU stays stopped; no proprietary input.
        endpoint = root / 'qtest.sock'
        process = subprocess.Popen([str(qemu), '-M', 'cdj2000nxs-main', '-S', '-display', 'none',
            '-nodefaults', '-bios', str(rom), '-qtest', f'unix:{endpoint},server=on,wait=off'],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 10
            while not endpoint.exists():
                if process.poll() is not None:
                    pytest.fail(process.stderr.read().decode(errors='replace'))
                if time.monotonic() > deadline: pytest.fail('qtest socket startup timed out')
                time.sleep(.05)
            sock = socket.socket(socket.AF_UNIX)
            sock.settimeout(5)
            sock.connect(str(endpoint))
            stream = sock.makefile('rwb', buffering=0)
            def command(text):
                stream.write((text + '\n').encode())
                response = stream.readline().decode().strip()
                assert response.startswith('OK'), response
                return response.split()[1:]
            def write(address, value): command(f'writel {address:#x} {value:#x}')
            def read(address): return int(command(f'readl {address:#x}')[0], 0)
            control, address, auto, fixed = 0xc000000, 0xc040000, 0xc080000, 0xc0c0000
            write(control, 0x01010101)
            assert read(control) == 0x01010101
            write(address, 0x11801da0)
            write(auto, 0x12345678)
            write(auto, 0x90abcdef)
            assert read(address) == 0x11801da8
            write(address, 0x11801da0)
            assert read(fixed) == 0x12345678
            assert read(fixed) == 0x12345678
            assert read(address) == 0x11801da0
            assert read(auto) == 0x12345678
            assert read(auto) == 0x90abcdef
            # Real SH DMAC channel 5, incrementing RAM source / fixed HPID.
            write(0x04001000, 0x11223344)
            write(0x04001004, 0x55667788)
            write(address, 0x11802000)
            write(0x1f608080, 0x04001000)
            write(0x1f608084, auto)
            write(0x1f608088, 2)
            command('writew 0x1f608060 1')
            write(0x1f60808c, 0x1431)
            assert read(0x1f608080) == 0x04001008
            assert read(0x1f608084) == auto
            assert read(0x1f608088) == 0
            assert read(0x1f60808c) & 2
            write(address, 0x11802000)
            assert read(auto) == 0x11223344
            assert read(auto) == 0x55667788
            stream.close(); sock.close()
        finally:
            process.terminate()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.kill(); process.wait(timeout=5)
            process.stderr.close()
