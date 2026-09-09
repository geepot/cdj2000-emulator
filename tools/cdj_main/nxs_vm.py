# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the experimental NXS MAIN and GUI profiles over a direct serial link.

No proxy or generated status packets. DSP uses UHPI transport and a partial C674x interpreter.
"""
from __future__ import annotations
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]

CHECKPOINT_HEADER = struct.Struct('<8sIIII9I5IQQ')
CHECKPOINT_MAGIC = {1: b'CDJDSP1\0', 2: b'CDJDSP2\0', 3: b'CDJDSP3\0'}
SHARED_RAM_SIZE = 0x20000


def fnv1a(data) -> int:
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def checkpoint_metadata(path: Path) -> dict:
    raw = path.read_bytes()
    if len(raw) < CHECKPOINT_HEADER.size:
        raise RuntimeError(f'incomplete DSP checkpoint: {path}')
    fields = CHECKPOINT_HEADER.unpack_from(raw)
    magic, schema, endian, header_size, state_size = fields[:5]
    component_sizes = list(fields[5:14])
    l2_size, sdram_size, page_size, page_count, present_pages = fields[14:19]
    payload_size, payload_checksum = fields[19:21]
    if (CHECKPOINT_MAGIC.get(schema) != magic or endian != 0x01020304 or
            header_size != CHECKPOINT_HEADER.size or
            len(raw) != header_size + payload_size or
            fnv1a(memoryview(raw)[header_size:]) != payload_checksum):
        raise RuntimeError(f'incompatible or incomplete DSP checkpoint: {path}')
    shared_size = SHARED_RAM_SIZE if schema >= 2 else 0
    shared_start = header_size + state_size + l2_size
    shared = raw[shared_start:shared_start + shared_size]
    return dict(file=path.name, sha256=hashlib.sha256(raw).hexdigest(),
                size=len(raw), schema=schema, endian='little', state_size=state_size,
                component_sizes=component_sizes, l2_size=l2_size,
                shared_ram_size=shared_size,
                shared_ram_sha256=(hashlib.sha256(shared).hexdigest()
                                   if shared_size else None),
                sdram_size=sdram_size, page_size=page_size,
                page_count=page_count, present_pages=present_pages,
                payload_checksum=f'{payload_checksum:016x}')


def finalize_dsp_artifacts(run: Path, firmware: Path) -> None:
    checkpoint_dir = run / 'dsp-checkpoints'
    checkpoints = [checkpoint_metadata(path) for path in sorted(checkpoint_dir.glob('*.cdjdsp'))]
    events = run / 'dsp-events.jsonl'
    event_counts = Counter()
    last_sequence = 0
    boot_phases = []
    if events.is_file():
        with events.open() as stream:
            for line in stream:
                event = json.loads(line)
                if event['sequence'] != last_sequence + 1:
                    raise RuntimeError('DSP event transcript sequence is incomplete')
                last_sequence = event['sequence']
                event_counts[event['event']] += 1
                if event['event'] == 'boot_phase':
                    boot_phases.append(event['value'])
    sources = sorted((ROOT / 'emulator/qemu').glob('cdj_c674*.c')) + \
              sorted((ROOT / 'emulator/qemu').glob('cdj_c674*.h')) + \
              [ROOT / 'emulator/qemu/cdj_dsp_checkpoint.c',
               ROOT / 'emulator/qemu/cdj_dsp_checkpoint.h',
               ROOT / 'emulator/qemu/cdj2000_nxs_hpi.c']
    manifest = dict(schema=3, format=('ABI-bound native state including C6747 INTC, '
                                     'L2 and shared RAM plus sparse zero-default SDRAM pages'),
        byte_order=sys.byteorder, complete=bool(checkpoints and events.is_file()),
        checkpoints=checkpoints, latest=checkpoints[-1]['file'] if checkpoints else None,
        event_transcript=dict(file=events.name, sha256=sha256(events) if events.is_file() else None,
                              events=last_sequence, counts=dict(sorted(event_counts.items())),
                              boot_phases=boot_phases),
        firmware_sha256={path.name: sha256(path) for path in
            (firmware / 'main-firmware.bin', firmware / 'gui-boot-memory.elf',
             firmware / 'gui-flash-image.bin')},
        source_sha256={str(path.relative_to(ROOT)): sha256(path) for path in sources},
        approximations=[
            'DSP boot ROM is not executed; its documented HPI-ready handoff is modeled',
            'checkpoint schema 3 is ABI-bound and rejects structure-size or endianness changes',
            '128 KiB C6747 shared RAM is captured losslessly',
            'sparse SDRAM pages are lossless because omitted pages restore as zero',
            'SDRAM command timing, arbitration and retention are not modeled',
            'PSC transition ticks and PLL divider GO latency remain deterministic approximations',
            'physical HPI pins, FIFO/HRDY timing and DSP interrupt delivery are not modeled'])
    checkpoint_dir.mkdir(parents=True, exist_ok=True)
    (checkpoint_dir / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path, help='new run directory, relative to repository')
    parser.add_argument('--seconds', type=float, default=60)
    parser.add_argument('--port', type=int, default=5980)
    parser.add_argument('--qemu', type=Path, default=ROOT / 'build/qemu/build/qemu-system-sh4')
    args = parser.parse_args()
    if args.seconds <= 0 or not 1024 <= args.port <= 65531:
        parser.error('positive duration and port 1024..65531 required')
    run = (ROOT / args.run).resolve()
    firmware = ROOT / 'firmware/nxs'
    simulator = ROOT / 'bin/cdj-run'
    for path in (args.qemu, simulator, firmware / 'main-firmware.bin', firmware / 'gui-boot-memory.elf', firmware / 'gui-flash-image.bin'):
        if not path.is_file(): parser.error(f'missing input: {path}')
    run.mkdir(parents=True, exist_ok=False)
    main_command = [str(args.qemu.resolve()), '-M', 'cdj2000nxs-main', '-bios', str(firmware / 'main-firmware.bin'),
        '-display', 'none', '-no-reboot', '-d', 'unimp,guest_errors', '-D', str(run / 'main.log'),
        '-serial', f'tcp:127.0.0.1:{args.port},server,nowait',
        '-serial', f'tcp:127.0.0.1:{args.port + 2},server,nowait', '-serial', 'null']
    gui_command = [str(simulator), '--model', 'bf531', '--environment', 'operating', '--memory-region', '0,64M',
        '--hw-board-file', 'emulator/cdj2000-gui-nxs.hw', str(firmware / 'gui-boot-memory.elf')]
    overrides = dict(BFIN_PARALLEL_WRITEBACK='1', BFIN_GUI_COLOR='rgb555le',
        BFIN_GUI_OUTPUT=str(run / 'screen.ppm'), BFIN_MAIN_LINK=f'127.0.0.1:{args.port}',
        BFIN_MAIN_LINK_DUMP=str(run / 'main-link.bin'), BFIN_GPIO5_READY_TOGGLE='1',
        BFIN_STATS='5', BFIN_EXCEPTION_TRACE='1', BFIN_EXIT_AFTER_WALL=str(args.seconds))
    # Do not inherit replay/proxy data or a firmware shortcut from the shell.
    gui_env = {k:v for k,v in os.environ.items() if not k.startswith('BFIN_')}
    gui_env.update(overrides)
    main_env = {k:v for k,v in os.environ.items() if not k.startswith('CDJ_')}
    main_env['CDJ_INPUT_PORT'] = str(args.port + 4)
    main_env['CDJ_NXS_HPI_DUMP'] = str(run / 'dsp-l2.bin')
    main_env['CDJ_NXS_DSP_EVENTS'] = str(run / 'dsp-events.jsonl')
    main_env['CDJ_NXS_DSP_CHECKPOINT_DIR'] = str(run / 'dsp-checkpoints')
    main_env['CDJ_REQ_STATUS_FRESH'] = '0'
    (run / 'run.json').write_text(json.dumps(dict(main=main_command, gui=gui_command,
        gui_environment=overrides, main_environment={k:v for k,v in main_env.items() if k.startswith('CDJ_')},
        dsp='NXS UHPI plus partial C674x interpreter; incomplete ISA, ROM handoff abstraction', profile='experimental NXS'), indent=2) + '\n')
    processes = []
    result = {}
    with (run / 'main-stderr.log').open('w') as mainlog, (run / 'gui.log').open('w') as guilog:
        try:
            main_process = subprocess.Popen(main_command, cwd=ROOT, env=main_env, stdin=subprocess.DEVNULL, stdout=mainlog, stderr=mainlog)
            processes.append(main_process)
            time.sleep(1)
            if main_process.poll() is not None: raise RuntimeError('MAIN exited; see main-stderr.log')
            gui = subprocess.Popen(gui_command, cwd=ROOT, env=gui_env, stdin=subprocess.DEVNULL, stdout=guilog, stderr=guilog)
            processes.append(gui)
            print(f'MAIN {main_process.pid}, GUI {gui.pid}; logs: {run}', flush=True)
            deadline = time.monotonic() + args.seconds + 5
            while gui.poll() is None and time.monotonic() < deadline:
                if main_process.poll() is not None: raise RuntimeError('MAIN exited during run')
                time.sleep(.1)
            result = dict(gui_exit=gui.poll(), timed_out=gui.poll() is None, frame_exists=(run / 'screen.ppm').exists())
            print(json.dumps(result), flush=True)
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                    try: process.wait(timeout=5)
                    except subprocess.TimeoutExpired: process.kill(); process.wait(timeout=5)
            finalize_dsp_artifacts(run, firmware)
            (run / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    return 0 if result.get('gui_exit') == 0 and result.get('frame_exists') else 1


if __name__ == '__main__':
    raise SystemExit(main())
